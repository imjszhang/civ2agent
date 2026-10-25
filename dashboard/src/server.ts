import { createServer, type ServerResponse } from "node:http";
import { readFile } from "node:fs/promises";
import path from "node:path";
import { fileURLToPath } from "node:url";
import { readGame } from "./game.js";
import { commandFromLog, earlierUsage, freshTail, latestEvents, piecesFromRuntime, readActive, readAppended, readPlanBrief, readSession, type Tail } from "./follow.js";
import { foldUsage, mergeRun, runChanged } from "./project.js";
import type { CommandView, GameView, Piece, RunView, SessionView, Snapshot, ThoughtView } from "./types.js";

const HERE = path.dirname(fileURLToPath(import.meta.url));
const DASHBOARD = path.dirname(HERE);
const AGENT = path.dirname(DASHBOARD);
const WEB = path.join(DASHBOARD, "web");
const CTL = path.join(AGENT, "cli", "civ2ctl.py");
const python = process.env.CIV2AGENT_PYTHON || "python";
const gamesDir = process.env.CIV2AGENT_GAMES_DIR || path.join(AGENT, "games");
const port = Number(process.env.DASHBOARD_PORT || 4732);
const host = "127.0.0.1";

const emptyGame = (): GameView => ({
  connected: false, busy: false, reason: "还没有读到游戏", inGame: false, turn: null, year: null,
  civ: null, cities: [], units: [], diplomacy: [], dialog: null, cityWindow: null, taxWindow: null,
});

const snapshot: Snapshot = {
  game: emptyGame(), session: null, run: null, earlier: null, prompt: "", thoughts: [], actions: [], commands: [],
};

const clients = new Set<ServerResponse>();
let sessionDir: string | null = null;
let eventsTail: Tail = freshTail();
let logTail: Tail = freshTail();
let plannedFile: string | null = null;

function send(res: ServerResponse, event: string, data: unknown) {
  res.write(`event: ${event}\ndata: ${JSON.stringify(data)}\n\n`);
}

function broadcast(event: string, data: unknown) {
  for (const client of clients) send(client, event, data);
}

function apply(piece: Piece) {
  if (piece.event === "thought") {
    const last = snapshot.thoughts.at(-1);
    if (piece.data.done) {
      if (last) last.done = true;
    } else if (last && !last.done) last.text += piece.data.text;
    else snapshot.thoughts.push({ text: piece.data.text, done: false });
  } else if (piece.event === "action") snapshot.actions.push(piece.data);
  else if (piece.event === "prompt" && !snapshot.prompt) snapshot.prompt = piece.data.text;
  else if (piece.event === "command") snapshot.commands.push(piece.data);
  else if (piece.event === "usage") {
    if (!snapshot.run) return;
    const usage = foldUsage(snapshot.run.adapter, snapshot.run.usage, piece.data);
    if (!runChanged(snapshot.run, { ...snapshot.run, usage })) return;
    snapshot.run = { ...snapshot.run, usage };
    broadcast("run", snapshot.run);
    return;
  } else if (piece.event === "run") {
    const next = mergeRun(snapshot.run, piece.data);
    if (!runChanged(snapshot.run, next)) return;
    snapshot.run = next;
    broadcast("run", snapshot.run);
    return;
  }
  broadcast(piece.event, piece.data);
}

function clearAgent() {
  snapshot.run = null;
  snapshot.prompt = "";
  snapshot.thoughts = [];
  snapshot.actions = [];
  broadcast("reset", { scope: "agent" });
}

function clearSession() {
  snapshot.earlier = null;
  broadcast("earlier", null);
  clearAgent();
  snapshot.commands = [];
  snapshot.session = null;
  broadcast("reset", { scope: "session" });
  broadcast("session", null);
}

async function followSession() {
  const lock = await readActive(gamesDir);
  const dir = lock?.dir ?? null;
  if (dir !== sessionDir) {
    sessionDir = dir;
    eventsTail = freshTail();
    logTail = freshTail();
    plannedFile = null;
    clearSession();
    if (lock && dir) {
      snapshot.session = await readSession(dir, lock);
      logTail.path = path.join(dir, "log.jsonl");
      broadcast("session", snapshot.session);
    }
  }
  if (!sessionDir) return;
  const events = await latestEvents(sessionDir);
  if (events !== eventsTail.path) {
    snapshot.earlier = await earlierUsage(sessionDir, events);
    broadcast("earlier", snapshot.earlier);
    eventsTail = { path: events, offset: 0, rest: "" };
    plannedFile = null;
    clearAgent();
  }
  for (const line of await readAppended(logTail)) {
    const command = commandFromLog(line);
    if (command) apply({ event: "command", data: command });
  }
  for (const line of await readAppended(eventsTail)) {
    for (const piece of piecesFromRuntime(line)) apply(piece);
  }
  if (events && plannedFile !== events) {
    plannedFile = events;
    const plan = await readPlanBrief(events);
    if (plan) {
      apply({
        event: "run",
        data: {
          runId: snapshot.run?.runId ?? "",
          ...(snapshot.run?.adapter ? {} : { adapter: plan.adapter }),
          ...(snapshot.run?.target ? {} : { target: plan.target }),
          requestedModel: plan.requestedModel,
          ...(snapshot.run?.effort ? {} : { effort: plan.effort }),
        },
      });
    }
  }
}

async function followGame() {
  const next = await readGame(python, CTL, snapshot.game.connected ? snapshot.game : null);
  snapshot.game = next;
  broadcast("game", next);
}

function sleep(ms: number) {
  return new Promise(resolve => setTimeout(resolve, ms));
}

async function loops() {
  let lastGame = 0;
  for (;;) {
    const now = Date.now();
    try {
      await followSession();
      if (now - lastGame >= 1000) { lastGame = now; await followGame(); }
    } catch (cause) {
      snapshot.game = { ...snapshot.game, busy: true, reason: cause instanceof Error ? cause.message : "观察中断" };
      broadcast("game", snapshot.game);
    }
    await sleep(400);
  }
}

const types: Record<string, string> = { ".html": "text/html; charset=utf-8", ".js": "text/javascript; charset=utf-8", ".css": "text/css; charset=utf-8" };

const server = createServer(async (req, res) => {
  const url = new URL(req.url ?? "/", `http://${host}`);
  if (url.pathname === "/events") {
    res.writeHead(200, {
      "content-type": "text/event-stream; charset=utf-8",
      "cache-control": "no-cache",
      connection: "keep-alive",
    });
    res.write(": connected\n\n");
    send(res, "snapshot", snapshot);
    clients.add(res);
    const beat = setInterval(() => res.write(": ping\n\n"), 15000);
    req.on("close", () => { clearInterval(beat); clients.delete(res); });
    return;
  }
  const name = url.pathname === "/" ? "index.html" : path.basename(url.pathname);
  try {
    const body = await readFile(path.join(WEB, name));
    res.writeHead(200, { "content-type": types[path.extname(name)] ?? "application/octet-stream", "cache-control": "no-cache" });
    res.end(body);
  } catch {
    res.writeHead(404, { "content-type": "text/plain; charset=utf-8" });
    res.end("没有这个页面");
  }
});

server.listen(port, host, () => {
  console.log(`civ2agent dashboard http://${host}:${port}`);
});
loops();

export type { CommandView, RunView, SessionView, ThoughtView };
