import { randomUUID } from "node:crypto";
import type { CliResult } from "./cli.js";

export type GateLast = { command: string; ok: boolean; reason: string };

export type GateView = {
  holder: string | null;
  command: string | null;
  since: string | null;
  last: GateLast | null;
};

const LABELS: Record<string, string> = {
  start_game: "开局",
  resume_game: "续玩",
  checkpoint: "存盘",
  close_game: "收尾",
};

type Holder = { lease: string; client: string; since: string; touched: number };
type Current = { command: string; client: string; since: string };

/** One writer at a time. A second mutation is rejected; it does not wait. */
export class Gate {
  private holder: Holder | null = null;
  private current: Current | null = null;
  private last: GateLast | null = null;
  private observers = 0;

  constructor(private readonly idleMs: number, private readonly now: () => number = Date.now) {}

  private stamp(): string {
    return new Date(this.now()).toISOString();
  }

  private expire(): void {
    if (this.holder && !this.current && this.now() - this.holder.touched >= this.idleMs) this.holder = null;
  }

  private busyReason(): string {
    if (this.current) {
      const label = LABELS[this.current.command];
      return label ? `${this.current.client} 正在${label}` : `${this.current.client} 正在执行 ${this.current.command}`;
    }
    if (this.observers > 0) return "正在读取局面";
    return "网关正忙";
  }

  /** Why a mutation holds the pipe, or null when a read may start. */
  activity(): string | null {
    this.expire();
    return this.current ? this.busyReason() : null;
  }

  view(): GateView {
    this.expire();
    return {
      holder: this.holder?.client ?? null,
      command: this.current?.command ?? null,
      since: this.current?.since ?? null,
      last: this.last,
    };
  }

  claim(client: string): CliResult {
    this.expire();
    const name = client.trim().slice(0, 80);
    if (!name) return { ok: false, error: "usage", reason: "claim 需要调用方的名字" };
    if (this.holder) return { ok: false, error: "busy", reason: `租约在 ${this.holder.client} 手里` };
    const since = this.stamp();
    const lease = randomUUID();
    this.holder = { lease, client: name, since, touched: this.now() };
    return { ok: true, lease, client: name, since };
  }

  release(lease: string): CliResult {
    this.expire();
    if (!this.holder || this.holder.lease !== lease) return { ok: false, error: "illegal", reason: "没有这张租约" };
    if (this.current || this.observers > 0) return { ok: false, error: "busy", reason: this.busyReason() };
    this.holder = null;
    return { ok: true };
  }

  /** Take the write lock. The caller must `end` it, including when the command fails. */
  begin(command: string, lease: string): CliResult {
    this.expire();
    if (!this.holder || this.holder.lease !== lease) return { ok: false, error: "illegal", reason: "没有这张租约，先 claim" };
    if (this.current || this.observers > 0) return { ok: false, error: "busy", reason: this.busyReason() };
    this.holder.touched = this.now();
    this.current = { command, client: this.holder.client, since: this.stamp() };
    return { ok: true };
  }

  end(command: string, result: CliResult): void {
    this.current = null;
    if (this.holder) this.holder.touched = this.now();
    const reason = String(result.reason ?? (result.ok === true ? "完成" : result.error ?? ""));
    this.last = { command, ok: result.ok === true, reason };
  }

  /**
   * Reserve the pipe for a read. A mutation already in progress returns its reason
   * and does not count an observer, so the caller must not spawn a command.
   */
  beginObserve(): string | null {
    this.expire();
    if (this.current) return this.busyReason();
    this.observers += 1;
    return null;
  }

  endObserve(): void {
    this.observers = Math.max(0, this.observers - 1);
  }
}

export async function runMutation(
  gate: Gate,
  command: string,
  lease: string,
  run: () => Promise<CliResult>,
  onActivity?: () => void,
): Promise<CliResult> {
  const begun = gate.begin(command, lease);
  if (begun.ok !== true) return begun;
  onActivity?.();
  let result: CliResult;
  try {
    result = await run();
  } catch (cause) {
    result = { ok: false, error: "cli_fault", reason: cause instanceof Error ? cause.message : "命令失败" };
  }
  gate.end(command, result);
  onActivity?.();
  return result;
}

export async function whenIdle<T>(gate: Gate, read: () => Promise<T>, skipped: (reason: string) => T): Promise<T> {
  const reason = gate.beginObserve();
  if (reason) return skipped(reason);
  try {
    return await read();
  } finally {
    gate.endObserve();
  }
}
