import { createHash } from "node:crypto";
import { createReadStream, promises as fsp } from "node:fs";
import path from "node:path";

import { IgnoreMatcher } from "./gitignore.js";

export interface FileEntry {
  p: string;
  s: number;
  h: string;
}

export interface Manifest {
  v: 1;
  files: FileEntry[];
}

export interface TrackedFile {
  rel: string;
  abs: string;
}

type Ign = IgnoreMatcher;

interface Frame {
  prefix: string;
  ig: Ign;
}

function pathIsInside(rel: string): boolean {
  if (rel.includes("\\") || rel.includes("..") || rel.startsWith("/")) return false;
  const parts = rel.split("/");
  for (const part of parts) {
    if (part === "" || part === "." || part === "..") return false;
  }
  return rel.length > 0;
}

function framesMatch(stack: Frame[], rel: string, isDir: boolean): boolean {
  for (let i = stack.length - 1; i >= 0; i--) {
    const frame = stack[i];
    const sub = frame.prefix === "" ? rel : rel.slice(frame.prefix.length + 1);
    if (sub.length === 0) continue;
    if (frame.ig.ignores(sub, isDir)) return true;
  }
  return false;
}

function matcherFrom(patterns: string[]): Ign {
  const m = new IgnoreMatcher();
  for (const p of patterns) m.add(p);
  return m;
}

export async function walkTree(root: string, extraIgnore: string[] = []): Promise<TrackedFile[]> {
  const output: TrackedFile[] = [];
  const stack: Frame[] = [];
  if (extraIgnore.length > 0) {
    stack.push({ prefix: "", ig: matcherFrom(extraIgnore) });
  }
  await visit(fsp, root, "");
  return output;

  async function visit(fs: typeof fsp, dirAbs: string, relDir: string): Promise<void> {
    let pushed = false;
    try {
      let text: string | null = null;
      try {
        text = await fs.readFile(path.join(dirAbs, ".gitignore"), "utf8");
      } catch {
        text = null;
      }
      if (text !== null) {
        stack.push({ prefix: relDir, ig: matcherFrom([text]) });
        pushed = true;
      }
      const items = await fs.readdir(dirAbs, { withFileTypes: true });
      for (const item of items) {
        const rel = relDir === "" ? item.name : `${relDir}/${item.name}`;
        if (item.isDirectory()) {
          if (item.name === ".git") continue;
          if (framesMatch(stack, rel, true)) continue;
          await visit(fs, path.join(dirAbs, item.name), rel);
        } else if (item.isFile()) {
          if (framesMatch(stack, rel, false)) continue;
          output.push({ rel, abs: path.join(dirAbs, item.name) });
        }
      }
    } finally {
      if (pushed) stack.pop();
    }
  }
}

async function statAndHash(abs: string): Promise<{ s: number; h: string }> {
  const st = await fsp.stat(abs);
  const hash = createHash("sha1");
  const rs = createReadStream(abs);
  for await (const chunk of rs) {
    hash.update(chunk as Buffer);
  }
  return { s: st.size, h: hash.digest("hex") };
}

export async function buildManifest(root: string, extraIgnore: string[] = []): Promise<Manifest> {
  const files = await walkTree(root, extraIgnore);
  files.sort((a, b) => (a.rel < b.rel ? -1 : a.rel > b.rel ? 1 : 0));
  const out: FileEntry[] = [];
  for (const f of files) {
    const { s, h } = await statAndHash(f.abs);
    out.push({ p: f.rel, s, h });
  }
  return { v: 1, files: out };
}

export function diffManifest(local: FileEntry[], remote: FileEntry[] | null): FileEntry[] {
  const rem = new Map<string, FileEntry>();
  if (remote) for (const f of remote) rem.set(f.p, f);
  return local.filter((f) => {
    const r = rem.get(f.p);
    return !r || r.h !== f.h || r.s !== f.s;
  });
}

export function parseManifest(text: string): Manifest {
  const json: unknown = JSON.parse(text);
  const m = json as Partial<Manifest>;
  if (!m || typeof m !== "object" || m.v !== 1 || !Array.isArray(m.files)) {
    throw new Error("bad manifest: expected {v:1, files:[...]}");
  }
  const files: FileEntry[] = m.files.map((f) => {
    if (typeof f?.p !== "string" || typeof f?.s !== "number" || typeof f?.h !== "string") {
      throw new Error("bad manifest entry");
    }
    if (!/^[0-9a-f]{40}$/.test(f.h)) throw new Error(`bad manifest hash for ${f.p}`);
    if (!pathIsInside(f.p)) throw new Error(`unsafe manifest path: ${f.p}`);
    return { p: f.p, s: f.s, h: f.h };
  });
  return { v: 1, files };
}

export function serializeManifest(m: Manifest): string {
  return JSON.stringify({ v: 1, files: m.files });
}
