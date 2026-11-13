import { spawn, type ChildProcess } from "node:child_process";
import { createServer, type IncomingMessage, type Server, type ServerResponse } from "node:http";
import { readFile, mkdtemp, writeFile, rm } from "node:fs/promises";
import { tmpdir } from "node:os";
import path from "node:path";
import { fileURLToPath } from "node:url";
import { runCli } from "./cli.js";
import { readGame } from "./game.js";
import { commandFromLog, earlierUsage, freshTail, latestEvents, piecesFromRuntime, readActive, readAppended, readPlanBrief, readSession, type Tail } from "./follow.js";
import { Gate, runMutation, whenIdle } from "./gate.js";
import { listenLines } from "./lan.js";
import { clearLease, writeLease } from "./lease.js";
import { LEASE_COOKIE, PAGE_COOKIE, bindAllowed, cookieValue, createMcpHandler, pageAllowed, setCookie, type McpDeps } from "./mcp.js";
import { actTimeoutMs, callArgs, loadPlayTools } from "./play.js";
import { foldUsage, mergeRun, runChanged } from "./project.js";
import { retainActions, retainCommands, retainThoughts } from "./retain.js";
import { CHECKPOINT_MS, openGame, runSession, sessionEnv } from "./session.js";
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
  /** civ2-play.mjs. Tests pass a short script so a run does not start the game. */
  runner?: string;
};

export type RunningDashboard = {
  port: number;
  url: string;
  close: () => Promise<void>;
};

const emptyGame = (): GameView => ({
  connected: false, busy: false, reason: "还没有读到游戏", inGame: false, turn: null, year: null,
  civ: null, cities: [], units: [], foreignUnits: [], foreignCities: [], tiles: [],
  diplomacy: [], dialog: null, cityWindow: null, taxWindow: null,
  activeUnit: null, moveUnit: null, notice: null,
});

const types: Record<string, string> = {
  ".html": "text/html; charset=utf-8",
  ".js": "text/javascript; charset=utf-8",
  ".css": "text/css; charset=utf-8",
};

const PAGE_TOOLS = new Set(["respond", "end-turn", "move", "produce", "rates", "checkpoint"]);

function count(value: unknown, max: number): number | null {
  if (value === undefined) return null;
  const n = typeof value === "number" ? value : Number(value);
  if (!Number.isInteger(n) || n < 1 || n > max) return null;
  return n;
}

function sleep(ms: number) {
  return new Promise(resolve => setTimeout(resolve, ms));
}

function header(req: IncomingMessage, name: string): string | undefined {
  const value = req.headers[name];
  return Array.isArray(value) ? value[0] : value;
}

function readRaw(req: IncomingMessage): Promise<string> {
  return new Promise((resolve, reject) => {
    const chunks: Buffer[] = [];
    let size = 0;
    req.on("data", (chunk: Buffer) => {
      size += chunk.length;
      if (size > 1_000_000) { reject(new Error("请求太大")); req.destroy(); return; }
      chunks.push(chunk);
    });
    req.on("end", () => resolve(Buffer.concat(chunks).toString("utf8")));
    req.on("error", reject);
  });
}

function loginToken(type: string | undefined, text: string): string {
  if ((type ?? "").includes("application/json")) {
    try {
      const value = JSON.parse(text) as { token?: unknown };
      return typeof value.token === "string" ? value.token : "";
    } catch { return ""; }
  }
  return new URLSearchParams(text).get("token") ?? "";
}

function loginPage(wrong: boolean): string {
  return `<!DOCTYPE html>
<html lang="zh-CN">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>civ2agent 登录</title>
<link rel="stylesheet" href="app.css">
</head>
<body>
<header><h1>civ2agent</h1></header>
<main>
<form method="post" action="/login" class="panel">
<p>${wrong ? "口令不对。" : "输入令牌后才能看局面和操作。"}</p>
<label>令牌 <input type="password" name="token" autofocus></label>
<button type="submit">进入</button>
</form>
</main>
</body>
</html>`;
}

