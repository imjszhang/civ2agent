import assert from "node:assert/strict";
import { mkdir, mkdtemp, readFile, rm, writeFile } from "node:fs/promises";
import { tmpdir } from "node:os";
import path from "node:path";
import test from "node:test";
import { packGame, unpackGame, zipFiles } from "../src/pack.js";

test("a pack round-trips a game and leaves the user's backup out", async () => {
  const root = await mkdtemp(path.join(tmpdir(), "civ2-pack-"));
  const source = path.join(root, "from");
  const games = path.join(root, "games");
  const id = "kept";
  await mkdir(path.join(source, "backup"), { recursive: true });
  await mkdir(path.join(source, "saves"), { recursive: true });
  await mkdir(games, { recursive: true });
  await writeFile(path.join(source, "session.json"), JSON.stringify({ id, title: "样例", status: "paused" }));
  await writeFile(path.join(source, "agent-note.txt"), "笔记");
  await writeFile(path.join(source, "saves", "T0001.SAV"), Buffer.from("save"));
  await writeFile(path.join(source, "backup", "secret.txt"), "用户的");
  try {
    const packed = await packGame(source);
    assert.equal(packed.files, 3);
    const imported = await unpackGame(games, packed.zip, null);
    assert.equal(imported.ok, true);
    assert.equal(imported.id, id);
    assert.equal(await readFile(path.join(games, id, "agent-note.txt"), "utf8"), "笔记");
    assert.equal((await readFile(path.join(games, id, "saves", "T0001.SAV"))).toString(), "save");
    await assert.rejects(() => readFile(path.join(games, id, "backup", "secret.txt")));
    assert.equal((await unpackGame(games, packed.zip, null)).ok, false);
    assert.equal((await unpackGame(games, packed.zip, id)).error, "usage");
    const slipped = zipFiles([{ name: "../outside.txt", data: Buffer.from("no") }, { name: "session.json", data: Buffer.from(JSON.stringify({ id: "other" })) }]);
    const refused = await unpackGame(games, slipped, null);
    assert.equal(refused.ok, false);
    await assert.rejects(() => readFile(path.join(root, "outside.txt")));
  } finally {
    await rm(root, { recursive: true, force: true });
  }
});
