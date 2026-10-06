import { deepEqual, equal } from "node:assert";
import { test } from "node:test";
import { IgnoreMatcher } from "../src/gitignore.js";

function ignores(patterns: string[], path: string, isDir = false): boolean {
  const m = new IgnoreMatcher();
  for (const p of patterns) m.add(p);
  return m.ignores(path, isDir);
}

test("plain filename matches at any depth", () => {
  equal(ignores(["foo"], "foo"), true);
  equal(ignores(["foo"], "a/foo"), true);
  equal(ignores(["foo"], "foo/bar"), false);
  equal(ignores(["foo"], "bar"), false);
});

test("anchored pattern only matches at root", () => {
  equal(ignores(["/build"], "build"), true);
  equal(ignores(["/build"], "a/build"), false);
  equal(ignores(["src/logs"], "src/logs"), true);
  equal(ignores(["src/logs"], "other/src/logs"), false);
});

test("dir-only patterns", () => {
  equal(ignores(["build/"], "build", true), true);
  equal(ignores(["build/"], "build", false), false);
  equal(ignores(["build/"], "a/build", true), true);
});

test("star does not cross slashes", () => {
  equal(ignores(["*.log"], "a/b/x.log"), true);
  equal(ignores(["*.log"], "a/b/x.log.bak"), false);
  equal(ignores(["f?o"], "foo"), true);
  equal(ignores(["f?o"], "fo"), false);
});

test("double star matches across slashes", () => {
  equal(ignores(["**/temp"], "temp"), true);
  equal(ignores(["**/temp"], "a/b/temp"), true);
  equal(ignores(["a/**/b"], "a/b"), true);
  equal(ignores(["a/**/b"], "a/x/y/b"), true);
  equal(ignores(["a/**/b"], "a/x/y/c"), false);
  equal(ignores(["docs/**"], "docs/a/b.md"), true);
  equal(ignores(["docs/**"], "docs"), false);
});

test("negation wins when last", () => {
  equal(ignores(["*.log", "!keep.log"], "keep.log"), false);
  equal(ignores(["*.log", "!keep.log"], "drop.log"), true);
  equal(ignores(["!keep.log", "*.log"], "keep.log"), true);
});

test("character classes", () => {
  equal(ignores(["[abc].txt"], "a.txt"), true);
  equal(ignores(["[abc].txt"], "d.txt"), false);
  equal(ignores(["[!abc].txt"], "d.txt"), true);
});

test("comments and blank lines are skipped", () => {
  equal(ignores(["# comment", "", "   "], "comment"), false);
});
