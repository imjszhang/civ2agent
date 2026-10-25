// Pure parts of the runner: targets, prompts and event lines. No processes.
import { readFileSync } from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";
import { listAdapters, validateTarget } from "js-agent-runtime";

export const RUNNER_DIR = path.dirname(fileURLToPath(import.meta.url));
export const AGENT_ROOT = path.dirname(RUNNER_DIR);
export const CLI = {
  ctl: path.join(AGENT_ROOT, "cli", "civ2ctl.py"),
  session: path.join(AGENT_ROOT, "cli", "civ2session.py"),
  protocol: path.join(AGENT_ROOT, "protocol.md"),
};

/** Named harness targets from a targets.json: { default?, targets: { name: { harness, backend } } }. */
export function loadTargets(file) {
  let raw;
  try {
    raw = JSON.parse(readFileSync(file, "utf8"));
  } catch (cause) {
    throw new Error(`Cannot read targets file ${file}: ${cause.message}`);
  }
  if (!raw || typeof raw !== "object" || !raw.targets || typeof raw.targets !== "object" || Array.isArray(raw.targets))
    throw new Error(`${file} must contain an object "targets"`);
  const known = new Set(listAdapters().map(a => a.id));
  const targets = {};
  for (const [name, value] of Object.entries(raw.targets)) {
    const target = validateTarget({ name, ...value });
    if (!known.has(target.harness.adapter))
      throw new Error(`Target ${name}: unknown adapter ${target.harness.adapter} (${[...known].join(", ")})`);
    targets[name] = target;
  }
  if (raw.default !== undefined && !targets[raw.default])
    throw new Error(`Default target ${raw.default} is not in ${file}`);
  return { default: raw.default ?? Object.keys(targets)[0], targets };
}

export function pickTarget(loaded, name) {
  const chosen = name ?? loaded.default;
  const target = loaded.targets[chosen];
  if (!target) throw new Error(`No target ${chosen}; choose one of ${Object.keys(loaded.targets).join(", ")}`);
  return target;
}

/** What this stretch of play is for, as context.md states it. */
export function segmentGoal(context, untilTurn) {
  if (context.status === "setup")
    return "这局还没开局。按 brief.md 里用户的要求写 ../spec.json，运行 setup 开局，然后接着玩：" +
      `玩到第 ${untilTurn} 回合开始为止。`;
  if (context.status === "failed")
    return "开局停在一个 spec 没写到的对话框上（原因见下面「开局停在」）。用 dialog 看它，按用户的要求用 respond 回答，" +
      `再运行 setup <这局的目录> 接着开局，然后玩到第 ${untilTurn} 回合开始为止。`;
  return `从第 ${context.turn} 回合接着玩，玩到第 ${untilTurn} 回合开始为止（游戏先结束就停下）。`;
}

/** The prompt of one segment: where the tools are and what to read first. */
export function buildPrompt({ sessionDir, python }) {
  const py = python.includes(" ") ? `"${python}"` : python;
  return [
    "你在玩一局《文明 II》，是这局唯一的玩家。",
    "",
    `- 游戏局目录：${sessionDir}`,
    `- 操作游戏：${py} "${CLI.ctl}" <命令>（命令列表：${py} "${CLI.ctl}" tools）`,
    `- 存盘点、开局：${py} "${CLI.session}" checkpoint、setup <目录>`,
    `- 全部命令的说明：${CLI.protocol}`,
    "",
    "先读当前目录里的 playbook.md（规则和每回合的步骤），再读 context.md（这一段的目标）、brief.md（用户的要求）和 notes.md（你之前的笔记）。",
    "然后按 context.md 的目标玩。结束前更新 notes.md，最后用一两句中文说明这一段做了什么。",
  ].join("\n");
}

const clip = (text, n = 140) => {
  const flat = String(text ?? "").replace(/\s+/gu, " ").trim();
  return flat.length > n ? `${flat.slice(0, n)}…` : flat;
};

/** One short line for a runtime event worth showing, or null. */
export function describeEvent(event) {
  const d = event.data ?? {};
  switch (event.type) {
    case "started":
      return `started ${d.target ?? ""} (${d.adapter?.id ?? ""})`;
    case "tool": {
      if (d.type === "tool_result" || d.phase === "item.updated") return null;
      const item = d.item ?? d;
      if (item.type === "tool_use" || d.phase === "started") {
        const input = item.input ?? {};
        return `tool ${item.name ?? "?"} ${clip(input.command ?? input.file_path ?? input.pattern ?? JSON.stringify(input), 120)}`;
      }
      if (item.type === "command_execution") return d.phase === "item.started" ? `tool shell ${clip(item.command, 120)}` : null;
      return null;
    }
    case "message":
      return d.delta === undefined && d.text ? `say ${clip(d.text)}` : null;
    case "usage":
      return d.totalTokens !== undefined ? `usage ${d.totalTokens} tokens` : null;
    case "diagnostic":
      return `stderr ${clip(d.text)}`;
    case "completed":
    case "failed":
    case "cancelled":
    case "timed_out":
      return `${event.type}${d.error ? ` ${clip(d.error)}` : ""}`;
    default:
      return null;
  }
}

/** The JSON object on the last non-empty line of a civ2 command's output. */
export function lastJson(stdout) {
  const lines = String(stdout).split(/\r?\n/u).filter(line => line.trim());
  if (!lines.length) throw new Error("command printed nothing");
  return JSON.parse(lines[lines.length - 1]);
}
