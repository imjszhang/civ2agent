import { deflateRawSync, inflateRawSync } from "node:zlib";
import { mkdir, readdir, readFile, rm, writeFile } from "node:fs/promises";
import path from "node:path";
import type { CliResult } from "./cli.js";
import { resolveGameDir } from "./session.js";

export const PACK_LIMIT = 128 * 1024 * 1024;

type Entry = { name: string; data: Buffer };

function usage(reason: string): CliResult {
  return { ok: false, error: "usage", reason };
}

function crc32(data: Buffer): number {
  let crc = 0xffffffff;
  for (let i = 0; i < data.length; i += 1) {
    crc ^= data[i] ?? 0;
    for (let bit = 0; bit < 8; bit += 1) crc = (crc >>> 1) ^ (0xedb88320 & -(crc & 1));
  }
  return (crc ^ 0xffffffff) >>> 0;
}

function dosTime(date: Date): { time: number; day: number } {
  const time = (date.getHours() << 11) | (date.getMinutes() << 5) | Math.floor(date.getSeconds() / 2);
  const day = ((date.getFullYear() - 1980) << 9) | ((date.getMonth() + 1) << 5) | date.getDate();
  return { time, day };
}

/** Zip file bytes. Names are slash-separated and stored as UTF-8. */
export function zipFiles(files: Entry[]): Buffer {
  const now = dosTime(new Date());
  const locals: Buffer[] = [];
  const centrals: Buffer[] = [];
  let offset = 0;
  for (const file of files) {
    const name = Buffer.from(file.name, "utf8");
    const stored = file.data.length === 0 ? Buffer.alloc(0) : deflateRawSync(file.data);
    const crc = crc32(file.data);
    const local = Buffer.alloc(30);
    local.writeUInt32LE(0x04034b50, 0);
    local.writeUInt16LE(20, 4);
    local.writeUInt16LE(0x0800, 6);
    local.writeUInt16LE(file.data.length === 0 ? 0 : 8, 8);
    local.writeUInt16LE(now.time, 10);
    local.writeUInt16LE(now.day, 12);
    local.writeUInt32LE(crc, 14);
    local.writeUInt32LE(stored.length, 18);
    local.writeUInt32LE(file.data.length, 22);
    local.writeUInt16LE(name.length, 26);
    locals.push(local, name, stored);
    const central = Buffer.alloc(46);
    central.writeUInt32LE(0x02014b50, 0);
    central.writeUInt16LE(20, 4);
    central.writeUInt16LE(20, 6);
    central.writeUInt16LE(0x0800, 8);
    central.writeUInt16LE(file.data.length === 0 ? 0 : 8, 10);
    central.writeUInt16LE(now.time, 12);
    central.writeUInt16LE(now.day, 14);
    central.writeUInt32LE(crc, 16);
    central.writeUInt32LE(stored.length, 20);
    central.writeUInt32LE(file.data.length, 24);
    central.writeUInt16LE(name.length, 28);
    central.writeUInt32LE(offset, 42);
    centrals.push(central, name);
    offset += local.length + name.length + stored.length;
  }
  const central = Buffer.concat(centrals);
  const end = Buffer.alloc(22);
  end.writeUInt32LE(0x06054b50, 0);
  end.writeUInt16LE(files.length, 8);
  end.writeUInt16LE(files.length, 10);
  end.writeUInt32LE(central.length, 12);
  end.writeUInt32LE(offset, 16);
  return Buffer.concat([...locals, central, end]);
}

function findEocd(zip: Buffer): number {
  const start = Math.max(0, zip.length - 66_000);
  for (let i = zip.length - 22; i >= start; i -= 1) {
    if (zip.readUInt32LE(i) === 0x06054b50) return i;
  }
  return -1;
}

/** Entries in a zip. Throws when the archive is not a zip we can read. */
export function unzipFiles(zip: Buffer): Entry[] {
  const eocd = findEocd(zip);
  if (eocd < 0) throw new Error("不是 zip");
  const count = zip.readUInt16LE(eocd + 8);
  let cursor = zip.readUInt32LE(eocd + 16);
  const files: Entry[] = [];
  let total = 0;
  for (let n = 0; n < count; n += 1) {
    if (cursor + 46 > zip.length || zip.readUInt32LE(cursor) !== 0x02014b50) throw new Error("目录损坏");
    const flags = zip.readUInt16LE(cursor + 8);
    const method = zip.readUInt16LE(cursor + 10);
    const crc = zip.readUInt32LE(cursor + 16);
    const compSize = zip.readUInt32LE(cursor + 20);
    const size = zip.readUInt32LE(cursor + 24);
    const nameLen = zip.readUInt16LE(cursor + 28);
    const extraLen = zip.readUInt16LE(cursor + 30);
    const commentLen = zip.readUInt16LE(cursor + 32);
    const localAt = zip.readUInt32LE(cursor + 42);
    const name = zip.subarray(cursor + 46, cursor + 46 + nameLen).toString("utf8").replaceAll("\\", "/");
    cursor += 46 + nameLen + extraLen + commentLen;
    if (!name || name.endsWith("/")) continue;
    if (flags & 1) throw new Error("不读加密的包");
    if (method !== 0 && method !== 8) throw new Error("压缩方式不支持");
    if (size > PACK_LIMIT || compSize > PACK_LIMIT) throw new Error("包太大");
    total += size;
    if (total > PACK_LIMIT) throw new Error("包太大");
    if (localAt + 30 > zip.length || zip.readUInt32LE(localAt) !== 0x04034b50) throw new Error("文件头损坏");
    const localName = zip.readUInt16LE(localAt + 26);
    const localExtra = zip.readUInt16LE(localAt + 28);
    const dataAt = localAt + 30 + localName + localExtra;
    const compressed = zip.subarray(dataAt, dataAt + compSize);
    const data = method === 0 ? Buffer.from(compressed) : inflateRawSync(compressed);
    if (data.length !== size || crc32(data) !== crc) throw new Error(`${name} 校验失败`);
    files.push({ name, data });
  }
  return files;
}

