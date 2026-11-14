import { timingSafeEqual, randomUUID } from "node:crypto";
import type { IncomingMessage, ServerResponse } from "node:http";
import { McpServer } from "@modelcontextprotocol/sdk/server/mcp.js";
import { StreamableHTTPServerTransport } from "@modelcontextprotocol/sdk/server/streamableHttp.js";
import type { RequestHandlerExtra } from "@modelcontextprotocol/sdk/shared/protocol.js";
import type { ServerNotification, ServerRequest } from "@modelcontextprotocol/sdk/types.js";
import * as z from "zod/v4";
import type { CliResult } from "./cli.js";
import { runCli } from "./cli.js";
import { Gate, runMutation, whenIdle } from "./gate.js";
import type { CtlTool, SchemaProp } from "./play.js";
import { actTimeoutMs, callArgs, needsLease, toolDescription, visiblePlayTools } from "./play.js";
import { packGame } from "./pack.js";
import { readRecord } from "./record.js";
import { CHECKPOINT_MS, SESSION_MS, closeArgs, openGame, resolveGameDir, runSession, sessionEnv, startArgs } from "./session.js";
import type { GameView } from "./types.js";

type Extra = RequestHandlerExtra<ServerRequest, ServerNotification>;

export type McpDeps = {
  python: string;
  ctl: string;
  session: string;
  gamesDir: string;
  gate: Gate;
  playTools: CtlTool[];
  readSnapshot: () => GameView;
  onActivity: () => void;
  /** True when no game process is up and nothing is launching one. */
  gameDown: () => boolean;
};

export function bindAllowed(host: string, token: string): boolean {
  return host === "127.0.0.1" || host === "::1" || host === "localhost" || token.length > 0;
}

export function bearerOk(authorization: string | undefined, token: string): boolean {
  if (!token) return true;
  const given = authorization?.startsWith("Bearer ") ? authorization.slice("Bearer ".length) : "";
  return secretOk(given, token);
}

export const PAGE_COOKIE = "civ2agent";
export const LEASE_COOKIE = "civ2agent_lease";
/** How long the browser keeps a successful page login. */
export const PAGE_COOKIE_MAX_AGE = 180 * 24 * 60 * 60;

export function cookieValue(header: string | undefined, name: string): string {
  if (!header) return "";
  for (const part of header.split(";")) {
    const eq = part.indexOf("=");
    if (eq < 0) continue;
    if (part.slice(0, eq).trim() !== name) continue;
    try { return decodeURIComponent(part.slice(eq + 1).trim()); }
    catch { return ""; }
  }
  return "";
}

function secretOk(given: string, token: string): boolean {
  const left = Buffer.from(given);
  const right = Buffer.from(token);
  return left.length === right.length && timingSafeEqual(left, right);
}

/** A page, event stream or /play call. Bearer or the login cookie. No token means loopback. */
export function pageAllowed(authorization: string | undefined, cookie: string | undefined, token: string): boolean {
  if (!token) return true;
  return bearerOk(authorization, token) || secretOk(cookieValue(cookie, PAGE_COOKIE), token);
}

export function setCookie(name: string, value: string, maxAge?: number): string {
  const body = value ? `${name}=${encodeURIComponent(value)}` : `${name}=`;
  const lifetime = value ? (maxAge === undefined ? "" : `; Max-Age=${maxAge}`) : "; Max-Age=0";
  return `${body}; HttpOnly; SameSite=Lax; Path=/${lifetime}`;
}

function asTool(value: CliResult) {
  return { content: [{ type: "text" as const, text: JSON.stringify(value) }], isError: value.ok !== true };
}

function gameBrief(game: GameView, reason: string | null) {
  return {
    ok: true,
    connected: game.connected,
    busy: game.busy || reason !== null,
    reason: reason ?? game.reason,
    inGame: game.inGame,
    turn: game.turn,
    year: game.year,
    civ: game.civ,
    dialog: game.dialog,
    cityWindow: game.cityWindow,
    taxWindow: game.taxWindow,
  };
}

