#!/usr/bin/env node
// Let an agent CLI play a civ2agent game, a stretch of turns at a time.
// js-agent-runtime starts the chosen harness (Codex, Claude Code, DeepSeek
// Harness, OpenCode, Cursor Agent) in the game's agent/ folder; between
// stretches this program checkpoints the game and records the run.
import { spawnSync } from "node:child_process";
import { existsSync } from "node:fs";
import path from "node:path";
import { parseArgs } from "node:util";
import { plan, probe, run } from "js-agent-runtime";
import { CLI, RUNNER_DIR, buildPrompt, describeEvent, lastJson, loadTargets, pickTarget, segmentGoal } from "./lib.mjs";

const HELP = `用法：
  node civ2-play.mjs targets
  node civ2-play.mjs probe [--target NAME]
  node civ2-play.mjs new (--spec FILE | --request 文字) [--title T] [选项]
  node civ2-play.mjs resume DIR [选项]

选项：
  --target NAME        用哪个 agent（targets.json 里的名字，默认是它的 default）
  --targets FILE       targets 文件，默认 runner/targets.json
  --turns N            这次玩多少回合，默认 10
  --segment-turns K    每段 agent 运行玩多少回合，默认 5
  --segment-minutes M  每段最长多少分钟，默认 60
  --keep-open          玩完不收尾（不退出游戏，锁也留着）
  --python PATH        运行 civ2 命令行的 Python，默认 CIV2AGENT_PYTHON 或 python`;

const python = () => options.python ?? process.env.CIV2AGENT_PYTHON ?? "python";
let options = {};

function session(...args) {
  const proc = spawnSync(python(), [CLI.session, ...args], {
    encoding: "utf8", env: { ...process.env, PYTHONIOENCODING: "utf-8" }, timeout: 15 * 60_000, windowsHide: true,
  });
  if (proc.error) throw new Error(`civ2session.py ${args[0]}: ${proc.error.message}`);
  return lastJson(proc.stdout);
}

function say(line) {
  process.stderr.write(`${new Date().toISOString().slice(11, 19)} ${line}\n`);
}

function targets() {
  return loadTargets(options.targets ?? path.join(RUNNER_DIR, "targets.json"));
}

async function cmdProbe() {
  const target = pickTarget(targets(), options.target);
  const execution = plan({ runId: "civ2-probe", workspace: RUNNER_DIR, prompt: "probe", target, timeoutMs: 60_000 });
  const login = ["chatgpt", "claude-subscription", "claude-config", "cursor-login"].includes(target.backend.auth);
  return { ok: true, target: target.name, ...(await probe(execution, { checkExecutable: true, checkLogin: login })) };
}

async function playSegment(dir, target, untilTurn, number, signal) {
  const before = session("context", dir, "--goal", "(准备中)");
  const goal = segmentGoal(before, untilTurn);
  const context = session("context", dir, "--goal", goal);
  const stamp = new Date().toISOString().replace(/[-:T]/gu, "").slice(0, 14);
  const runId = `${path.basename(dir)}-s${String(number).padStart(3, "0")}-${stamp}`.slice(0, 120);
  const execution = plan({
    runId, workspace: context.workspace, prompt: buildPrompt({ sessionDir: dir, python: python() }), target,
    timeoutMs: (options.segmentMinutes ?? 60) * 60_000, maxOutputBytes: 64 * 1024 * 1024,
  });
  say(`segment ${number}: ${target.name} (${target.harness.adapter}) — ${goal}`);
  const result = await run(execution, {
    signal, runDir: path.join(dir, "runs", `s${String(number).padStart(3, "0")}-${stamp}-${target.name}`),
    onEvent: event => { const line = describeEvent(event); if (line) say(line); },
  });
  const after = session("context", dir, "--goal", goal);
  const record = {
    segment: number, target: target.name, adapter: target.harness.adapter, status: result.status,
    error: result.error, turn_before: before.turn, turn_after: after.turn, goal_turn: untilTurn,
    duration_ms: result.durationMs, usage: result.usage, cost: result.cost, model: result.responseModel,
    summary: result.text.slice(0, 2000),
  };
  session("record-run", dir, JSON.stringify(record));
  say(`segment ${number}: ${result.status}, turn ${before.turn ?? "-"} -> ${after.turn ?? "-"}`);
  return { record, context: after };
}

