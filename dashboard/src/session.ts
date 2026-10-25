import { existsSync } from "node:fs";
import path from "node:path";
import { runCli, type CliResult } from "./cli.js";

export const CHECKPOINT_MS = 120_000;
export const SESSION_MS = 600_000;

const ID_REASON = "局 id 只能是 games 目录下的一层名字";

type GameDir = { ok: true; dir: string } | { ok: false; error: "usage"; reason: string };

/** Reject absolute paths, `..`, and any separator. A game id is one path segment. */
export function resolveGameDir(gamesDir: string, id: string): GameDir {
  if (typeof id !== "string" || id.length === 0 || id.length > 80
    || id === "." || id === ".." || id.includes("..")
    || id.includes("/") || id.includes("\\") || id.includes(":")
    || path.win32.isAbsolute(id) || path.posix.isAbsolute(id)) {
    return { ok: false, error: "usage", reason: ID_REASON };
  }
  const root = path.resolve(gamesDir);
  const dir = path.resolve(root, id);
  if (path.dirname(dir) !== root) return { ok: false, error: "usage", reason: ID_REASON };
  return { ok: true, dir };
}

export function hasSession(dir: string): boolean {
  return existsSync(path.join(dir, "session.json"));
}

/** A game id plus the session file. Missing files are not a game. */
export function openGame(gamesDir: string, id: string): GameDir {
  const found = resolveGameDir(gamesDir, id);
  if (found.ok !== true) return found;
  if (!hasSession(found.dir)) return { ok: false, error: "usage", reason: `${id} 不是游戏局目录（没有 session.json）` };
  return found;
}

export function sessionEnv(gamesDir: string): NodeJS.ProcessEnv {
  return { ...process.env, CIV2AGENT_GAMES_DIR: gamesDir };
}

export function runSession(python: string, script: string, gamesDir: string, args: string[], timeoutMs: number): Promise<CliResult> {
  return runCli(python, script, args, timeoutMs, sessionEnv(gamesDir));
}

export function startArgs(spec: unknown, title?: string, request?: string, id?: string): string[] {
  const args = ["new", "--spec", JSON.stringify(spec)];
  if (title) args.push("--title", title);
  if (request) args.push("--request", request);
  if (id) args.push("--id", id);
  return args;
}

export function closeArgs(status: "paused" | "ended", keepRunning: boolean): string[] {
  const args = ["close", "--status", status];
  if (keepRunning) args.push("--keep-running");
  return args;
}