async function notify(extra: Extra, message: string) {
  const progressToken = extra._meta?.progressToken;
  if (progressToken === undefined) return;
  try {
    await extra.sendNotification({ method: "notifications/progress", params: { progressToken, progress: 1, message } });
  } catch { /* opening the game still proceeds when the client is not listening */ }
}

function field(prop: SchemaProp): z.ZodType {
  if (prop.enum && prop.enum.length > 0) return z.enum(prop.enum as [string, ...string[]]);
  if (prop.type === "integer") return z.number().int();
  if (prop.type === "number") return z.number();
  if (prop.type === "boolean") return z.boolean();
  if (prop.type === "array") return z.array(z.string());
  if (prop.type === "object") return z.record(z.string(), z.unknown());
  return z.string();
}

export function shapeFrom(schema: { properties?: Record<string, SchemaProp>; required?: string[] }): Record<string, z.ZodType> {
  const required = new Set(schema.required ?? []);
  const shape: Record<string, z.ZodType> = {};
  for (const [name, prop] of Object.entries(schema.properties ?? {})) {
    let entry = field(prop);
    if (prop.description) entry = entry.describe(prop.description);
    if (!required.has(name)) entry = entry.optional();
    shape[name] = entry;
  }
  return shape;
}

export function inputShape(tool: CtlTool): Record<string, z.ZodType> {
  const shape = shapeFrom(tool.input_schema);
  if (needsLease(tool)) shape.lease = z.string().describe("claim 返回的租约");
  return shape;
}

const LEASE_PROP: SchemaProp = { type: "string", description: "claim 返回的租约" };

export type McpToolDoc = {
  name: string;
  description: string;
  /** 这一调用会改局面，或取走事件队列，所以必须先 claim。 */
  lease: boolean;
  input_schema: {
    type: "object";
    properties: Record<string, SchemaProp>;
    required: string[];
  };
};

function toolDoc(name: string, description: string, lease: boolean, properties: Record<string, SchemaProp>, required: string[]): McpToolDoc {
  return { name, description, lease, input_schema: { type: "object", properties, required } };
}