async function filesOf(dir: string): Promise<Entry[]> {
  const out: Entry[] = [];
  let total = 0;
  async function walk(rel: string): Promise<void> {
    const abs = path.join(dir, rel);
    const entries = await readdir(abs, { withFileTypes: true });
    for (const entry of entries.sort((a, b) => a.name.localeCompare(b.name))) {
      if (rel === "" && entry.name === "backup") continue;
      if (entry.isSymbolicLink()) continue;
      const name = rel ? `${rel}/${entry.name}` : entry.name;
      if (entry.isDirectory()) {
        await walk(name);
        continue;
      }
      if (!entry.isFile()) continue;
      const data = await readFile(path.join(dir, name));
      total += data.length;
      if (total > PACK_LIMIT || out.length > 8000) throw new Error("这一局太大，打不成包");
      out.push({ name, data });
    }
  }
  await walk("");
  return out;
}

export async function packGame(dir: string): Promise<{ zip: Buffer; files: number; bytes: number }> {
  const files = await filesOf(dir);
  const bytes = files.reduce((sum, file) => sum + file.data.length, 0);
  return { zip: zipFiles(files), files: files.length, bytes };
}

function rooted(files: Entry[]): Entry[] {
  const names = files.map(file => file.name);
  const top = new Set(names.map(name => name.split("/")[0] ?? ""));
  const only = [...top][0] ?? "";
  if (top.size === 1 && only && names.every(name => name.startsWith(`${only}/`)) && names.includes(`${only}/session.json`)) {
    return files.map(file => ({ name: file.name.slice(only.length + 1), data: file.data }));
  }
  return files;
}

function safeName(name: string): boolean {
  if (!name || name.startsWith("/") || /^[a-z]:/iu.test(name)) return false;
  const parts = name.split("/");
  return parts.every(part => part && part !== "." && part !== "..");
}

/** Put a pack into games/<id> from its session.json. An existing or active game is refused. */
export async function unpackGame(gamesDir: string, zip: Buffer, activeId: string | null): Promise<CliResult> {
  let files: Entry[];
  try {
    files = rooted(unzipFiles(zip));
  } catch (cause) {
    return usage(cause instanceof Error ? cause.message : "包读不了");
  }
  const sessionFile = files.find(file => file.name === "session.json");
  if (!sessionFile) return usage("包里没有 session.json");
  let session: Record<string, unknown>;
  try {
    const parsed = JSON.parse(sessionFile.data.toString("utf8")) as unknown;
    if (typeof parsed !== "object" || parsed === null || Array.isArray(parsed)) return usage("session.json 不是对象");
    session = parsed as Record<string, unknown>;
  } catch {
    return usage("session.json 不是 JSON");
  }
  const id = typeof session.id === "string" ? session.id : "";
  const placed = resolveGameDir(gamesDir, id);
  if (placed.ok !== true) return placed;
  if (activeId && activeId === id) return usage("这一局正在进行，先收尾再上传");
  if (files.some(file => !safeName(file.name))) return usage("包里的路径不能跳出这一局");
  try {
    await readFile(path.join(placed.dir, "session.json"));
    return usage("这一局已经在");
  } catch { /* a new id */ }
  const root = path.resolve(placed.dir);
  await mkdir(root, { recursive: true });
  try {
    for (const file of files) {
      const target = path.resolve(root, file.name);
      if (target !== root && !target.startsWith(root + path.sep)) throw new Error("包里的路径不能跳出这一局");
      await mkdir(path.dirname(target), { recursive: true });
      await writeFile(target, file.data);
    }
  } catch (cause) {
    await rm(root, { recursive: true, force: true });
    return usage(cause instanceof Error ? cause.message : "没有写进这一局");
  }
  return { ok: true, id, title: session.title ?? "", files: files.length };
}
