import { deepEqual, equal, throws } from "node:assert";
import { test } from "node:test";
import { packTar, parseTar } from "../src/tar.js";
import type { ParsedEntry } from "../src/tar.js";

function roundTrip(entries: { path: string; data: Buffer; mtimeSec?: number }[]): ParsedEntry[] {
  const tar = packTar(entries);
  const out: ParsedEntry[] = [];
  parseTar(tar, (e) => out.push(e));
  return out;
}

test("ustar round trip single file", () => {
  const out = roundTrip([{ path: "a/b.txt", data: Buffer.from("hello tar"), mtimeSec: 1700000000 }]);
  equal(out.length, 1);
  equal(out[0].path, "a/b.txt");
  equal(out[0].data.toString(), "hello tar");
  equal(out[0].mtimeSec, 1700000000);
  equal(out[0].size, 9);
});

test("multi-block file round trips", () => {
  const data = Buffer.alloc(3000);
  for (let i = 0; i < data.length; i++) data[i] = i & 0xff;
  const out = roundTrip([{ path: "big.bin", data }]);
  equal(out.length, 1);
  deepEqual(Buffer.compare(out[0]!.data, data), 0);
});

test("empty file round trips", () => {
  const out = roundTrip([{ path: "empty.txt", data: Buffer.alloc(0) }]);
  equal(out.length, 1);
  equal(out[0].size, 0);
});

test("many entries stay ordered", () => {
  const entries = [];
  for (let i = 0; i < 50; i++) {
    entries.push({ path: `dir/f${i}.txt`, data: Buffer.from(`content-${i}`) });
  }
  const out = roundTrip(entries);
  equal(out.length, 50);
  for (let i = 0; i < 50; i++) {
    equal(out[i].path, `dir/f${i}.txt`);
    equal(out[i].data.toString(), `content-${i}`);
  }
});

test("long paths split into ustar prefix", () => {
  const dir = "d/".repeat(60);
  const longPath = `${dir}${"a".repeat(89)}.txt`;
  const out = roundTrip([{ path: longPath, data: Buffer.from("x") }]);
  equal(out.length, 1);
  equal(out[0].path, longPath);
});

test("path over ustar limit is rejected", () => {
  const noSlashes = "t".repeat(300);
  throws(() => packTar([{ path: noSlashes, data: Buffer.alloc(0) }]), /too long/);
});

test("corrupted header is rejected", () => {
  const tar = packTar([{ path: "x.txt", data: Buffer.from("abc") }]);
  tar[0] = tar[0]! ^ 0xff;
  throws(() => parseTar(tar, () => {}));
});

test("truncated data is rejected", () => {
  const tar = packTar([{ path: "x.txt", data: Buffer.alloc(700, 7) }]);
  throws(() => parseTar(tar.subarray(0, 600), () => {}));
});
