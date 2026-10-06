import { deepEqual, equal, throws } from "node:assert";
import { mkdir, mkdtemp, rm, writeFile } from "node:fs/promises";
import { tmpdir } from "node:os";
import path from "node:path";
import { test } from "node:test";
import { buildManifest, diffManifest, parseManifest, serializeManifest } from "../src/sync.js";
import type { FileEntry, Manifest } from "../src/sync.js";

async function makeTree(): Promise<string> {
  const root = await mkdtemp(path.join(tmpdir(), "rb-sync-"));
  await mkdir(path.join(root, "node_modules"), { recursive: true });
  await mkdir(path.join(root, "src"), { recursive: true });
  await mkdir(path.join(root, ".git"), { recursive: true });
  await writeFile(path.join(root, ".gitignore"), "*.log\nnode_modules/\n");
  await writeFile(path.join(root, "a.txt"), "a");
  await writeFile(path.join(root, "b.log"), "b");
  await writeFile(path.join(root, "node_modules", "x.js"), "x");
  await writeFile(path.join(root, ".git", "config"), "cfg");
  await writeFile(path.join(root, "src", ".gitignore"), "secret.txt\n");
  await writeFile(path.join(root, "src", "secret.txt"), "s");
  await writeFile(path.join(root, "src", "keep.js"), "k");
  return root;
}

test("walk respects nested gitignore and skips .git", async () => {
  const root = await makeTree();
  try {
    const m = await buildManifest(root);
    const paths = m.files.map((f) => f.p).sort();
    deepEqual(paths, [".gitignore", "a.txt", "src/.gitignore", "src/keep.js"]);
    for (const f of m.files) {
      equal(f.h.length, 40);
      equal(typeof f.s, "number");
    }
  } finally {
    await rm(root, { recursive: true, force: true });
  }
});

test("manifest is stable across calls", async () => {
  const root = await makeTree();
  try {
    const a = await buildManifest(root);
    const b = await buildManifest(root);
    deepEqual(a, b);
  } finally {
    await rm(root, { recursive: true, force: true });
  }
});

test("extra excludes prune files", async () => {
  const root = await makeTree();
  try {
    const m = await buildManifest(root, ["a.txt"]);
    const paths = m.files.map((f) => f.p);
    equal(paths.includes("a.txt"), false);
    equal(paths.includes("src/keep.js"), true);
  } finally {
    await rm(root, { recursive: true, force: true });
  }
});

test("diff detects changed and stale entries, ignores remote-only extras", () => {
  const local: FileEntry[] = [
    { p: "a", s: 1, h: "1" },
    { p: "b", s: 2, h: "2" },
  ];
  const remote: FileEntry[] = [
    { p: "a", s: 1, h: "1" },
    { p: "b", s: 9, h: "2" },
    { p: "c", s: 9, h: "9" },
  ];
  deepEqual(diffManifest(local, remote), [{ p: "b", s: 2, h: "2" }]);
});

test("diff with no remote uploads everything", () => {
  const local: FileEntry[] = [{ p: "a", s: 1, h: "1" }];
  deepEqual(diffManifest(local, null), local);
});

test("manifest serialization round trips and rejects unsafe paths", () => {
  const m: Manifest = { v: 1, files: [{ p: "a/b.txt", s: 1, h: "a".repeat(40) }] };
  deepEqual(parseManifest(serializeManifest(m)), m);
  throws(() =>
    parseManifest(JSON.stringify({ v: 1, files: [{ p: "../x", s: 1, h: "a".repeat(40) }] })),
  );
  throws(() => parseManifest(JSON.stringify({ v: 2, files: [] })));
});
