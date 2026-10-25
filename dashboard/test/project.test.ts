import assert from "node:assert/strict";
import test from "node:test";
import { commandFromLog, earlierUsage, freshTail, readAppended, readPlanBrief } from "../src/follow.js";
import { foldUsage, mergeRun, parseUsage, projectRuntime, toolLine, usageFromLines } from "../src/project.js";
import type { RuntimeEvent } from "../src/types.js";
import { appendFile, mkdir, mkdtemp, rm, writeFile } from "node:fs/promises";
import { tmpdir } from "node:os";
import path from "node:path";

const event = (type: RuntimeEvent["type"], data: unknown, runId = "run-1"): RuntimeEvent =>
  ({ schemaVersion: 1, runId, sequence: 1, time: "2026-10-25T09:21:14.000Z", type, data });

test("thinking deltas and a completed block become thought pieces", () => {
  assert.deepEqual(projectRuntime(event("raw", { type: "thinking", subtype: "delta", text: "先看" })),
    [{ event: "thought", data: { text: "先看", done: false } }]);
  assert.deepEqual(projectRuntime(event("raw", { type: "thinking", subtype: "completed" })),
    [{ event: "thought", data: { text: "", done: true } }]);
});

test("a shell tool start is one short line and its completion is ignored", () => {
  const started = { type: "tool_call", subtype: "started", tool_call: { shellToolCall: { args: { command: "python civ2ctl.py state --only civ" } } } };
  const line = toolLine(started);
  assert.equal(line, "shell python civ2ctl.py state --only civ");
  assert.deepEqual(projectRuntime(event("tool", { ...started, subtype: "completed" })), []);
});

test("assistant text and the opening prompt are kept", () => {
  const said = projectRuntime(event("message", { message: { content: [{ type: "text", text: "先读局面。" }] } }));
  assert.equal(said[0]?.event, "action");
  assert.equal(said[0] && said[0].event === "action" ? said[0].data.kind : "", "say");
  const prompt = projectRuntime(event("raw", { type: "user", message: { content: [{ type: "text", text: "你在玩一局。" }] } }));
  assert.deepEqual(prompt, [{ event: "prompt", data: { text: "你在玩一局。" } }]);
});

test("a log line keeps the command and drops the state body", () => {
  const command = commandFromLog(JSON.stringify({
    time: "2026-10-25T17:18:56", command: "state",
    response: { ok: false, error: "modal_open", reason: "已有对话框", cities: [{ name: "罗马" }] },
  }));
  assert.deepEqual(command, { time: "2026-10-25T17:18:56", command: "state", ok: false, error: "modal_open", reason: "已有对话框" });
});

test("a harness init names the model it selected", () => {
  const pieces = projectRuntime(event("raw", { type: "system", subtype: "init", model: "Auto" }));
  assert.deepEqual(pieces, [{ event: "run", data: { runId: "run-1", model: "Auto" } }]);
});

test("a started event keeps the adapter and the target name", () => {
  const pieces = projectRuntime(event("started", { adapter: { id: "cursor-agent", version: "2" }, target: "cursor" }));
  assert.equal(pieces[0]?.event, "run");
  assert.deepEqual(pieces[0] && pieces[0].event === "run" ? pieces[0].data : {}, {
    runId: "run-1", status: "running", adapter: "cursor-agent", target: "cursor",
  });
});

test("cursor usage sums input, output and cache when the event has no total", () => {
  const usage = parseUsage({ inputTokens: 54487, outputTokens: 4716, cacheReadTokens: 338688, cacheWriteTokens: 0 });
  assert.deepEqual(usage, {
    input: 54487, output: 4716, cached: 338688, cacheWrite: 0, reasoning: null, total: 397891,
  });
});

test("a stated total is kept when cache is already inside the input count", () => {
  const usage = parseUsage({
    inputTokens: 100, outputTokens: 20, totalTokens: 120, cachedInputTokens: 40, reasoningTokens: 5,
  });
  assert.equal(usage?.total, 120);
  assert.equal(usage?.reasoning, 5);
});

