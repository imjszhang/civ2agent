import { runCli, type CliResult } from "./cli.js";
import { sessionEnv } from "./session.js";

export type SchemaProp = {
  type?: string;
  description?: string;
  enum?: string[];
  default?: unknown;
};

export type CtlTool = {
  name: string;
  kind: string;
  description: string;
  input_schema: { properties?: Record<string, SchemaProp>; required?: string[] };
};

/** Session lifecycle and process control stay on the session tools. */
export const HIDDEN_TOOLS = new Set(["save-game", "load-game", "quit-game", "wait-ready", "skip-intro"]);

const MOUSE_NOTE = "部分对话框会移动本机鼠标，操作期间游戏窗口留在屏幕上。";

export function visiblePlayTools(tools: CtlTool[]): CtlTool[] {
  return tools.filter(tool => (tool.kind === "observe" || tool.kind === "act") && !HIDDEN_TOOLS.has(tool.name));
}

/** `events` consumes the game's queue, so only the lease holder may take it. */
export function needsLease(tool: CtlTool): boolean {
  return tool.kind === "act" || tool.name === "events";
}

export function toolDescription(tool: CtlTool): string {
  return tool.name === "respond" ? `${tool.description} ${MOUSE_NOTE}` : tool.description;
}

export function actTimeoutMs(args: Record<string, unknown>): number {
  const asked = typeof args.timeout_ms === "number" && Number.isFinite(args.timeout_ms) ? args.timeout_ms : 120_000;
  return Math.min(600_000, Math.max(150_000, asked + 30_000));
}

export function callArgs(name: string, args: Record<string, unknown>, connectSeconds: number): string[] {
  const rest = { ...args };
  delete rest.lease;
  return ["--timeout", String(connectSeconds), "call", name, JSON.stringify(rest)];
}

function isTool(value: unknown): value is CtlTool {
  if (typeof value !== "object" || value === null) return false;
  const tool = value as CtlTool;
  return typeof tool.name === "string" && typeof tool.kind === "string"
    && typeof tool.description === "string" && typeof tool.input_schema === "object" && tool.input_schema !== null;
}

export async function loadPlayTools(python: string, ctl: string, gamesDir: string): Promise<CtlTool[]> {
  const result: CliResult = await runCli(python, ctl, ["tools"], 8000, sessionEnv(gamesDir));
  if (result.ok !== true || !Array.isArray(result.tools)) return [];
  return visiblePlayTools(result.tools.filter(isTool));
}
