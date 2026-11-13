import { readdir, readFile } from "node:fs/promises";
import path from "node:path";
import type { CliResult } from "./cli.js";
import { openGame } from "./session.js";

const PARTS = ["index", "turns", "notes", "strategy", "snapshot", "runs", "run", "log", "brief", "rule"] as const;
type Part = typeof PARTS[number];

const TEXT_CAP = 60_000;

export type RecordQuery = {
  id: string;
  part?: string;
  turn?: number;
  run?: string;
  offset?: number;
  limit?: number;
};

function usage(reason: string): CliResult {
  return { ok: false, error: "usage", reason };
}

function turnTag(turn: number): string | null {
  if (!Number.isInteger(turn) || turn < 0 || turn > 9999) return null;
  return `T${String(turn).padStart(4, "0")}`;
}

function runNameOk(name: string): boolean {
  return /^s\d{3}-[A-Za-z0-9._-]+$/u.test(name);
}

async function readText(file: string): Promise<string | null> {
  try {
    return await readFile(file, "utf8");
  } catch {
    return null;
  }
}

function jsonLines(text: string | null): unknown[] {
  if (!text) return [];
  const rows: unknown[] = [];
  for (const line of text.split(/\r?\n/u)) {
    if (!line.trim()) continue;
    try { rows.push(JSON.parse(line) as unknown); }
    catch { rows.push({ raw: line.slice(0, 200) }); }
  }
  return rows;
}

function windowOf<T>(items: T[], offset: number | undefined, limit: number | undefined, fallback: number, max: number): { rows: T[]; total: number; offset: number } {
  const cap = Math.min(Math.max(limit ?? fallback, 1), max);
  const start = offset === undefined ? Math.max(0, items.length - cap) : Math.max(0, Math.floor(offset));
  return { rows: items.slice(start, start + cap), total: items.length, offset: start };
}

function clip(text: string, n: number): string {
  const flat = text.replace(/\s+/gu, " ").trim();
  return flat.length > n ? `${flat.slice(0, n)}…` : flat;
}

function record(value: unknown): Record<string, unknown> | null {
  return typeof value === "object" && value !== null && !Array.isArray(value) ? value as Record<string, unknown> : null;
}

function logOutline(row: unknown): Record<string, unknown> {
  const item = record(row);
  if (!item) return { raw: String(row).slice(0, 200) };
  const response = record(item.response) ?? {};
  const reason = typeof response.reason === "string" ? response.reason.slice(0, 200) : undefined;
  return {
    time: item.time, command: item.command, ok: response.ok, error: response.error, reason,
    turn: response.turn, year: response.year,
  };
}

async function names(dir: string, suffix: string): Promise<string[]> {
  try {
    const found = await readdir(dir);
    return found.filter(name => name.toLowerCase().endsWith(suffix)).sort();
  } catch {
    return [];
  }
}

/** Read one review slice of a game folder. Saves are names only. No pipe. */
export async function readRecord(gamesDir: string, query: RecordQuery): Promise<CliResult> {
  const found = openGame(gamesDir, query.id);
  if (found.ok !== true) return found;
  const part = (query.part ?? "index") as Part;
  if (!PARTS.includes(part)) return usage(`part 只能是 ${PARTS.join("、")}`);
  const dir = found.dir;
  if (part === "index") return index(dir);
  if (part === "turns") return turns(dir, query.offset, query.limit);
  if (part === "notes") return markdown(dir, "agent/notes.md", "notes");
  if (part === "brief") return markdown(dir, "brief.md", "brief");
  if (part === "rule") return markdown(dir, "agent/rule.md", "rule");
  if (part === "strategy") return strategy(dir, query.turn);
  if (part === "snapshot") return snapshot(dir, query.turn);
  if (part === "runs") return runs(dir);
  if (part === "run") return run(dir, query.run);
  return log(dir, query.offset, query.limit);
}

