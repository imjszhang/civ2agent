import assert from "node:assert/strict";
import { mkdir, mkdtemp, rm, writeFile } from "node:fs/promises";
import { tmpdir } from "node:os";
import path from "node:path";
import test from "node:test";
import { fileURLToPath } from "node:url";
import { runCli } from "../src/cli.js";
import { Gate, runMutation, whenIdle } from "../src/gate.js";
import { bindAllowed } from "../src/mcp.js";
import { inputShape } from "../src/mcp.js";
import { needsLease, toolDescription, visiblePlayTools, type CtlTool } from "../src/play.js";
import { openGame, resolveGameDir } from "../src/session.js";
import { startDashboard, type DashboardOptions } from "../src/server.js";

const fixture: CtlTool[] = [
  { name: "move", kind: "act", description: "走一步", input_schema: { properties: { unit: { type: "integer", description: "单位" }, x: { type: "integer" }, y: { type: "integer" } }, required: ["unit", "x", "y"] } },
  { name: "save-game", kind: "act", description: "存", input_schema: { properties: {}, required: [] } },
  { name: "load-game", kind: "act", description: "读", input_schema: { properties: {}, required: [] } },
  { name: "quit-game", kind: "act", description: "离开", input_schema: { properties: {}, required: [] } },
  { name: "wait-ready", kind: "session", description: "等", input_schema: { properties: {}, required: [] } },
  { name: "skip-intro", kind: "session", description: "跳过", input_schema: { properties: {}, required: [] } },
  { name: "events", kind: "observe", description: "事件", input_schema: { properties: {}, required: [] } },
  { name: "status", kind: "observe", description: "状态", input_schema: { properties: {}, required: [] } },
  { name: "respond", kind: "act", description: "回答当前对话框。", input_schema: { properties: {}, required: [] } },
];

test("a second mutation is rejected and observe does not spawn", async () => {
  const gate = new Gate(60_000, () => 0);
  const claimed = gate.claim("甲");
  assert.equal(claimed.ok, true);
  const lease = String(claimed.lease);
  let spawned = 0;
  let release = () => {};
  const hold = new Promise<void>(resolve => { release = resolve; });
  const running = runMutation(gate, "start_game", lease, async () => {
    spawned += 1;
    await hold;
    return { ok: true };
  });
  await Promise.resolve();
  const skipped = await whenIdle(gate, async () => { spawned += 10; return "ran"; }, reason => reason);
  assert.equal(spawned, 1);
  assert.match(skipped, /开局/);
  const second = await runMutation(gate, "close_game", lease, async () => { spawned += 100; return { ok: true }; });
  assert.equal(second.ok, false);
  assert.equal(second.error, "busy");
  assert.equal(spawned, 1);
  release();
  assert.equal((await running).ok, true);
});

test("a mutation without a lease is rejected, and an idle lease can be claimed again", () => {
  const gate = new Gate(60_000, () => 0);
  const denied = gate.begin("start_game", "");
  assert.equal(denied.ok, false);
  assert.equal(denied.error, "illegal");

  let now = 1_000;
  const idle = new Gate(500, () => now);
  const first = idle.claim("甲");
  assert.equal(first.ok, true);
  assert.equal(idle.begin("start_game", String(first.lease)).ok, true);
  now = 10_000;
  assert.equal(idle.claim("乙").ok, false);
  idle.end("start_game", { ok: true });
  now = 20_000;
  const second = idle.claim("乙");
  assert.equal(second.ok, true);
  assert.notEqual(second.lease, first.lease);
});

test("game ids stay inside one directory level", async () => {
  assert.equal(resolveGameDir("D:/games", "..").ok, false);
  assert.equal(resolveGameDir("D:/games", "D:/elsewhere").ok, false);
  assert.equal(resolveGameDir("D:/games", "a/b").ok, false);
  assert.equal(resolveGameDir("D:/games", "a\\b").ok, false);
  const named = resolveGameDir("D:/games", "romans-king");
  assert.equal(named.ok, true);
  if (named.ok === true) assert.equal(path.basename(named.dir), "romans-king");

  const root = await mkdtemp(path.join(tmpdir(), "civ2-games-"));
  try {
    assert.equal(openGame(root, "missing").ok, false);
    const dir = path.join(root, "kept-game");
    await mkdir(dir);
    await writeFile(path.join(dir, "session.json"), "{}\n");
    const opened = openGame(root, "kept-game");
    assert.equal(opened.ok, true);
  } finally {
    await rm(root, { recursive: true, force: true });
  }
});