test("step usage adds up and a later run total replaces it", () => {
  const step = { input: 10, output: 4, cached: 1, cacheWrite: null, reasoning: 2, total: 14 };
  const again = { ...step, input: 3, output: 1, total: 4 };
  assert.equal(foldUsage("codex", step, again).total, 18);
  assert.equal(foldUsage("cursor-agent", step, again).total, 4);
  const covered = { input: 13, output: 5, cached: 1, cacheWrite: null, reasoning: 2, total: 18 };
  assert.equal(foldUsage("opencode", foldUsage("opencode", step, again), covered).total, 18);
});

test("claude's assistant message records the model that answered", () => {
  const pieces = projectRuntime(event("raw", { type: "assistant", message: { model: "claude-opus-4-5", content: [] } }));
  assert.deepEqual(pieces, [{ event: "run", data: { runId: "run-1", model: "claude-opus-4-5" } }]);
});

test("a run keeps tokens already counted when the plan fills in effort", () => {
  const usage = { input: 3, output: 1, cached: null, cacheWrite: null, reasoning: null, total: 4 };
  const current = {
    runId: "run-1", status: "running" as const, adapter: "codex", target: "codex",
    model: "gpt-6", requestedModel: "", effort: "", usage,
  };
  const next = mergeRun(current, { requestedModel: "gpt-6", effort: "high" });
  assert.equal(next.effort, "high");
  assert.equal(next.model, "gpt-6");
  assert.equal(next.usage, usage);
});

test("usage lines from one file fold with that file's adapter", () => {
  const started = JSON.stringify({ schemaVersion: 1, runId: "run-1", sequence: 1, time: "t", type: "started", data: { adapter: { id: "codex" }, target: "codex" } });
  const usage = (total: number) => JSON.stringify({
    schemaVersion: 1, runId: "run-1", sequence: 2, time: "t", type: "usage",
    data: { inputTokens: total, outputTokens: 1, totalTokens: total + 1, cachedInputTokens: 0, reasoningTokens: 0 },
  });
  const folded = usageFromLines([started, usage(10), usage(4)]);
  assert.equal(folded?.total, 16);
});

test("the plan head supplies the requested model and reasoning effort", async () => {
  const dir = await mkdtemp(path.join(tmpdir(), "civ2-dash-"));
  const events = path.join(dir, "events.jsonl");
  await writeFile(events, "");
  await writeFile(path.join(dir, "plan.json"), JSON.stringify({
    request: { target: { name: "codex", harness: { adapter: "codex" }, backend: { model: "gpt-6", reasoningEffort: "high" } } },
    files: [{ content: "x".repeat(200000) }],
  }));
  assert.deepEqual(await readPlanBrief(events), { adapter: "codex", target: "codex", requestedModel: "gpt-6", effort: "high" });
  await rm(dir, { recursive: true, force: true });
});

test("earlier usage skips the run that is still open", async () => {
  const dir = await mkdtemp(path.join(tmpdir(), "civ2-dash-"));
  const line = (runId: string, total: number) => JSON.stringify({
    schemaVersion: 1, runId, sequence: 1, time: "t", type: "usage",
    data: { inputTokens: total, outputTokens: 0, totalTokens: total, cachedInputTokens: 0, reasoningTokens: 0 },
  });
  const first = path.join(dir, "runs", "s001", "events.jsonl");
  const second = path.join(dir, "runs", "s002", "events.jsonl");
  await mkdir(path.dirname(first), { recursive: true });
  await mkdir(path.dirname(second), { recursive: true });
  await writeFile(first, line("a", 10) + "\n");
  await writeFile(second, line("b", 7) + "\n");
  assert.equal((await earlierUsage(dir, second))?.total, 10);
  assert.equal((await earlierUsage(dir, null))?.total, 17);
  await rm(dir, { recursive: true, force: true });
});

test("a tail reads only the bytes added since last time", async () => {
  const dir = await mkdtemp(path.join(tmpdir(), "civ2-dash-"));
  const file = path.join(dir, "events.jsonl");
  await writeFile(file, "{\"a\":1}\n{\"b\":");
  const tail = freshTail();
  tail.path = file;
  assert.deepEqual(await readAppended(tail), ['{"a":1}']);
  await appendFile(file, "2}\n");
  assert.deepEqual(await readAppended(tail), ['{"b":2}']);
  await rm(dir, { recursive: true, force: true });
});
