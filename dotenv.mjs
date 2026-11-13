// Fill unset environment variables from a KEY=VALUE file.
// Same rules as cli/civ2ctl.py load_dotenv. Existing variables stay.
// CIV2AGENT_LEASE is a one-call token and is never taken from the file.
import { readFileSync } from "node:fs";

const SKIP = new Set(["CIV2AGENT_LEASE"]);

function valueOf(raw) {
  if (raw.startsWith("'") || raw.startsWith('"')) {
    const quote = raw[0];
    let body = "";
    for (let i = 1; i < raw.length; i++) {
      const char = raw[i];
      if (quote === '"' && char === "\\" && i + 1 < raw.length) {
        const next = raw[++i];
        const mapped = { n: "\n", r: "\r", t: "\t", "\\": "\\", '"': '"' }[next];
        body += mapped === undefined ? "\\" + next : mapped;
        continue;
      }
      if (char === quote) {
        const rest = raw.slice(i + 1).trim();
        if (rest && !rest.startsWith("#")) return null;
        return body;
      }
      body += char;
    }
    return null;
  }
  const cut = raw.indexOf(" #");
  return (cut === -1 ? raw : raw.slice(0, cut)).trimEnd();
}

function assignment(line) {
  let text = line.trim();
  if (!text || text.startsWith("#")) return null;
  if (text.startsWith("export ")) text = text.slice(7).trimStart();
  const eq = text.indexOf("=");
  if (eq === -1) return null;
  const key = text.slice(0, eq).trim();
  if (!key || /^[0-9]/u.test(key) || !/^[A-Za-z0-9_]+$/u.test(key)) return null;
  const value = valueOf(text.slice(eq + 1).trimStart());
  if (value === null) return null;
  return [key, value];
}

/** @param {string} file @returns {string[]} keys that were set */
export function loadDotenv(file) {
  let text;
  try {
    text = readFileSync(file, "utf8");
  } catch (cause) {
    if (cause && cause.code === "ENOENT") return [];
    const message = cause instanceof Error ? cause.message : String(cause);
    throw new Error(`.env 读不了：${message}`);
  }
  if (text.charCodeAt(0) === 0xfeff) text = text.slice(1);
  const applied = [];
  for (const line of text.split("\n")) {
    const parsed = assignment(line.endsWith("\r") ? line.slice(0, -1) : line);
    if (!parsed) continue;
    const [key, value] = parsed;
    if (SKIP.has(key) || process.env[key] !== undefined) continue;
    process.env[key] = value;
    applied.push(key);
  }
  return applied;
}
