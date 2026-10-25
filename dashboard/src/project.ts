import type { CommandView, Piece, RunPatch, RunView, RuntimeEvent, TokenCount } from "./types.js";

const clip = (text: string, n = 160) => {
  const flat = text.replace(/\s+/gu, " ").trim();
  return flat.length > n ? `${flat.slice(0, n)}…` : flat;
};

function record(value: unknown): Record<string, unknown> | null {
  return typeof value === "object" && value !== null && !Array.isArray(value) ? value as Record<string, unknown> : null;
}

function textOf(value: unknown): string {
  if (typeof value === "string") return value;
  const row = record(value);
  if (!row) return "";
  if (typeof row.text === "string") return row.text;
  const message = record(row.message);
  const content = message?.content ?? row.content;
  if (typeof content === "string") return content;
  if (!Array.isArray(content)) return "";
  return content.map(part => {
    if (typeof part === "string") return part;
    const item = record(part);
    return typeof item?.text === "string" ? item.text : "";
  }).join("");
}

function count(value: unknown): number | null {
  return typeof value === "number" && Number.isFinite(value) && value >= 0 ? value : null;
}

/** Token fields from a runtime usage event. Cursor names cache fields differently and omits a total. */
export function parseUsage(data: Record<string, unknown>): TokenCount | null {
  const input = count(data.inputTokens);
  const output = count(data.outputTokens);
  if (input === null || output === null) return null;
  const cached = count(data.cachedInputTokens) ?? count(data.cacheReadTokens);
  const cacheWrite = count(data.cacheWriteTokens);
  const reasoning = count(data.reasoningTokens);
  const stated = count(data.totalTokens);
  const total = stated ?? input + output + (cached ?? 0) + (cacheWrite ?? 0);
  return { input, output, cached, cacheWrite, reasoning, total };
}

const addField = (left: number | null, right: number | null) =>
  left === null && right === null ? null : (left ?? 0) + (right ?? 0);

export function addUsage(left: TokenCount, right: TokenCount): TokenCount {
  return {
    input: left.input + right.input,
    output: left.output + right.output,
    cached: addField(left.cached, right.cached),
    cacheWrite: addField(left.cacheWrite, right.cacheWrite),
    reasoning: addField(left.reasoning, right.reasoning),
    total: left.total + right.total,
  };
}

/**
 * Codex and DeepSeek emit one usage per turn or step. Claude and Cursor emit one run total.
 * OpenCode emits steps and then a cumulative total; a sample that already covers the sum replaces it.
 */
export function foldUsage(adapter: string, current: TokenCount | null, next: TokenCount): TokenCount {
  if (!current) return next;
  if (adapter === "codex" || adapter === "deepseek-harness") return addUsage(current, next);
  if (adapter === "opencode") {
    const covers = next.total >= current.total && next.input >= current.input && next.output >= current.output;
    return covers ? next : addUsage(current, next);
  }
  return next;
}

export function usageFromLines(lines: Iterable<string>): TokenCount | null {
  let adapter = "";
  let usage: TokenCount | null = null;
  for (const line of lines) {
    if (!line.includes('"type":"usage"') && !line.includes('"type":"started"')) continue;
    let value: unknown;
    try { value = JSON.parse(line); } catch { continue; }
    const event = value as RuntimeEvent;
    if (!event || event.schemaVersion !== 1 || typeof event.type !== "string") continue;
    for (const piece of projectRuntime(event)) {
      if (piece.event === "run" && piece.data.adapter) adapter = piece.data.adapter;
      if (piece.event === "usage") usage = foldUsage(adapter, usage, piece.data);
    }
  }
  return usage;
}

function reportedModel(data: Record<string, unknown>): string | null {
  if (data.type === "system" && data.subtype === "init" && typeof data.model === "string") return data.model;
  if (data.type === "assistant") {
    const message = record(data.message);
    if (typeof message?.model === "string") return message.model;
  }
  if (data.type === "stream_event") {
    const stream = record(data.event);
    const message = record(stream?.message);
    if (stream?.type === "message_start" && typeof message?.model === "string") return message.model;
  }
  if (data.type === "result") {
    const models = record(data.modelUsage);
    if (models) {
      const names = Object.keys(models);
      if (names.length === 1) return names[0] ?? null;
      if (names.length > 1) return names.join("、");
    }
  }
  if (typeof data.model === "string" && data.type !== "user") return data.model;
  return null;
}

function reportedEffort(data: Record<string, unknown>): string | null {
  const own = data.reasoningEffort ?? data.reasoning_effort;
  return typeof own === "string" && own ? own : null;
}

const text = (next: string | undefined, prev: string) => next !== undefined && next !== "" ? next : prev;