/** Session tools. Play tools are appended from civ2ctl at startup. */
export function sessionTools(): McpToolDoc[] {
  return [
    toolDoc("claim", "领取这台机器上的游戏租约。变更工具都要带返回的 lease。同一 client 再次调用时交还它已经持有的那把。别人持有且没有命令在执行、游戏也没在跑时，接过空闲租约；否则返回 busy，并带上 holder 和当前 lease。", false, {
      client: { type: "string", description: "调用方的名字，显示在观察页上" },
    }, ["client"]),
    toolDoc("release", "交回租约。有命令在跑时返回 busy。没有命令在跑时，可以不带 lease，改用 client 交回持有者自己的那把。", false, {
      lease: { type: "string", description: "claim 返回的租约" },
      client: { type: "string", description: "持有者名字。没有命令在跑、又没有 lease 时用来交回" },
    }, []),
    toolDoc("list_games", "列出 games 目录里的全部游戏局。", false, {}, []),
    toolDoc("game_status", "一局的状态。不给 id 时是进行中的那局。", false, {
      id: { type: "string", description: "games 目录下的一层目录名" },
    }, []),
    toolDoc("read_game", "观察页内存里的最新局面：连接、回合、年份、文明、对话框、城市窗口、税率窗口。不连管道。", false, {}, []),
    toolDoc("read_record", "读取一局目录里的复盘材料，不连游戏，也不需要租约。part 默认 index。存档只列名字，不读 .SAV。log 是命令摘要，每次最多 100 行。", false, {
      id: { type: "string", description: "games 目录下的一层目录名" },
      part: {
        type: "string",
        enum: ["index", "turns", "notes", "strategy", "snapshot", "runs", "run", "log", "brief", "rule"],
        description: "index 目录，turns 每回合一行，notes 笔记，strategy 策略，snapshot 快照，runs 各段总结，run 一段的 result.json，log 命令摘要，brief 要求，rule 法则",
      },
      turn: { type: "integer", description: "strategy 或 snapshot 的回合。不给 turn 时 strategy 读当前 strategy.md" },
      run: { type: "string", description: "runs 下的一段目录名" },
      offset: { type: "integer", description: "turns 或 log 从第几行开始，从 0 计。不给则取末尾" },
      limit: { type: "integer", description: "turns 最多 200 行，log 最多 100 行" },
    }, ["id"]),
    toolDoc("pack_game", "把一局打成复盘包。返回下载路径，用同一令牌 GET。不含开局前备份的用户文件。不需要租约。", false, {
      id: { type: "string", description: "games 目录下的一层目录名" },
    }, ["id"]),
    toolDoc("start_game", "按 spec 建一局并启动游戏、走完开局对话框。spec 的键与 civ2session 的 spec.json 相同。可能要数分钟。", true, {
      lease: LEASE_PROP,
      spec: { type: "object", description: "开局设置" },
      title: { type: "string", description: "这局的名字" },
      request: { type: "string", description: "用户的原话，记进 brief.md" },
      id: { type: "string", description: "目录名里的英文标识" },
    }, ["lease", "spec"]),
    toolDoc("resume_game", "从一局的目录接着玩：启动游戏，读最新的存档。id 是 games 下的一层目录名。", true, {
      lease: LEASE_PROP,
      id: { type: "string", description: "games 目录下的一层目录名" },
    }, ["lease", "id"]),
    toolDoc("checkpoint", "存盘点：存这一回合、记一行。回合开始、没有对话框也没有城市窗口时调用，每回合一次。", true, {
      lease: LEASE_PROP,
    }, ["lease"]),
    toolDoc("close_game", "收尾：存盘点，离开并退出游戏，恢复用户的文件，解锁。status 为 paused 或 ended。", true, {
      lease: LEASE_PROP,
      status: { type: "string", enum: ["paused", "ended"], description: "paused 暂停，ended 这局已经结束" },
      keep_running: { type: "boolean", description: "不退出游戏（用户的 CIV2.DAT、HALLFAME.DAT 不恢复）" },
    }, ["lease"]),
  ];
}

/** Every tool registered on /mcp, including the civ2ctl play tools loaded at startup. */
export function mcpTools(playTools: CtlTool[]): McpToolDoc[] {
  const play = visiblePlayTools(playTools).map((tool): McpToolDoc => {
    const properties = { ...(tool.input_schema.properties ?? {}) };
    const required = [...(tool.input_schema.required ?? [])];
    const lease = needsLease(tool);
    if (lease && !required.includes("lease")) {
      properties.lease = LEASE_PROP;
      required.push("lease");
    }
    return toolDoc(tool.name, toolDescription(tool), lease, properties, required);
  });
  return [...sessionTools(), ...play];
}

function text(value: unknown): string {
  return typeof value === "string" ? value : "";
}

function optText(value: unknown): string | undefined {
  return typeof value === "string" ? value : undefined;
}

function optInt(value: unknown): number | undefined {
  return typeof value === "number" && Number.isInteger(value) ? value : undefined;
}

function recordValue(value: unknown): Record<string, unknown> {
  return typeof value === "object" && value !== null && !Array.isArray(value) ? value as Record<string, unknown> : {};
}

function leaseOf(args: Record<string, unknown>): string {
  return typeof args.lease === "string" ? args.lease : "";
}

