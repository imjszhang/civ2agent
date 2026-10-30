import { createServer, type Server, type ServerResponse } from "node:http";
import { readFile } from "node:fs/promises";
import path from "node:path";
import { fileURLToPath } from "node:url";
import { readGame } from "./game.js";
import { commandFromLog, earlierUsage, freshTail, latestEvents, piecesFromRuntime, readActive, readAppended, readPlanBrief, readSession, type Tail } from "./follow.js";
import { Gate, whenIdle } from "./gate.js";
import { bindAllowed, createMcpHandler, type McpDeps } from "./mcp.js";
import { loadPlayTools } from "./play.js";
import { foldUsage, mergeRun, runChanged } from "./project.js";
import { retainActions, retainCommands, retainThoughts } from "./retain.js";
import type { GameView, GatewayView, Piece, Snapshot } from "./types.js";

const HERE = path.dirname(fileURLToPath(import.meta.url));
const DASHBOARD = path.dirname(HERE);
const AGENT = path.dirname(DASHBOARD);

export type DashboardOptions = {
  host: string;
  port: number;
  token: string;
  python: string;
  ctl: string;
  session: string;
  gamesDir: string;
  webDir: string;
  idleMs: number;
};

export type RunningDashboard = {
  port: number;
  url: string;
  close: () => Promise<void>;
};

const emptyGame = (): GameView => ({
  connected: false, busy: false, reason: "还没有读到游戏", inGame: false, turn: null, year: null,
  civ: null, cities: [], units: [], diplomacy: [], dialog: null, cityWindow: null, taxWindow: null,
});

const types: Record<string, string> = {
  ".html": "text/html; charset=utf-8",
  ".js": "text/javascript; charset=utf-8",
  ".css": "text/css; charset=utf-8",
};

function sleep(ms: number) {
  return new Promise(resolve => setTimeout(resolve, ms));
}