export function mergeRun(previous: RunView | null, data: RunPatch): RunView {
  return {
    runId: data.runId || previous?.runId || "",
    status: data.status ?? previous?.status ?? "running",
    adapter: text(data.adapter, previous?.adapter ?? ""),
    target: text(data.target, previous?.target ?? ""),
    model: text(data.model, previous?.model ?? ""),
    requestedModel: text(data.requestedModel, previous?.requestedModel ?? ""),
    effort: data.effort !== undefined ? (data.effort || previous?.effort || "") : (previous?.effort ?? ""),
    usage: previous?.usage ?? null,
  };
}

function usageEqual(left: TokenCount | null, right: TokenCount | null): boolean {
  if (left === right) return true;
  if (!left || !right) return false;
  return left.input === right.input && left.output === right.output && left.cached === right.cached
    && left.cacheWrite === right.cacheWrite && left.reasoning === right.reasoning && left.total === right.total;
}

export function runChanged(previous: RunView | null, next: RunView): boolean {
  if (!previous) return true;
  return previous.runId !== next.runId || previous.status !== next.status || previous.adapter !== next.adapter
    || previous.target !== next.target || previous.model !== next.model || previous.requestedModel !== next.requestedModel
    || previous.effort !== next.effort || !usageEqual(previous.usage, next.usage);
}

/** One short line for a tool call the agent started. Completed calls repeat the same request. */
export function toolLine(data: Record<string, unknown>): string | null {
  if (data.type === "tool_result" || data.phase === "item.updated" || data.subtype === "completed") return null;
  const call = record(data.tool_call);
  if (call) {
    if (data.subtype !== undefined && data.subtype !== "started") return null;
    for (const [key, value] of Object.entries(call)) {
      if (!key.endsWith("ToolCall")) continue;
      const args = record(record(value)?.args) ?? {};
      const detail = [args.command, args.path, args.file_path, args.pattern].find(item => typeof item === "string");
      const name = key.replace(/ToolCall$/u, "");
      return clip(typeof detail === "string" ? `${name} ${detail}` : name);
    }
  }
  const item = record(data.item) ?? data;
  if (item.type === "tool_use" || data.phase === "started") {
    const input = record(item.input) ?? {};
    const detail = [input.command, input.file_path, input.pattern].find(part => typeof part === "string");
    const name = typeof item.name === "string" ? item.name : "tool";
    return clip(typeof detail === "string" ? `${name} ${detail}` : name);
  }
  if (item.type === "command_execution" && data.phase === "item.started" && typeof item.command === "string")
    return clip(`shell ${item.command}`);
  return null;
}

/** Page events for one runtime line. Raw tool calls are skipped: the runtime also emits type "tool". */
export function projectRuntime(event: RuntimeEvent): Piece[] {
  const data = record(event.data) ?? {};
  if (event.type === "started") {
    const adapter = record(data.adapter);
    return [{
      event: "run",
      data: {
        runId: event.runId,
        status: "running",
        adapter: typeof adapter?.id === "string" ? adapter.id : "",
        target: typeof data.target === "string" ? data.target : "",
      },
    }];
  }
  if (event.type === "completed" || event.type === "failed" || event.type === "cancelled" || event.type === "timed_out") {
    return [{ event: "run", data: { runId: event.runId, status: event.type } }];
  }
  if (event.type === "message") {
    if (data.delta !== undefined) return [];
    const text = textOf(data).trim();
    return text ? [{ event: "action", data: { kind: "say", text: clip(text, 500), time: event.time } }] : [];
  }
  if (event.type === "tool") {
    const text = toolLine(data);
    return text ? [{ event: "action", data: { kind: "tool", text, time: event.time } }] : [];
  }
  if (event.type === "usage") {
    const usage = parseUsage(data);
    return usage ? [{ event: "usage", data: usage }] : [];
  }
  if (event.type !== "raw") return [];
  const pieces: Piece[] = [];
  const model = reportedModel(data);
  const effort = reportedEffort(data);
  if (model || effort) {
    pieces.push({
      event: "run",
      data: { runId: event.runId, ...(model ? { model } : {}), ...(effort ? { effort } : {}) },
    });
  }
  if (data.type === "thinking" && data.subtype === "delta" && typeof data.text === "string")
    pieces.push({ event: "thought", data: { text: data.text, done: false } });
  else if (data.type === "thinking" && data.subtype === "completed")
    pieces.push({ event: "thought", data: { text: "", done: true } });
  else if (data.type === "user") {
    const text = textOf(data).trim();
    if (text) pieces.push({ event: "prompt", data: { text } });
  }
  return pieces;
}

export function projectLog(value: unknown): CommandView | null {
  const row = record(value);
  if (!row || typeof row.command !== "string" || typeof row.time !== "string") return null;
  const response = record(row.response) ?? {};
  return {
    time: row.time,
    command: row.command,
    ok: response.ok === true,
    error: typeof response.error === "string" ? response.error : "",
    reason: typeof response.reason === "string" ? response.reason : "",
  };
}