test("play tools omit saves, loads, quits and launch commands", () => {
  const visible = visiblePlayTools(fixture);
  const names = visible.map(tool => tool.name);
  for (const hidden of ["save-game", "load-game", "quit-game", "wait-ready", "skip-intro"]) {
    assert.equal(names.includes(hidden), false);
  }
  const move = visible.find(tool => tool.name === "move");
  assert.ok(move);
  assert.equal(needsLease(move), true);
  assert.ok(inputShape(move).lease);
  const events = visible.find(tool => tool.name === "events");
  const status = visible.find(tool => tool.name === "status");
  assert.ok(events && status);
  assert.equal(needsLease(events), true);
  assert.equal(needsLease(status), false);
  const respond = visible.find(tool => tool.name === "respond");
  assert.ok(respond);
  assert.match(toolDescription(respond), /鼠标/);
});

test("a timed out command is killed", async () => {
  const dir = await mkdtemp(path.join(tmpdir(), "civ2-cli-"));
  const script = path.join(dir, "fake.mjs");
  await writeFile(script, "setInterval(() => {}, 1000);\n");
  try {
    const result = await runCli(process.execPath, script, [], 200);
    assert.equal(result.ok, false);
    assert.equal(result.error, "timeout");
  } finally {
    await rm(dir, { recursive: true, force: true });
  }
});

test("a LAN bind without a token is refused", async () => {
  assert.equal(bindAllowed("127.0.0.1", ""), true);
  assert.equal(bindAllowed("0.0.0.0", ""), false);
  await assert.rejects(() => startDashboard(options("0.0.0.0", "", "", "")), /DASHBOARD_TOKEN/);
});

test("MCP rejects a bad token and lists session tools without one", async () => {
  const dir = await mkdtemp(path.join(tmpdir(), "civ2-mcp-"));
  const script = path.join(dir, "fake.mjs");
  await writeFile(script, `const args = process.argv.slice(2);
if (args[0] === "tools") {
  console.log(JSON.stringify({ ok: true, tools: ${JSON.stringify(fixture)} }));
} else {
  console.log(JSON.stringify({ ok: true, args }));
}
`);
  const locked = await startDashboard(options("127.0.0.1", "secret", script, dir));
  const open = await startDashboard(options("127.0.0.1", "", script, dir));
  try {
    const denied = await post(locked.url, { authorization: "Bearer wrong" }, initialize());
    assert.equal(denied.status, 401);
    const init = await post(open.url, {}, initialize());
    assert.equal(init.status, 200, init.body);
    const session = init.headers.get("mcp-session-id");
    assert.ok(session);
    await post(open.url, { "mcp-session-id": session }, { jsonrpc: "2.0", method: "notifications/initialized" });
    const listed = await post(open.url, { "mcp-session-id": session, "mcp-protocol-version": "2025-03-26" }, {
      jsonrpc: "2.0", id: 2, method: "tools/list",
    });
    assert.equal(listed.status, 200, listed.body);
    const names = toolNames(listed.body);
    assert.ok(names.includes("list_games"));
    assert.ok(names.includes("start_game"));
    assert.equal(names.includes("save-game"), false);
    assert.ok(names.includes("move"));
  } finally {
    await open.close();
    await locked.close();
    await rm(dir, { recursive: true, force: true });
  }
});

function options(host: string, token: string, script: string, gamesDir: string): DashboardOptions {
  return {
    host, port: 0, token, python: process.execPath, ctl: script, session: script,
    gamesDir, webDir: path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..", "web"),
    idleMs: 60_000,
  };
}

function initialize() {
  return {
    jsonrpc: "2.0", id: 1, method: "initialize",
    params: { protocolVersion: "2025-03-26", capabilities: {}, clientInfo: { name: "test", version: "0" } },
  };
}

async function post(url: string, headers: Record<string, string>, body: unknown) {
  const response = await fetch(`${url}/mcp`, {
    method: "POST",
    headers: {
      "content-type": "application/json",
      accept: "application/json, text/event-stream",
      ...headers,
    },
    body: JSON.stringify(body),
  });
  return { status: response.status, headers: response.headers, body: await response.text() };
}

function toolNames(body: string): string[] {
  const lines = body.split(/\r?\n/).filter(line => line.startsWith("data:")).map(line => line.slice(5).trim());
  for (const item of (lines.length ? lines : [body]).reverse()) {
    try {
      const message = JSON.parse(item) as { result?: { tools?: { name: string }[] } };
      if (message.result?.tools) return message.result.tools.map(tool => tool.name);
    } catch { /* a non-JSON SSE field */ }
  }
  return [];
}