export async function startDashboard(options: DashboardOptions): Promise<RunningDashboard> {
  if (!bindAllowed(options.host, options.token)) {
    throw new Error("监听局域网地址时必须设置 DASHBOARD_TOKEN");
  }
  const lan = options.host !== "127.0.0.1" && options.host !== "::1" && options.host !== "localhost";
  const playTools = await loadPlayTools(options.python, options.ctl, options.gamesDir);
  if (playTools.length === 0) console.error("没有读到 civ2ctl 工具表，代打工具未注册");

  const gate = new Gate(options.idleMs);
  const snapshot: Snapshot = {
    game: emptyGame(), session: null, gateway: { lan, holder: null, command: null, since: null, last: null },
    run: null, earlier: null, prompt: "", thoughts: [], actions: [], commands: [],
  };
  const clients = new Set<ServerResponse>();
  let sessionDir: string | null = null;
  let eventsTail: Tail = freshTail();
  let logTail: Tail = freshTail();
  let plannedFile: string | null = null;
  let stopped = false;

  function send(res: ServerResponse, event: string, data: unknown) {
    res.write(`event: ${event}\ndata: ${JSON.stringify(data)}\n\n`);
  }

  function broadcast(event: string, data: unknown) {
    for (const client of clients) send(client, event, data);
  }

  function publish() {
    const view = gate.view();
    const next: GatewayView = { lan, ...view };
    if (JSON.stringify(next) === JSON.stringify(snapshot.gateway)) return;
    snapshot.gateway = next;
    broadcast("gateway", next);
  }

  function apply(piece: Piece) {
    if (piece.event === "thought") {
      const last = snapshot.thoughts.at(-1);
      if (piece.data.done) {
        if (last) last.done = true;
      } else if (last && !last.done) last.text += piece.data.text;
      else snapshot.thoughts.push({ text: piece.data.text, done: false });
      retainThoughts(snapshot.thoughts);
    } else if (piece.event === "action") {
      snapshot.actions.push(piece.data);
      retainActions(snapshot.actions);
    } else if (piece.event === "prompt" && !snapshot.prompt) snapshot.prompt = piece.data.text;
    else if (piece.event === "command") {
      snapshot.commands.push(piece.data);
      retainCommands(snapshot.commands);
    }
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
    const lock = await readActive(options.gamesDir);
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

  function showGame(next: GameView) {
    if (JSON.stringify(next) === JSON.stringify(snapshot.game)) return;
    snapshot.game = next;
    broadcast("game", next);
  }

  async function followGame() {
    const next = await whenIdle(
      gate,
      () => readGame(options.python, options.ctl, snapshot.game.connected ? snapshot.game : null),
      reason => ({ ...snapshot.game, busy: true, reason }),
    );
    showGame(next);
  }

  async function loops() {
    let lastGame = 0;
    while (!stopped) {
      const now = Date.now();
      try {
        publish();
        await followSession();
        if (now - lastGame >= 1000) { lastGame = now; await followGame(); }
      } catch (cause) {
        showGame({ ...snapshot.game, busy: true, reason: cause instanceof Error ? cause.message : "观察中断" });
      }
      await sleep(400);
    }
  }

  const deps: McpDeps = {
    python: options.python,
    ctl: options.ctl,
    session: options.session,
    gamesDir: options.gamesDir,
    gate,
    playTools,
    readSnapshot: () => snapshot.game,
    onActivity: publish,
  };
  const mcp = createMcpHandler(deps, options.token);

  const server: Server = createServer(async (req, res) => {
    const url = new URL(req.url ?? "/", `http://${options.host}`);
    if (url.pathname === "/mcp") { await mcp.handle(req, res); return; }
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
      const body = await readFile(path.join(options.webDir, name));
      res.writeHead(200, { "content-type": types[path.extname(name)] ?? "application/octet-stream", "cache-control": "no-cache" });
      res.end(body);
    } catch {
      res.writeHead(404, { "content-type": "text/plain; charset=utf-8" });
      res.end("没有这个页面");
    }
  });

  await new Promise<void>((resolve, reject) => {
    server.once("error", reject);
    server.listen(options.port, options.host, () => resolve());
  });
  const address = server.address();
  const bound = typeof address === "object" && address ? address.port : options.port;
  const pageHost = options.host === "0.0.0.0" || options.host === "::" ? "127.0.0.1" : options.host;
  loops();

  return {
    port: bound,
    url: `http://${pageHost}:${bound}`,
    async close() {
      stopped = true;
      for (const client of clients) client.end();
      clients.clear();
      await mcp.close();
      await new Promise<void>(resolve => server.close(() => resolve()));
    },
  };
}

function optionsFromEnv(): DashboardOptions {
  return {
    host: process.env.DASHBOARD_HOST || "127.0.0.1",
    port: Number(process.env.DASHBOARD_PORT || 4732),
    token: process.env.DASHBOARD_TOKEN || "",
    python: process.env.CIV2AGENT_PYTHON || "python",
    ctl: path.join(AGENT, "cli", "civ2ctl.py"),
    session: path.join(AGENT, "cli", "civ2session.py"),
    gamesDir: process.env.CIV2AGENT_GAMES_DIR || path.join(AGENT, "games"),
    webDir: path.join(DASHBOARD, "web"),
    idleMs: Number(process.env.GATE_LEASE_IDLE_MS || 600_000),
  };
}

function invokedDirectly(): boolean {
  const entry = process.argv[1];
  if (!entry) return false;
  const invoked = path.resolve(entry);
  const self = fileURLToPath(import.meta.url);
  return process.platform === "win32" ? invoked.toLowerCase() === self.toLowerCase() : invoked === self;
}

if (invokedDirectly()) {
  const options = optionsFromEnv();
  startDashboard(options).then(running => {
    console.log(`civ2agent dashboard ${running.url}  mcp /mcp`);
  }).catch(error => {
    console.error(error instanceof Error ? error.message : error);
    process.exit(1);
  });
}

export type { CommandView, RunView, SessionView, ThoughtView } from "./types.js";