async function index(dir: string): Promise<CliResult> {
  const session = record(JSON.parse(await readFile(path.join(dir, "session.json"), "utf8")));
  if (!session) return usage("session.json 不是对象");
  const turnsText = await readText(path.join(dir, "turns.jsonl"));
  const logText = await readText(path.join(dir, "log.jsonl"));
  const runsText = await readText(path.join(dir, "runs.jsonl"));
  const runRows = jsonLines(runsText).map(row => {
    const item = record(row) ?? {};
    const summary = typeof item.summary === "string" ? clip(item.summary, 180) : "";
    return {
      segment: item.segment, target: item.target, status: item.status,
      turn_before: item.turn_before, turn_after: item.turn_after, summary,
    };
  });
  const identity = record(session.identity);
  return {
    ok: true,
    id: session.id, title: session.title, status: session.status,
    created: session.created, updated: session.updated, closed: session.closed,
    turn: session.turn, year: session.year, latest_save: session.latest_save,
    civ: identity?.civ, leader: identity?.leader,
    spec: session.spec, request: session.request, guide: session.guide, rule: session.rule,
    segments: session.segments,
    saves: Array.isArray(session.saves) ? session.saves : [],
    turns: jsonLines(turnsText).length,
    snapshots: await names(path.join(dir, "snapshots"), ".json"),
    strategies: await names(path.join(dir, "agent", "strategy"), ".md"),
    notes: (await readText(path.join(dir, "agent", "notes.md"))) !== null,
    runs: runRows,
    log_lines: jsonLines(logText).length,
  };
}

async function turns(dir: string, offset: number | undefined, limit: number | undefined): Promise<CliResult> {
  const rows = jsonLines(await readText(path.join(dir, "turns.jsonl")));
  const page = windowOf(rows, offset, limit, 40, 200);
  return { ok: true, ...page };
}

async function markdown(dir: string, relative: string, field: string): Promise<CliResult> {
  const text = await readText(path.join(dir, relative));
  if (text === null) return usage(`没有 ${relative}`);
  const truncated = text.length > TEXT_CAP;
  return { ok: true, [field]: truncated ? text.slice(0, TEXT_CAP) : text, truncated };
}

async function strategy(dir: string, turn: number | undefined): Promise<CliResult> {
  if (turn === undefined) return markdown(dir, "agent/strategy.md", "strategy");
  const tag = turnTag(turn);
  if (!tag) return usage("turn 是 0 到 9999 的整数");
  return markdown(dir, path.join("agent", "strategy", `${tag}.md`), "strategy");
}

async function snapshot(dir: string, turn: number | undefined): Promise<CliResult> {
  if (turn === undefined) return usage("snapshot 需要 turn");
  const tag = turnTag(turn);
  if (!tag) return usage("turn 是 0 到 9999 的整数");
  const text = await readText(path.join(dir, "snapshots", `${tag}.json`));
  if (text === null) return usage(`没有快照 ${tag}`);
  try {
    return { ok: true, turn, snapshot: JSON.parse(text) as unknown };
  } catch {
    return usage(`快照 ${tag} 不是 JSON`);
  }
}

async function runs(dir: string): Promise<CliResult> {
  return { ok: true, runs: jsonLines(await readText(path.join(dir, "runs.jsonl"))) };
}

async function run(dir: string, name: string | undefined): Promise<CliResult> {
  if (!name || !runNameOk(name)) return usage("run 是 runs 下的一段目录名，如 s001-20261113121338-cursor");
  const text = await readText(path.join(dir, "runs", name, "result.json"));
  if (text === null) return usage(`没有 ${name}/result.json`);
  try {
    return { ok: true, run: name, result: JSON.parse(text) as unknown };
  } catch {
    return usage(`${name}/result.json 不是 JSON`);
  }
}

async function log(dir: string, offset: number | undefined, limit: number | undefined): Promise<CliResult> {
  const rows = jsonLines(await readText(path.join(dir, "log.jsonl"))).map(logOutline);
  const page = windowOf(rows, offset, limit, 40, 100);
  return { ok: true, ...page };
}
