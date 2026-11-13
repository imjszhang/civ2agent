import assert from "node:assert/strict";
import { mkdtempSync, rmSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import path from "node:path";
import test from "node:test";
import { loadDotenv } from "../../dotenv.mjs";

const KEYS = ["CIV2_DOTENV_EXPORT", "CIV2_DOTENV_PLAIN", "CIV2_DOTENV_SINGLE", "CIV2_DOTENV_QUOTED",
  "CIV2_DOTENV_COMMENT", "CIV2_DOTENV_ESC", "CIV2_DOTENV_OPEN", "CIV2_DOTENV_KEEP", "CIV2AGENT_LEASE"];

test("a .env file fills only unset keys and keeps quotes", () => {
  const saved = Object.fromEntries(KEYS.map(key => [key, process.env[key]]));
  for (const key of KEYS) delete process.env[key];
  process.env.CIV2_DOTENV_KEEP = "from-process";
  const dir = mkdtempSync(path.join(tmpdir(), "civ2-dotenv-"));
  const file = path.join(dir, ".env");
  try {
    writeFileSync(file, [
      "# comment",
      "export CIV2_DOTENV_EXPORT=from-export",
      "CIV2_DOTENV_PLAIN=D:\\Games\\Civilization II",
      "CIV2_DOTENV_SINGLE='D:\\\\Games'",
      'CIV2_DOTENV_QUOTED="D:\\\\civ2 games"',
      "CIV2_DOTENV_COMMENT=4732 # port",
      'CIV2_DOTENV_ESC="D:\\Games"',
      "CIV2AGENT_LEASE=nope",
      "CIV2_DOTENV_KEEP=from-file",
      'CIV2_DOTENV_OPEN="nope',
      "not a pair",
      "=missing",
      "",
    ].join("\n"));
    assert.deepEqual(loadDotenv(file), [
      "CIV2_DOTENV_EXPORT", "CIV2_DOTENV_PLAIN", "CIV2_DOTENV_SINGLE",
      "CIV2_DOTENV_QUOTED", "CIV2_DOTENV_COMMENT", "CIV2_DOTENV_ESC",
    ]);
    assert.equal(process.env.CIV2_DOTENV_EXPORT, "from-export");
    assert.equal(process.env.CIV2_DOTENV_PLAIN, "D:\\Games\\Civilization II");
    assert.equal(process.env.CIV2_DOTENV_SINGLE, "D:\\\\Games");
    assert.equal(process.env.CIV2_DOTENV_QUOTED, "D:\\civ2 games");
    assert.equal(process.env.CIV2_DOTENV_COMMENT, "4732");
    assert.equal(process.env.CIV2_DOTENV_ESC, "D:\\Games");
    assert.equal(process.env.CIV2_DOTENV_KEEP, "from-process");
    assert.equal(process.env.CIV2AGENT_LEASE, undefined);
    assert.equal(process.env.CIV2_DOTENV_OPEN, undefined);
    assert.deepEqual(loadDotenv(file), []);
    assert.deepEqual(loadDotenv(path.join(dir, "missing")), []);
  } finally {
    for (const key of KEYS) {
      if (saved[key] === undefined) delete process.env[key];
      else process.env[key] = saved[key];
    }
    rmSync(dir, { recursive: true, force: true });
  }
});
