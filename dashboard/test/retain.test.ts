import assert from "node:assert/strict";
import test from "node:test";
import { KEEP_COMMANDS, THOUGHT_CHARS, retainCommands, retainThoughts, retainThoughtText } from "../src/retain.js";
import type { CommandView, ThoughtView } from "../src/types.js";

test("a list keeps the newest items and drops the oldest", () => {
  const commands: CommandView[] = Array.from({ length: KEEP_COMMANDS + 5 }, (_, index) => ({
    time: `2026-10-30T12:00:${String(index).padStart(2, "0")}`,
    command: `cmd-${index}`,
    ok: true,
    error: "",
    reason: "",
  }));
  retainCommands(commands);
  assert.equal(commands.length, KEEP_COMMANDS);
  assert.equal(commands[0]?.command, "cmd-5");
  assert.equal(commands.at(-1)?.command, `cmd-${KEEP_COMMANDS + 4}`);
});

test("a long thought keeps its ending", () => {
  const text = `${"前".repeat(THOUGHT_CHARS)}结尾`;
  assert.equal(retainThoughtText(text).endsWith("结尾"), true);
  assert.equal(retainThoughtText(text).length, THOUGHT_CHARS);
  const thoughts: ThoughtView[] = [{ text, done: false }];
  retainThoughts(thoughts);
  assert.equal(thoughts[0]?.text.endsWith("结尾"), true);
  assert.equal(thoughts[0]?.text.length, THOUGHT_CHARS);
});
