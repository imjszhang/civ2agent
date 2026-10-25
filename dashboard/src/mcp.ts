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
};

export function bindAllowed(host: string, token: string): boolean {
  return host === "127.0.0.1" || host === "::1" || host === "localhost" || token.length > 0;
}

export function bearerOk(authorization: string | undefined, token: string): boolean {
  if (!token) return true;
  const given = authorization?.startsWith("Bearer ") ? authorization.slice("Bearer ".length) : "";
  const left = Buffer.from(given);
  const right = Buffer.from(token);
  return left.length === right.length && timingSafeEqual(left, right);
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
  if (prop.type === "integer" || prop.type === "number") return z.number();
  if (prop.type === "boolean") return z.boolean();
  if (prop.type === "array") return z.array(z.string());
  return z.string();
}

export function inputShape(tool: CtlTool): Record<string, z.ZodType> {
  const required = new Set(tool.input_schema.required ?? []);
  const shape: Record<string, z.ZodType> = {};
  for (const [name, prop] of Object.entries(tool.input_schema.properties ?? {})) {
    let entry = field(prop);
    if (prop.description) entry = entry.describe(prop.description);
    if (!required.has(name)) entry = entry.optional();
    shape[name] = entry;
  }
  if (needsLease(tool)) shape.lease = z.string().describe("claim 返回的租约");
  return shape;
}

function leaseOf(args: Record<string, unknown>): string {
  return typeof args.lease === "string" ? args.lease : "";
}

export function createMcpServer(deps: McpDeps): McpServer {
  const mcp = new McpServer({ name: "civ2agent", version: "1" });
  const env = () => sessionEnv(deps.gamesDir);

  const mutate = (command: string, lease: string, run: () => Promise<CliResult>) =>
    runMutation(deps.gate, command, lease, run, deps.onActivity);

  mcp.registerTool("claim", {
    description: "领取这台机器上的游戏租约。变更工具都要带返回的 lease。已有租约时返回 busy。",
    inputSchema: { client: z.string().describe("调用方的名字，显示在观察页上") },
  }, async args => {
    const result = deps.gate.claim(args.client);
    deps.onActivity();
    return asTool(result);
  });

  mcp.registerTool("release", {
    description: "交回租约。有命令在跑时返回 busy。",
    inputSchema: { lease: z.string().describe("claim 返回的租约") },
  }, async args => {
    const result = deps.gate.release(args.lease);
    deps.onActivity();
    return asTool(result);
  });

  mcp.registerTool("list_games", {
    description: "列出 games 目录里的全部游戏局。",
    inputSchema: {},
  }, async () => asTool(await runSession(deps.python, deps.session, deps.gamesDir, ["list"], 8000)));

  mcp.registerTool("game_status", {
    description: "一局的状态。不给 id 时是进行中的那局。",
    inputSchema: { id: z.string().optional().describe("games 目录下的一层目录名") },
  }, async args => {
    let argv = ["status"];
    if (args.id) {
      const found = openGame(deps.gamesDir, args.id);
      if (found.ok !== true) return asTool(found);
      argv = ["status", found.dir];
    }
    return whenIdle(
      deps.gate,
      () => runSession(deps.python, deps.session, deps.gamesDir, argv, 8000).then(asTool),
      reason => asTool({ ok: false, error: "busy", reason, game: gameBrief(deps.readSnapshot(), reason) }),
    );
  });

  mcp.registerTool("read_game", {
    description: "观察页内存里的最新局面：连接、回合、年份、文明、对话框、城市窗口、税率窗口。不连管道。",
    inputSchema: {},
  }, async () => asTool(gameBrief(deps.readSnapshot(), deps.gate.activity())));

  mcp.registerTool("start_game", {
    description: "按 spec 建一局并启动游戏、走完开局对话框。spec 的键与 civ2session 的 spec.json 相同。可能要数分钟。",
    inputSchema: {
      lease: z.string().describe("claim 返回的租约"),
      spec: z.record(z.string(), z.unknown()).describe("开局设置"),
      title: z.string().optional().describe("这局的名字"),
      request: z.string().optional().describe("用户的原话，记进 brief.md"),
      id: z.string().optional().describe("目录名里的英文标识"),
    },
  }, async (args, extra) => {
    if (args.id) {
      const found = resolveGameDir(deps.gamesDir, args.id);
      if (found.ok !== true) return asTool(found);
    }
    const result = await mutate("start_game", args.lease, async () => {
      await notify(extra, "正在开局");
      return runSession(deps.python, deps.session, deps.gamesDir, startArgs(args.spec, args.title, args.request, args.id), SESSION_MS);
    });
    return asTool(result);
  });

  mcp.registerTool("resume_game", {
    description: "从一局的目录接着玩：启动游戏，读最新的存档。id 是 games 下的一层目录名。",
    inputSchema: {
      lease: z.string().describe("claim 返回的租约"),
      id: z.string().describe("games 目录下的一层目录名"),
    },
  }, async args => {
    const found = openGame(deps.gamesDir, args.id);
    if (found.ok !== true) return asTool(found);
    return asTool(await mutate("resume_game", args.lease, () => runSession(deps.python, deps.session, deps.gamesDir, ["resume", found.dir], SESSION_MS)));
  });

  mcp.registerTool("checkpoint", {
    description: "存盘点：存这一回合、记一行。回合开始、没有对话框也没有城市窗口时调用，每回合一次。",
    inputSchema: { lease: z.string().describe("claim 返回的租约") },
  }, async args => asTool(await mutate("checkpoint", args.lease, () => runSession(deps.python, deps.session, deps.gamesDir, ["checkpoint"], CHECKPOINT_MS))));

  mcp.registerTool("close_game", {
    description: "收尾：存盘点，离开并退出游戏，恢复用户的文件，解锁。status 为 paused 或 ended。",
    inputSchema: {
      lease: z.string().describe("claim 返回的租约"),
      status: z.enum(["paused", "ended"]).optional().describe("paused 暂停，ended 这局已经结束"),
      keep_running: z.boolean().optional().describe("不退出游戏（用户的 CIV2.DAT、HALLFAME.DAT 不恢复）"),
    },
  }, async args => asTool(await mutate("close_game", args.lease, () => runSession(
    deps.python, deps.session, deps.gamesDir, closeArgs(args.status ?? "paused", args.keep_running === true), SESSION_MS,
  ))));

  for (const tool of visiblePlayTools(deps.playTools)) {
    const leased = needsLease(tool);
    mcp.registerTool(tool.name, {
      description: toolDescription(tool),
      inputSchema: inputShape(tool),
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