export async function startDashboard(options: DashboardOptions): Promise<RunningDashboard> {
  if (!bindAllowed(options.host, options.token)) {
    throw new Error("监听局域网地址时必须设置 DASHBOARD_TOKEN");
  }
  const lan = options.host !== "127.0.0.1" && options.host !== "::1" && options.host !== "localhost";
  const playTools = await loadPlayTools(options.python, options.ctl, options.gamesDir);
  if (playTools.length === 0) console.error("没有读到 civ2ctl 工具表，代打工具未注册");

  const gate = new Gate(options.idleMs, Date.now, holder => {
    if (holder) writeLease(options.gamesDir, { pid: process.pid, lease: holder.lease, client: holder.client });
    else clearLease(options.gamesDir);
  });
  const snapshot: Snapshot = {
    game: emptyGame(), session: null, gateway: { lan, holder: null, command: null, since: null, last: null, playing: null },
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

  let playing = "";
  let runToken = 0;
  let runner: ChildProcess | null = null;
  const runnerScript = options.runner ?? path.join(AGENT, "runner", "civ2-play.mjs");

  function publish() {
    const view = gate.view();
    const next: GatewayView = { lan, ...view, playing: playing || null };
    if (JSON.stringify(next) === JSON.stringify(snapshot.gateway)) return;
    snapshot.gateway = next;
    broadcast("gateway", next);
  }

  function stopRunner() {
    runToken += 1;
    const child = runner;
    runner = null;
    playing = "";
    if (child?.pid && process.platform === "win32") {
      spawn("taskkill", ["/PID", String(child.pid), "/T", "/F"], { windowsHide: true, stdio: "ignore" });
    } else child?.kill();
  }

  function runOnce(args: string[], env: NodeJS.ProcessEnv, token: number): Promise<number> {
    return new Promise(resolve => {
      if (token !== runToken) { resolve(1); return; }
      const child = spawn(process.execPath, args, { windowsHide: true, env, stdio: "ignore" });
      runner = child;
      const finish = (code: number) => {
        if (runner === child) runner = null;
        resolve(code);
      };
      child.on("error", () => finish(1));
      child.on("close", code => finish(code ?? 1));
    });
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
      () => readGame(
        options.python, options.ctl, snapshot.game.connected ? snapshot.game : null,
        sessionEnv(options.gamesDir, gate.leaseId()),
      ),
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

  const allowed = (req: IncomingMessage) =>
    pageAllowed(header(req, "authorization"), header(req, "cookie"), options.token);

  function sendJson(res: ServerResponse, status: number, body: unknown, extra?: Record<string, string | string[]>) {
    if (res.headersSent) return;
    res.writeHead(status, { "content-type": "application/json; charset=utf-8", ...extra });
    res.end(JSON.stringify(body));
  }

  function sendHtml(res: ServerResponse, status: number, html: string, extra?: Record<string, string | string[]>) {
    res.writeHead(status, { "content-type": "text/html; charset=utf-8", ...extra });
    res.end(html);
  }

  function deny(req: IncomingMessage, res: ServerResponse, page: boolean): boolean {
    if (allowed(req)) return false;
    if (page) {
      res.writeHead(302, { location: "/login" });
      res.end();
    } else sendJson(res, 401, { ok: false, error: "unauthorized", reason: "先登录" });
    return true;
  }

  async function handlePlay(req: IncomingMessage, res: ServerResponse) {
    if (req.method === "GET") {
      const held = cookieValue(header(req, "cookie"), LEASE_COOKIE);
      const current = gate.leaseId();
      sendJson(res, 200, { ok: true, holding: Boolean(held && current && held === current) });
      return;
    }
    if (req.method !== "POST") {
      sendJson(res, 405, { ok: false, error: "usage", reason: "用 POST" });
      return;
    }
    let body: Record<string, unknown>;
    try {
      const text = await readRaw(req);
      const parsed: unknown = text ? JSON.parse(text) : {};
      if (typeof parsed !== "object" || parsed === null || Array.isArray(parsed)) throw new Error("请求不是对象");
      body = parsed as Record<string, unknown>;
    } catch (cause) {
      sendJson(res, 400, { ok: false, error: "usage", reason: cause instanceof Error ? cause.message : "请求不是 JSON" });
      return;
    }
    if (body.op === "claim") {
      const client = typeof body.client === "string" && body.client.trim() ? body.client : "观察页";
      const result = gate.claim(client);
      publish();
      const extra = result.ok === true && typeof result.lease === "string"
        ? { "set-cookie": setCookie(LEASE_COOKIE, result.lease) } : undefined;
      sendJson(res, 200, { ...result, holding: result.ok === true }, extra);
      return;
    }
    const fromBody = typeof body.lease === "string" ? body.lease : "";
    const lease = fromBody || cookieValue(header(req, "cookie"), LEASE_COOKIE);
    if (body.op === "release" || body.op === "stop") {
      stopRunner();
      const result = gate.release(lease);
      publish();
      const extra = result.ok === true ? { "set-cookie": setCookie(LEASE_COOKIE, "") } : undefined;
      sendJson(res, 200, { ...result, holding: result.ok !== true, playing: null }, extra);
      return;
    }
    if (body.op === "run") {
      if (runner) {
        sendJson(res, 200, { ok: false, error: "busy", reason: playing || "已经在玩", holding: true, playing });
        return;
      }
      const request = typeof body.request === "string" ? body.request.trim() : "";
      const resume = typeof body.resume === "string" ? body.resume.trim() : "";
      const guide = typeof body.guide === "string" ? body.guide.trim() : "";
      const rule = typeof body.rule === "string" ? body.rule.trim() : "";
      const games = count(body.games, 20) ?? (body.games === undefined ? 1 : null);
      const turns = count(body.turns, 500) ?? (body.turns === undefined ? 10 : null);
      if (!resume && !request) {
        sendJson(res, 400, { ok: false, error: "usage", reason: "先写要怎么开局" });
        return;
      }
      if (guide && !/^[a-z0-9-]{1,40}$/u.test(guide)) {
        sendJson(res, 400, { ok: false, error: "usage", reason: "攻略名字只能是注册表里的英文标识" });
        return;
      }
      if (rule && !/^[a-z0-9-]{1,40}$/u.test(rule)) {
        sendJson(res, 400, { ok: false, error: "usage", reason: "法则名字只能是注册表里的英文标识，或 none" });
        return;
      }
      if (games === null || turns === null) {
        sendJson(res, 400, { ok: false, error: "usage", reason: "局数是 1 到 20，每局回合是 1 到 500" });
        return;
      }
      if (resume && openGame(options.gamesDir, resume).ok !== true) {
        sendJson(res, 400, { ok: false, error: "usage", reason: "没有这一局" });
        return;
      }
      let owned = lease;
      let cookie: string | undefined;
      if (!owned || gate.leaseId() !== owned) {
        const claimed = gate.claim("观察页");
        if (claimed.ok !== true || typeof claimed.lease !== "string") {
          sendJson(res, 200, { ...claimed, holding: false });
          return;
        }
        owned = claimed.lease;
        cookie = setCookie(LEASE_COOKIE, owned);
      }
      const total = resume ? 1 : games;
      const argsFor = (index: number): string[] | { ok: false; error: "usage"; reason: string } => {
        const args = [runnerScript];
        if (resume) {
          const found = openGame(options.gamesDir, resume);
          if (found.ok !== true) return found;
          args.push("resume", found.dir, "--turns", String(turns));
        } else {
          args.push("new", "--request", request, "--turns", String(turns), "--title", `第 ${index} 局`);
          if (rule) args.push("--rule", rule);
        }
        if (guide) args.push("--guide", guide);
        return args;
      };
      const first = argsFor(1);
      if (!Array.isArray(first)) {
        sendJson(res, 400, first);
        return;
      }
      const token = runToken;
      playing = `正在玩第 1/${total} 局，每局 ${turns} 回合`;
      publish();
      const childEnv = sessionEnv(options.gamesDir, owned);
      void (async () => {
        let note = "";
        for (let index = 1; index <= total && token === runToken; index += 1) {
          const args = argsFor(index);
          if (!Array.isArray(args)) { note = args.reason; break; }
          playing = `正在玩第 ${index}/${total} 局，每局 ${turns} 回合`;
          publish();
          const code = await runOnce(args, childEnv, token);
          if (token !== runToken) return;
          if (code !== 0) { note = `第 ${index} 局没有完成`; break; }
        }
        if (token !== runToken) return;
        playing = note;
        gate.release(owned);
        publish();
      })();
      sendJson(res, 200, { ok: true, holding: true, playing, games: total, turns }, cookie ? { "set-cookie": cookie } : undefined);
      return;
    }
    const tool = body.tool;
    if (body.op !== "act" || typeof tool !== "string" || !PAGE_TOOLS.has(tool)) {
      sendJson(res, 400, { ok: false, error: "usage", reason: "页面只能回答对话框、结束回合、存盘、移动、改产和设定税率" });
      return;
    }
    const args = body.args && typeof body.args === "object" && !Array.isArray(body.args)
      ? body.args as Record<string, unknown> : {};
    const result = await runMutation(gate, tool, lease, async () => {
      const child = gate.leaseId();
      if (tool === "checkpoint") {
        return runSession(options.python, options.session, options.gamesDir, ["checkpoint"], CHECKPOINT_MS, child);
      }
      return runCli(options.python, options.ctl, callArgs(tool, args, 10), actTimeoutMs(args), sessionEnv(options.gamesDir, child));
    }, publish);
    sendJson(res, 200, { ...result, holding: Boolean(lease) && gate.leaseId() === lease });
  }

  async function handleGameRule(req: IncomingMessage, res: ServerResponse, url: URL) {
    const active = await readActive(options.gamesDir);
    if (!active) {
      sendJson(res, 400, { ok: false, error: "usage", reason: "没有正在进行的局" });
      return;
    }
    const lease = gate.leaseId();
    if (req.method === "GET") {
      const version = url.searchParams.get("version");
      const args = ["rule", active.dir];
      if (version) args.push("--version", version);
      const result = await runSession(options.python, options.session, options.gamesDir, args, 8000, lease);
      sendJson(res, result.ok === true ? 200 : 400, result);
      return;
    }
    if (req.method !== "POST") {
      sendJson(res, 405, { ok: false, error: "usage", reason: "用 POST" });
      return;
    }
    let body: Record<string, unknown>;
    try {
      const text = await readRaw(req);
      const parsed: unknown = text ? JSON.parse(text) : {};
      if (typeof parsed !== "object" || parsed === null || Array.isArray(parsed)) throw new Error("请求不是对象");
      body = parsed as Record<string, unknown>;
    } catch (cause) {
      sendJson(res, 400, { ok: false, error: "usage", reason: cause instanceof Error ? cause.message : "请求不是 JSON" });
      return;
    }
    if (typeof body.use === "string") {
      const result = await runSession(options.python, options.session, options.gamesDir, ["rule", active.dir, "--use", body.use], 8000, lease);
      sendJson(res, result.ok === true ? 200 : 400, result);
      return;
    }
    if (typeof body.text !== "string") {
      sendJson(res, 400, { ok: false, error: "usage", reason: "要保存法则的全文，或用 use 换一套" });
      return;
    }
    const folder = await mkdtemp(path.join(tmpdir(), "civ2-rule-"));
    const file = path.join(folder, "rule.md");
    try {
      await writeFile(file, body.text, "utf8");
      const result = await runSession(options.python, options.session, options.gamesDir, ["rule", active.dir, "--file", file], 8000, lease);
      sendJson(res, result.ok === true ? 200 : 400, result);
    } finally {
      await rm(folder, { recursive: true, force: true });
    }
  }

  const server: Server = createServer(async (req, res) => {
    const url = new URL(req.url ?? "/", `http://${options.host}`);
    if (url.pathname === "/mcp") { await mcp.handle(req, res); return; }
    if (url.pathname === "/login") {
      if (!options.token || (req.method === "GET" && allowed(req))) {
        res.writeHead(302, { location: "/" });
        res.end();
        return;
      }
      if (req.method === "GET") { sendHtml(res, 200, loginPage(false)); return; }
      if (req.method !== "POST") { sendJson(res, 405, { ok: false, error: "usage", reason: "用 POST" }); return; }
      const token = loginToken(header(req, "content-type"), await readRaw(req));
      if (!pageAllowed(`Bearer ${token}`, undefined, options.token)) {
        sendHtml(res, 401, loginPage(true));
        return;
      }
      sendHtml(res, 302, "", { location: "/", "set-cookie": setCookie(PAGE_COOKIE, options.token) });
      return;
    }
    if (url.pathname === "/logout" && req.method === "POST") {
      const lease = cookieValue(header(req, "cookie"), LEASE_COOKIE);
      if (lease) gate.release(lease);
      const next = options.token ? "/login" : "/";
      res.writeHead(302, {
        location: next,
        "set-cookie": [setCookie(PAGE_COOKIE, ""), setCookie(LEASE_COOKIE, "")],
      });
      res.end();
      return;
    }
    if (url.pathname === "/events") {
      if (deny(req, res, false)) return;
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
    if (url.pathname === "/play") {
      if (deny(req, res, false)) return;
      await handlePlay(req, res);
      return;
    }
    if (url.pathname === "/rules" && req.method === "GET") {
      if (deny(req, res, false)) return;
      const listed = await runSession(options.python, options.session, options.gamesDir, ["rules"], 8000, gate.leaseId());
      sendJson(res, listed.ok === true ? 200 : 400, listed);
      return;
    }
    if (url.pathname === "/rule") {
      if (deny(req, res, false)) return;
      await handleGameRule(req, res, url);
      return;
    }
    const name = url.pathname === "/" ? "index.html" : path.basename(url.pathname);
    const pub = name === "app.js" || name === "app.css";
    if (!pub && deny(req, res, true)) return;
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
      stopRunner();
      clearLease(options.gamesDir);
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
    for (const line of listenLines(options.host, running.port)) console.log(line);
  }).catch(error => {
    console.error(error instanceof Error ? error.message : error);
    process.exit(1);
  });
}

export type { CommandView, RunView, SessionView, ThoughtView } from "./types.js";