async function playSession(dir, signal) {
  const target = pickTarget(targets(), options.target);
  const turns = options.turns ?? 10, step = options.segmentTurns ?? 5;
  const segments = [];
  let start = session("context", dir).turn;
  const until = (start ?? 1) + turns;
  try {
    for (let number = session("context", dir).segments + 1; !signal.aborted; number++) {
      const now = session("context", dir);
      if (now.status === "ended") break;
      if (now.status !== "setup" && (now.turn ?? 0) >= until) break;
      const goalTurn = Math.min(until, (now.turn ?? 1) + step);
      const { record, context } = await playSegment(dir, target, goalTurn, number, signal);
      segments.push(record);
      if (record.status !== "completed" || ["setup", "failed"].includes(context.status)) break;
      if (start !== null && context.turn === record.turn_before) {
        say("the agent ended without advancing a turn; stopping");
        break;
      }
      start = start ?? context.turn;
    }
  } finally {
    // A game that never started has nothing to close; one that did is saved,
    // quit and the user's files put back.
    if (!options.keepOpen && session("context", dir).status !== "setup") {
      const closed = session("close");
      say(`close: ${closed.ok ? "ok" : closed.reason}`);
      segments.push({ close: closed });
    }
  }
  return { ok: segments.every(s => s.close ? s.close.ok : s.status === "completed"), dir, segments };
}

async function main(argv) {
  const { values, positionals } = parseArgs({
    args: argv, allowPositionals: true, options: {
      target: { type: "string" }, targets: { type: "string" }, turns: { type: "string" },
      "segment-turns": { type: "string" }, "segment-minutes": { type: "string" }, "keep-open": { type: "boolean" },
      spec: { type: "string" }, request: { type: "string" }, title: { type: "string" }, python: { type: "string" },
      help: { type: "boolean" },
    },
  });
  const int = v => (v === undefined ? undefined : Number.parseInt(v, 10));
  options = {
    target: values.target, targets: values.targets, turns: int(values.turns), segmentTurns: int(values["segment-turns"]),
    segmentMinutes: int(values["segment-minutes"]), keepOpen: values["keep-open"], python: values.python,
  };
  const [command, dirArg] = positionals;
  if (values.help || !command) return { ok: true, usage: HELP };
  if (command === "targets") {
    const loaded = targets();
    return { ok: true, default: loaded.default, targets: Object.values(loaded.targets).map(t => ({ name: t.name, ...t.harness, auth: t.backend.auth })) };
  }
  if (command === "probe") return cmdProbe();
  const controller = new AbortController();
  process.once("SIGINT", () => { say("stopping after this step (Ctrl+C again to force)"); controller.abort(); });
  if (command === "new") {
    if (!values.spec && !values.request) throw new Error("new 需要 --spec 或 --request");
    pickTarget(targets(), options.target);  // fail before a game folder is made
    const args = ["new"];
    if (values.spec) args.push("--spec", values.spec);
    if (values.request) args.push("--request", values.request);
    if (values.title) args.push("--title", values.title);
    const created = session(...args);
    // A start dialog the spec did not cover: the agent answers it in the first segment.
    if (!created.ok && !(created.dir && created.status === "failed")) return created;
    say(`game folder ${created.dir} (${created.ok ? created.status : `stopped: ${created.reason}`})`);
    return playSession(created.dir, controller.signal);
  }
  if (command === "resume") {
    if (!dirArg || !existsSync(dirArg)) throw new Error("resume 需要游戏局目录");
    const dir = path.resolve(dirArg);
    const resumed = session("resume", dir);
    if (!resumed.ok) return resumed;
    return playSession(dir, controller.signal);
  }
  throw new Error(`没有命令 ${command}\n${HELP}`);
}

main(process.argv.slice(2)).then(
  result => { process.stdout.write(JSON.stringify(result) + "\n"); process.exitCode = result.ok ? 0 : 1; },
  error => { process.stdout.write(JSON.stringify({ ok: false, error: "runner", reason: error.message }) + "\n"); process.exitCode = 2; },
);
