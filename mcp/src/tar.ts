import { Buffer } from "node:buffer";

const BLOCK = 512;
const ZERO_BLOCK = Buffer.alloc(BLOCK);

export interface PackEntry {
  path: string;
  data: Buffer;
  mtimeSec?: number;
  mode?: number;
}

export interface ParsedEntry {
  path: string;
  data: Buffer;
  size: number;
  mtimeSec: number;
}

function writeOctal(field: Buffer, value: number, digits: number): void {
  const s = value.toString(8);
  if (s.length > digits) throw new Error(`octal overflow: ${value} > ${digits} digits`);
  field.write(s.padStart(digits, "0"), 0, digits, "ascii");
  field[digits] = 0;
}

function writeStr(field: Buffer, value: string): void {
  const bytes = Buffer.byteLength(value, "utf8");
  if (bytes > field.length) throw new Error(`field overflow: ${value}`);
  field.write(value, 0, bytes, "utf8");
}

function readStr(field: Buffer): string {
  let end = 0;
  while (end < field.length && field[end] !== 0) end++;
  return field.subarray(0, end).toString("utf8");
}

function readOctal(field: Buffer): number {
  let i = 0;
  while (i < field.length && field[i] === 0x20) i++;
  let value = 0;
  while (i < field.length && field[i] >= 0x30 && field[i] <= 0x37) {
    value = value * 8 + (field[i] - 0x30);
    i++;
  }
  return value;
}

function splitName(pathIn: string): { name: string; prefix: string } {
  const p = pathIn.replace(/\\/g, "/").replace(/\/+$/, "");
  if (Buffer.byteLength(p, "utf8") <= 100) return { name: p, prefix: "" };
  let idx = p.lastIndexOf("/");
  while (idx !== -1) {
    const prefix = p.slice(0, idx);
    const name = p.slice(idx + 1);
    if (
      name.length > 0 &&
      Buffer.byteLength(name, "utf8") <= 100 &&
      Buffer.byteLength(prefix, "utf8") <= 155
    ) {
      return { name, prefix };
    }
    idx = prefix.lastIndexOf("/");
  }
  throw new Error(`path too long for ustar: ${pathIn}`);
}

function headerFor(e: PackEntry): Buffer {
  const h = Buffer.alloc(BLOCK);
  const { name, prefix } = splitName(e.path);
  writeStr(h.subarray(0, 100), name);
  writeOctal(h.subarray(100), e.mode ?? 0o644, 7);
  writeOctal(h.subarray(108), 0, 7);
  writeOctal(h.subarray(116), 0, 7);
  writeOctal(h.subarray(124), e.data.length, 11);
  writeOctal(h.subarray(136), e.mtimeSec ?? Math.floor(Date.now() / 1000), 11);
  h.fill(0x20, 148, 156);
  h[156] = 0x30;
  h.write("ustar", 257, "ascii");
  h.write("00", 263, "ascii");
  writeStr(h.subarray(265, 297), "remotebuilder");
  writeStr(h.subarray(297, 329), "remotebuilder");
  writeStr(h.subarray(345, 500), prefix);
  let sum = 0;
  for (const b of h) sum += b;
  writeOctal(h.subarray(148), sum, 6);
  h[155] = 0x20;
  return h;
}

function padLen(size: number): number {
  return (BLOCK - (size % BLOCK)) % BLOCK;
}

export function packTar(entries: PackEntry[]): Buffer {
  const parts: Buffer[] = [];
  for (const e of entries) {
    if (e.data.length === 0 && e.path === "") throw new Error("empty entry");
    parts.push(headerFor(e), e.data);
    const pad = padLen(e.data.length);
    if (pad !== 0) parts.push(ZERO_BLOCK.subarray(0, pad));
  }
  parts.push(Buffer.alloc(BLOCK * 2));
  return Buffer.concat(parts);
}

function isZeroBlock(buf: Buffer, off: number): boolean {
  for (let i = 0; i < BLOCK; i++) {
    if (buf[off + i] !== 0) return false;
  }
  return true;
}

export function parseTar(buf: Buffer, onEntry: (e: ParsedEntry) => void): void {
  let off = 0;
  while (off + BLOCK <= buf.length) {
    if (isZeroBlock(buf, off)) break;
    const storedSum = readOctal(buf.subarray(off + 148, off + 156));
    let sum = 0;
    for (let i = 0; i < BLOCK; i++) {
      const isChksumField = i >= 148 && i < 156;
      sum += isChksumField ? 0x20 : buf[off + i];
    }
    if (storedSum === 0 || sum !== storedSum) {
      throw new Error(`tar checksum mismatch at offset ${off}`);
    }
    const typeflag = buf[off + 156];
    const size = readOctal(buf.subarray(off + 124, off + 136));
    const mtime = readOctal(buf.subarray(off + 136, off + 148));
    const name = readStr(buf.subarray(off, off + 100));
    const prefix = readStr(buf.subarray(off + 345, off + 500));
    const p = prefix ? `${prefix}/${name}` : name;
    const dataStart = off + BLOCK;
    const dataEnd = dataStart + size;
    const isRegular = typeflag === 0x30 || typeflag === 0;
    if (!isRegular) {
      if (typeflag === 0x35 || typeflag === 0x4c || typeflag === 0x4b || typeflag === 0x78 || typeflag === 0x67) {
        off = dataEnd + padLen(size);
        continue;
      }
      throw new Error(`unsupported tar entry type 0x${typeflag?.toString(16)} at offset ${off}`);
    }
    if (dataEnd > buf.length) throw new Error(`truncated tar at offset ${off} (${p})`);
    onEntry({ path: p, data: buf.subarray(dataStart, dataEnd), size, mtimeSec: mtime });
    off = dataEnd + padLen(size);
  }
}
