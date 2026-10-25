// Offline tests of the runner's pure parts: node --test test/*.test.mjs
import assert from "node:assert/strict";
import { mkdtempSync, rmSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import path from "node:path";
import test from "node:test";
import { plan } from "js-agent-runtime";
import { CLI, RUNNER_DIR, buildPrompt, describeEvent, lastJson, loadTargets, pickTarget, segmentGoal } from "../lib.mjs";

const example = path.join(RUNNER_DIR, "targets.example.json");

function targetsFile(value) {
  const dir = mkdtempSync(path.join(tmpdir(), "civ2-targets-"));
  const file = path.join(dir, "targets.json");
  writeFileSync(file, JSON.stringify(value));
  return { file, done: () => rmSync(dir, { recursive: true, force: true }) };
}

test("the example has a target for every built-in harness", () => {
  const loaded = loadTargets(example);
  assert.equal(loaded.default, "claude");
  assert.deepEqual(Object.values(loaded.targets).map(t => t.harness.adapter).sort(),
    ["claude-code", "codex", "cursor-agent", "deepseek-harness", "opencode"]);
  assert.equal(pickTarget(loaded).name, "claude");
  assert.equal(pickTarget(loaded, "codex").harness.adapter, "codex");
  assert.throws(() => pickTarget(loaded, "gemini"), /No target gemini/u);
});

test("bad targets are refused before anything runs", () => {
  for (const bad of [
    { targets: [] },
    { targets: { x: { harness: { adapter: "gemini", executable: "g", permission: "read-only" }, backend: { auth: "api-key" } } } },
    { targets: { x: { harness: { adapter: "codex", executable: "codex", permission: "everything" }, backend: { auth: "chatgpt", model: "m" } } } },
    { default: "y", targets: { x: { harness: { adapter: "codex", executable: "codex", permission: "read-only" }, backend: { auth: "chatgpt", model: "m" } } } },
  ]) {
    const { file, done } = targetsFile(bad);
    try {
      assert.throws(() => loadTargets(file));
    } finally {
      done();
    }
  }
});

test("every example target plans a segment in a game folder", () => {
  const workspace = mkdtempSync(path.join(tmpdir(), "civ2-agent-"));
  try {
    const prompt = buildPrompt({ sessionDir: path.dirname(workspace), python: "C:\\Python313\\python.exe" });
    for (const target of Object.values(loadTargets(example).targets)) {
      const execution = plan({ runId: "g1-s001", workspace, prompt, target, timeoutMs: 60_000 });
      assert.equal(execution.request.workspace, path.resolve(workspace));
      assert.equal(execution.process.stdin, prompt, target.name);
    }
  } finally {
    rmSync(workspace, { recursive: true, force: true });
  }
});

test("the prompt points at the tools and the files to read", () => {
  const prompt = buildPrompt({ sessionDir: "D:\\games\\g1", python: "C:\\Program Files\\Python\\python.exe" });
  for (const part of [CLI.ctl, CLI.session, CLI.protocol, "D:\\games\\g1", "playbook.md", "notes.md", "\"C:\\Program Files\\Python\\python.exe\""])
    assert.ok(prompt.includes(part), part);
  assert.match(segmentGoal({ status: "setup" }, 6), /spec\.json/u);
  assert.match(segmentGoal({ status: "active", turn: 12 }, 17), /第 12 回合.*第 17 回合/u);
});

test("events become short lines", () => {
  const ev = (type, data) => ({ type, data });
  assert.equal(describeEvent(ev("tool", { type: "tool_use", name: "Bash", input: { command: "python civ2ctl.py state" } })),
    "tool Bash python civ2ctl.py state");
  assert.equal(describeEvent(ev("tool", { phase: "item.started", item: { type: "command_execution", command: "ls" } })),
    "tool shell ls");
  assert.equal(describeEvent(ev("tool", { type: "tool_result", content: "x" })), null);
  assert.equal(describeEvent(ev("message", { text: "partial", delta: "p" })), null);
  assert.equal(describeEvent(ev("message", { text: "第 3 回合结束" })), "say 第 3 回合结束");
  assert.equal(describeEvent(ev("failed", { error: "boom" })), "failed boom");
  assert.equal(describeEvent(ev("raw", {})), null);
});

test("the last JSON line of a command is its result", () => {
  assert.deepEqual(lastJson('noise\n{"ok": true, "turn": 4}\n\n'), { ok: true, turn: 4 });
  assert.throws(() => lastJson("   \n"));
});
