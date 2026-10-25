import { createReadStream } from "node:fs";
import { open, readdir, readFile, stat } from "node:fs/promises";
import readline from "node:readline";
import path from "node:path";
import type { CommandView, Piece, RuntimeEvent, SessionView, TokenCount } from "./types.js";
import { addUsage, projectLog, projectRuntime, usageFromLines } from "./project.js";

export type Tail = { path: string | null; offset: number; rest: string };

export function freshTail(): Tail {
  return { path: null, offset: 0, rest: "" };
}

/** Read bytes appended since the last call. A replaced file starts over. */
export async function readAppended(tail: Tail): Promise<string[]> {
  if (!tail.path) return [];
  let size: number;
  try { size = (await stat(tail.path)).size; } catch { return []; }
  if (size < tail.offset) { tail.offset = 0; tail.rest = ""; }
  if (size === tail.offset) return [];
  const handle = await open(tail.path, "r");
  try {
    const buf = Buffer.alloc(size - tail.offset);
    await handle.read(buf, 0, buf.length, tail.offset);
    tail.offset = size;
    const parts = (tail.rest + buf.toString("utf8")).split(/\r?\n/u);
    tail.rest = parts.pop() ?? "";
    return parts.filter(line => line.trim());
  } finally {
    await handle.close();
  }
}

export async function readActive(gamesDir: string): Promise<{ id: string; dir: string; since: string } | null> {
  try {
    const value = JSON.parse(await readFile(path.join(gamesDir, ".active"), "utf8")) as unknown;
    if (typeof value !== "object" || value === null) return null;
    const row = value as Record<string, unknown>;
    if (typeof row.dir !== "string" || typeof row.id !== "string") return null;
    return { id: row.id, dir: row.dir, since: typeof row.since === "string" ? row.since : "" };
  } catch {
    return null;
  }
}

export async function readSession(dir: string, lock: { id: string; since: string }): Promise<SessionView> {
  let title = lock.id, status = "";
  try {
    const value = JSON.parse(await readFile(path.join(dir, "session.json"), "utf8")) as Record<string, unknown>;
    if (typeof value.title === "string") title = value.title;
    if (typeof value.status === "string") status = value.status;
  } catch { /* The lock is enough to show the directory. */ }
  return { id: lock.id, dir, since: lock.since, title, status };
}

/** The events file of the run directory most recently written. */
export async function latestEvents(sessionDir: string): Promise<string | null> {
  let names: string[];
  try { names = await readdir(path.join(sessionDir, "runs")); } catch { return null; }
  let best: { mtime: number; file: string } | null = null;
  for (const name of names) {
    const file = path.join(sessionDir, "runs", name, "events.jsonl");
    try {
      const mtime = (await stat(file)).mtimeMs;
      if (!best || mtime >= best.mtime) best = { mtime, file };
    } catch { /* Not a finished-looking run directory yet. */ }
  }
  return best?.file ?? null;
}

const quoted = (block: string, key: string) => new RegExp(`"${key}"\\s*:\\s*"([^"\\\\]*)"`).exec(block)?.[1] ?? "";

/** Adapter, requested model and reasoning effort from the head of plan.json. The rest of that file embeds binaries. */
export async function readPlanBrief(eventsFile: string): Promise<{ adapter: string; target: string; requestedModel: string; effort: string } | null> {
  const file = path.join(path.dirname(eventsFile), "plan.json");
  let head = "";
  try {
    const handle = await open(file, "r");
    try {
      const buf = Buffer.alloc(131072);
      const { bytesRead } = await handle.read(buf, 0, buf.length, 0);
      head = buf.toString("utf8", 0, bytesRead);
    } finally {
      await handle.close();
    }
  } catch {
    return null;
  }
  const targetAt = /"target"\s*:\s*\{/.exec(head);
  if (!targetAt) return null;
  const slice = head.slice(targetAt.index, targetAt.index + 4000);
  const harness = /"harness"\s*:\s*\{([^}]*)\}/.exec(slice)?.[1] ?? "";
  const backend = /"backend"\s*:\s*\{([^}]*)\}/.exec(slice)?.[1] ?? "";
  return {
    adapter: quoted(harness, "adapter"),
    target: /"name"\s*:\s*"([^"]*)"/.exec(slice)?.[1] ?? "",
    requestedModel: quoted(backend, "model"),
    effort: quoted(backend, "reasoningEffort"),
  };
}

async function usageLines(file: string): Promise<string[]> {
  const lines: string[] = [];
  const stream = readline.createInterface({ input: createReadStream(file, { encoding: "utf8" }), crlfDelay: Infinity });
  for await (const line of stream) {
    if (line.includes('"type":"usage"') || line.includes('"type":"started"')) lines.push(line);
  }
  return lines;
}

/** Sum of usage in every run of this session except the one currently being followed. */
export async function earlierUsage(sessionDir: string, current: string | null): Promise<TokenCount | null> {
  let names: string[];
  try { names = await readdir(path.join(sessionDir, "runs")); } catch { return null; }
  const skip = current ? path.resolve(current) : null;
  let sum: TokenCount | null = null;
  for (const name of names) {
    const file = path.join(sessionDir, "runs", name, "events.jsonl");
    if (skip && path.resolve(file) === skip) continue;
    let lines: string[];
    try { lines = await usageLines(file); } catch { continue; }
    const usage = usageFromLines(lines);
    if (usage) sum = sum ? addUsage(sum, usage) : usage;
  }
  return sum;
}

export function piecesFromRuntime(line: string): Piece[] {
  try {
    const value = JSON.parse(line) as RuntimeEvent;
    if (!value || value.schemaVersion !== 1 || typeof value.type !== "string") return [];
    return projectRuntime(value);
  } catch {
    return [];
  }
}

export function commandFromLog(line: string): CommandView | null {
  try { return projectLog(JSON.parse(line)); } catch { return null; }
}