export function createMcpServer(deps: McpDeps): McpServer {
  const mcp = new McpServer({ name: "civ2agent", version: "1" });
  const leaseNow = () => deps.gate.leaseId();
  const env = () => sessionEnv(deps.gamesDir, leaseNow());
  const session = (args: string[], ms: number) =>
    runSession(deps.python, deps.session, deps.gamesDir, args, ms, leaseNow());

  const mutate = (command: string, lease: string, run: () => Promise<CliResult>) =>
    runMutation(deps.gate, command, lease, run, deps.onActivity);

  const documented = new Map(sessionTools().map(tool => [tool.name, tool]));
  const on = (name: string, handler: (args: Record<string, unknown>, extra: Extra) => Promise<ReturnType<typeof asTool>>) => {
    const tool = documented.get(name);
    if (!tool) throw new Error(`没有 MCP 工具 ${name}`);
    mcp.registerTool(name, {
      description: tool.description,
      inputSchema: shapeFrom(tool.input_schema),
    }, handler);
  };

  on("claim", async args => {
    const client = text(args.client);
    if (deps.gate.holderName() !== client.trim() && deps.gameDown()) deps.gate.releaseIdle();
    const result = deps.gate.claim(client);
    deps.onActivity();
    return asTool(result);
  });

  on("release", async args => {
    const lease = text(args.lease).trim();
    const result = lease ? deps.gate.release(lease) : deps.gate.releaseNamed(text(args.client));
    deps.onActivity();
    return asTool(result);
  });

  on("list_games", async () => asTool(await session(["list"], 8000)));

  on("game_status", async args => {
    let argv = ["status"];
    const id = optText(args.id);
    if (id) {
      const found = openGame(deps.gamesDir, id);
      if (found.ok !== true) return asTool(found);
      argv = ["status", found.dir];
    }
    return whenIdle(
      deps.gate,
      () => session(argv, 8000).then(asTool),
      reason => asTool({ ok: false, error: "busy", reason, game: gameBrief(deps.readSnapshot(), reason) }),
    );
  });

  on("read_game", async () => asTool(gameBrief(deps.readSnapshot(), deps.gate.activity())));

  on("read_record", async args => asTool(await readRecord(deps.gamesDir, {
    id: text(args.id),
    part: optText(args.part),
    turn: optInt(args.turn),
    run: optText(args.run),
    offset: optInt(args.offset),
    limit: optInt(args.limit),
  })));

  on("pack_game", async args => {
    const id = text(args.id);
    const found = openGame(deps.gamesDir, id);
    if (found.ok !== true) return asTool(found);
    try {
      const archive = await packGame(found.dir);
      return asTool({
        ok: true, id, files: archive.files, bytes: archive.bytes,
        path: `/pack/${encodeURIComponent(id)}`,
      });
    } catch (cause) {
      return asTool({ ok: false, error: "usage", reason: cause instanceof Error ? cause.message : "打不成包" });
    }
  });

  on("start_game", async (args, extra) => {
    const id = optText(args.id);
    if (id) {
      const found = resolveGameDir(deps.gamesDir, id);
      if (found.ok !== true) return asTool(found);
    }
    const result = await mutate("start_game", text(args.lease), async () => {
      await notify(extra, "正在开局");
      return session(startArgs(recordValue(args.spec), optText(args.title), optText(args.request), id), SESSION_MS);
    });
    return asTool(result);
  });

  on("resume_game", async args => {
    const found = openGame(deps.gamesDir, text(args.id));
    if (found.ok !== true) return asTool(found);
    return asTool(await mutate("resume_game", text(args.lease), () => session(["resume", found.dir], SESSION_MS)));
  });

  on("checkpoint", async args => asTool(await mutate("checkpoint", text(args.lease), () => session(["checkpoint"], CHECKPOINT_MS))));

  on("close_game", async args => asTool(await mutate("close_game", text(args.lease), () => session(
    closeArgs(args.status === "ended" ? "ended" : "paused", args.keep_running === true), SESSION_MS,
  ))));

  const playDocs = new Map(mcpTools(deps.playTools).map(tool => [tool.name, tool]));
  for (const tool of visiblePlayTools(deps.playTools)) {
    const doc = playDocs.get(tool.name);
    if (!doc) continue;
    const leased = doc.lease;
    mcp.registerTool(tool.name, {
      description: doc.description,
      inputSchema: shapeFrom(doc.input_schema),
    }, async (args: Record<string, unknown>) => {
      if (!leased) {
        return whenIdle(
          deps.gate,
          () => runCli(deps.python, deps.ctl, callArgs(tool.name, args, 2), 8000, env()).then(asTool),
          reason => asTool({ ok: false, error: "busy", reason, game: gameBrief(deps.readSnapshot(), reason) }),
        );
      }
      const timeout = tool.kind === "act" ? actTimeoutMs(args) : 8000;
      const connect = tool.kind === "act" ? 10 : 2;
      return asTool(await mutate(tool.name, leaseOf(args), () => runCli(deps.python, deps.ctl, callArgs(tool.name, args, connect), timeout, env())));
    });
  }

  return mcp;
}

