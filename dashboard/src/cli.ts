import { spawn } from "node:child_process";

export type CliResult = Record<string, unknown>;

export function lastJson(stdout: string): CliResult | null {
  const line = stdout.split(/\r?\n/u).filter(item => item.trim()).at(-1);
  if (!line) return null;
  try {
    const value = JSON.parse(line) as unknown;
    return typeof value === "object" && value !== null ? value as CliResult : null;
  } catch {
    return null;
  }
}

/** Run a civ2ctl / civ2session command. The last stdout line is its JSON result. */
export function runCli(python: string, script: string, args: string[], timeoutMs: number, env?: NodeJS.ProcessEnv): Promise<CliResult> {
  return new Promise(resolve => {
    const child = spawn(python, [script, ...args], { windowsHide: true, stdio: ["ignore", "pipe", "pipe"], env });
    let out = "";
    let err = "";
    let settled = false;
    const finish = (value: CliResult) => {
      if (settled) return;
      settled = true;
      clearTimeout(timer);
      resolve(value);
    };
    const timer = setTimeout(() => {
      child.kill();
      finish({ ok: false, error: "timeout", reason: "网关等待超时" });
    }, timeoutMs);
    child.stdout.on("data", (chunk: Buffer) => { out += chunk.toString("utf8"); });
    child.stderr.on("data", (chunk: Buffer) => { err += chunk.toString("utf8"); });
    child.on("error", () => finish({ ok: false, error: "not_connected", reason: "没有找到 Python" }));
    child.on("close", () => {
      const parsed = lastJson(out);
      if (parsed) finish(parsed);
      else finish({ ok: false, error: "cli_fault", reason: err.trim().slice(0, 200) || "命令没有输出" });
    });
  });
}
