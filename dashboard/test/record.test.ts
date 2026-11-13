import assert from "node:assert/strict";
import { mkdtemp, mkdir, rm, writeFile } from "node:fs/promises";
import { tmpdir } from "node:os";
import path from "node:path";
import test from "node:test";
import { readRecord } from "../src/record.js";

async function game(): Promise<{ dir: string; id: string; done: () => Promise<void> }> {
  const root = await mkdtemp(path.join(tmpdir(), "civ2-record-"));
  const id = "sample";
  const dir = path.join(root, id);
  await mkdir(path.join(dir, "agent", "strategy"), { recursive: true });
  await mkdir(path.join(dir, "snapshots"), { recursive: true });
  await mkdir(path.join(dir, "runs", "s001-demo-cursor"), { recursive: true });
  await writeFile(path.join(dir, "session.json"), JSON.stringify({
    id, title: "样例", status: "paused", turn: 2, year: -3950, latest_save: "T0002.SAV",
    identity: { civ: "罗马人", leader: "凯撒" }, spec: { difficulty: "deity" },
    request: "快攻", guide: "jin-yuan-rush", rule: "basic", segments: 1, saves: ["T0002.SAV"],
  }));
  await writeFile(path.join(dir, "turns.jsonl"), [
    JSON.stringify({ turn: 1, year: -4000, gold: 0 }),
    JSON.stringify({ turn: 2, year: -3950, gold: 1 }),
  ].join("\n") + "\n");
  await writeFile(path.join(dir, "agent", "notes.md"), "笔记正文");
  await writeFile(path.join(dir, "agent", "strategy.md"), "当前策略");
  await writeFile(path.join(dir, "agent", "strategy", "T0001.md"), "第一回合");
  await writeFile(path.join(dir, "snapshots", "T0001.json"), JSON.stringify({ ok: true, turn: 1 }));
  await writeFile(path.join(dir, "runs.jsonl"), JSON.stringify({
    segment: 1, target: "cursor", status: "completed", turn_before: 1, turn_after: 2, summary: "建了首都",
  }) + "\n");
  await writeFile(path.join(dir, "runs", "s001-demo-cursor", "result.json"), JSON.stringify({ status: "completed", text: "建了首都" }));
  await writeFile(path.join(dir, "log.jsonl"), JSON.stringify({
    time: "t", command: "state", response: { ok: true, turn: 1, tiles: [1, 2, 3], reason: "x".repeat(500) },
  }) + "\n");
  return { dir: root, id, done: () => rm(root, { recursive: true, force: true }) };
}

test("the index lists the review files and does not inline a save", async () => {
  const sample = await game();
  try {
    const index = await readRecord(sample.dir, { id: sample.id });
    assert.equal(index.ok, true);
    assert.equal(index.turn, 2);
    assert.equal(index.civ, "罗马人");
    assert.deepEqual(index.saves, ["T0002.SAV"]);
    assert.equal(index.turns, 2);
    assert.deepEqual(index.snapshots, ["T0001.json"]);
    assert.deepEqual(index.strategies, ["T0001.md"]);
    assert.equal(index.log_lines, 1);
    assert.equal((index.runs as { summary: string }[])[0]?.summary, "建了首都");
    const notes = await readRecord(sample.dir, { id: sample.id, part: "notes" });
    assert.equal(notes.notes, "笔记正文");
    const strategy = await readRecord(sample.dir, { id: sample.id, part: "strategy", turn: 1 });
    assert.equal(strategy.strategy, "第一回合");
    const snap = await readRecord(sample.dir, { id: sample.id, part: "snapshot", turn: 1 });
    assert.equal((snap.snapshot as { turn: number }).turn, 1);
    const logged = await readRecord(sample.dir, { id: sample.id, part: "log" });
    const row = (logged.rows as { command: string; reason?: string; tiles?: unknown }[])[0];
    assert.equal(row?.command, "state");
    assert.equal(row?.tiles, undefined);
    assert.ok((row?.reason ?? "").length <= 200);
    assert.equal((await readRecord(sample.dir, { id: sample.id, part: "run", run: "../session.json" })).ok, false);
    assert.equal((await readRecord(sample.dir, { id: "../other" })).ok, false);
  } finally {
    await sample.done();
  }
});