type Session = { transport: StreamableHTTPServerTransport };

function header(req: IncomingMessage, name: string): string | undefined {
  const value = req.headers[name];
  return Array.isArray(value) ? value[0] : value;
}

function readBody(req: IncomingMessage): Promise<unknown> {
  return new Promise((resolve, reject) => {
    const chunks: Buffer[] = [];
    let size = 0;
    req.on("data", (chunk: Buffer) => {
      size += chunk.length;
      if (size > 1_000_000) {
        reject(new Error("请求太大"));
        req.destroy();
        return;
      }
      chunks.push(chunk);
    });
    req.on("end", () => {
      const text = Buffer.concat(chunks).toString("utf8");
      if (!text) { resolve(undefined); return; }
      try { resolve(JSON.parse(text) as unknown); }
      catch { reject(new Error("请求不是 JSON")); }
    });
    req.on("error", reject);
  });
}

function sendJson(res: ServerResponse, status: number, body: unknown) {
  if (res.headersSent) return;
  res.writeHead(status, { "content-type": "application/json; charset=utf-8" });
  res.end(JSON.stringify(body));
}

/** Streamable HTTP at /mcp. One MCP session per client; the game lease is separate. */
export function createMcpHandler(deps: McpDeps, token: string) {
  const sessions = new Map<string, Session>();

  async function handle(req: IncomingMessage, res: ServerResponse) {
    if (!bearerOk(header(req, "authorization"), token)) {
      res.writeHead(401, {
        "content-type": "application/json; charset=utf-8",
        "www-authenticate": "Bearer",
      });
      res.end(JSON.stringify({ error: "unauthorized" }));
      return;
    }
    try {
      const sid = header(req, "mcp-session-id");
      if (sid) {
        const found = sessions.get(sid);
        if (!found) {
          sendJson(res, 404, { jsonrpc: "2.0", error: { code: -32001, message: "Session not found" }, id: null });
          return;
        }
        const body = req.method === "POST" ? await readBody(req) : undefined;
        await found.transport.handleRequest(req, res, body);
        return;
      }
      if (req.method !== "POST") {
        sendJson(res, 400, { jsonrpc: "2.0", error: { code: -32000, message: "需要先初始化会话" }, id: null });
        return;
      }
      const body = await readBody(req);
      const transport = new StreamableHTTPServerTransport({
        sessionIdGenerator: () => randomUUID(),
        onsessioninitialized: id => { sessions.set(id, { transport }); },
      });
      transport.onclose = () => { if (transport.sessionId) sessions.delete(transport.sessionId); };
      const mcp = createMcpServer(deps);
      await mcp.connect(transport);
      await transport.handleRequest(req, res, body);
      if (!transport.sessionId) await transport.close();
    } catch (cause) {
      sendJson(res, 400, { jsonrpc: "2.0", error: { code: -32700, message: cause instanceof Error ? cause.message : "请求无法处理" }, id: null });
    }
  }

  async function close() {
    for (const item of sessions.values()) await item.transport.close().catch(() => undefined);
    sessions.clear();
  }

  return { handle, close };
}
