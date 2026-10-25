import { spawn } from "node:child_process";
import type { CityBrief, CivBrief, DialogBrief, DiplomacyBrief, GameView, UnitBrief } from "./types.js";

const EMPTY: GameView = {
  connected: false, busy: false, reason: "游戏没有开", inGame: false, turn: null, year: null,
  civ: null, cities: [], units: [], diplomacy: [], dialog: null, cityWindow: null, taxWindow: null,
};

function lastJson(stdout: string): Record<string, unknown> | null {
  const line = stdout.split(/\r?\n/u).filter(item => item.trim()).at(-1);
  if (!line) return null;
  try {
    const value = JSON.parse(line) as unknown;
    return typeof value === "object" && value !== null ? value as Record<string, unknown> : null;
  } catch {
    return null;
  }
}

function call(python: string, ctl: string, args: string[], timeoutMs: number): Promise<Record<string, unknown>> {
  return new Promise(resolve => {
    const child = spawn(python, [ctl, "--timeout", "2", ...args], { windowsHide: true, stdio: ["ignore", "pipe", "pipe"] });
    let out = "";
    let settled = false;
    const finish = (value: Record<string, unknown>) => { if (!settled) { settled = true; clearTimeout(timer); resolve(value); } };
    const timer = setTimeout(() => { child.kill(); finish({ ok: false, error: "timeout", reason: "网关等待超时" }); }, timeoutMs);
    child.stdout.on("data", (chunk: Buffer) => { out += chunk.toString("utf8"); });
    child.on("error", () => finish({ ok: false, error: "not_connected", reason: "没有找到 Python" }));
    child.on("close", () => finish(lastJson(out) ?? { ok: false, error: "cli_fault", reason: "命令没有输出" }));
  });
}

function asList<T>(value: unknown): T[] {
  return Array.isArray(value) ? value as T[] : [];
}

/** One read-only look at the game. A busy pipe keeps the previous picture and sets busy. */
export async function readGame(python: string, ctl: string, previous: GameView | null): Promise<GameView> {
  const status = await call(python, ctl, ["status"], 4000);
  if (status.ok !== true) {
    const unreachable = status.error === "not_connected" || status.error === "timeout";
    if (unreachable && previous?.connected) return { ...previous, busy: true, reason: String(status.reason ?? "游戏正忙") };
    return { ...EMPTY, reason: String(status.reason ?? EMPTY.reason) };
  }
  const state = await call(python, ctl, ["state", "--only", "civ,cities,units,diplomacy,dialog"], 8000);
  if (state.ok !== true) {
    return {
      ...EMPTY, connected: true, busy: true, inGame: status.in_game === true,
      reason: String(state.reason ?? "局面暂时读不到"),
      cityWindow: (status.city_window as GameView["cityWindow"]) ?? null,
      taxWindow: (status.tax_window as GameView["taxWindow"]) ?? null,
    };
  }
  return {
    connected: true, busy: false, reason: "", inGame: state.in_game === true,
    turn: typeof state.turn === "number" ? state.turn : null,
    year: typeof state.year === "number" ? state.year : null,
    civ: (state.civ as CivBrief) ?? null,
    cities: asList<CityBrief>(state.cities),
    units: asList<UnitBrief>(state.units),
    diplomacy: asList<DiplomacyBrief>(state.diplomacy),
    dialog: (state.dialog as DialogBrief) ?? null,
    cityWindow: (state.city_window as GameView["cityWindow"]) ?? null,
    taxWindow: (state.tax_window as GameView["taxWindow"]) ?? null,
  };
}
