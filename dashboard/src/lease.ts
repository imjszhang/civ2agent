import { mkdirSync, renameSync, rmSync, writeFileSync } from "node:fs";
import path from "node:path";

export function leasePath(gamesDir: string): string {
  return path.join(gamesDir, ".lease");
}

/** The live holder. civ2ctl allows a child only when CIV2AGENT_LEASE matches. */
export function writeLease(gamesDir: string, record: { pid: number; lease: string; client: string }): void {
  mkdirSync(gamesDir, { recursive: true });
  const file = leasePath(gamesDir);
  const tmp = `${file}.tmp`;
  writeFileSync(tmp, `${JSON.stringify(record)}\n`, "utf8");
  renameSync(tmp, file);
}

export function clearLease(gamesDir: string): void {
  rmSync(leasePath(gamesDir), { force: true });
}
