interface Rule {
  negate: boolean;
  dirOnly: boolean;
  anchored: boolean;
  re: RegExp;
}

function segmentToRegex(seg: string): string {
  let out = "";
  for (let i = 0; i < seg.length; i++) {
    const c = seg[i];
    if (c === "[") {
      let j = i + 1;
      let neg = false;
      if (seg[j] === "!" || seg[j] === "^") {
        neg = true;
        j++;
      }
      let body = "";
      while (j < seg.length && seg[j] !== "]") {
        if (seg[j] === "\\" && j + 1 < seg.length) {
          body += seg[j] + seg[j + 1];
          j += 2;
          continue;
        }
        body += seg[j];
        j++;
      }
      if (j < seg.length) {
        out += "[" + (neg ? "^" : "") + body.replace(/([\]\\])/g, "\\$1") + "]";
        i = j;
      } else {
        out += "\\[";
      }
    } else if (c === "*") {
      out += "[^/]*";
    } else if (c === "?") {
      out += "[^/]";
    } else {
      out += c.replace(/[.+^${}()|\\]/g, "\\$&");
    }
  }
  return out;
}

function globToRegex(pattern: string): RegExp {
  const segs = pattern.split("/");
  let re = "^";
  let needSlash = false;
  for (let s = 0; s < segs.length; s++) {
    const seg = segs[s];
    if (seg === "**") {
      if (s === segs.length - 1) {
        re += needSlash ? "/.*" : ".*";
      } else {
        re += needSlash ? "/(.*/)?" : "(.*/)?";
      }
      needSlash = false;
    } else {
      if (needSlash) re += "/";
      re += segmentToRegex(seg);
      needSlash = true;
    }
  }
  re += "$";
  return new RegExp(re);
}

function compileLine(line: string): Rule | null {
  let p = line;
  if (p.endsWith(" ")) p = p.slice(0, -1);
  if (p === "" || p.startsWith("#")) return null;
  let negate = false;
  if (p.startsWith("!")) {
    negate = true;
    p = p.slice(1);
  }
  let dirOnly = false;
  if (p.endsWith("/")) {
    dirOnly = true;
    p = p.slice(0, -1);
  }
  if (p === "") return null;
  let anchored = false;
  if (p.startsWith("/")) {
    anchored = true;
    p = p.slice(1);
  } else if (p.includes("/")) {
    anchored = true;
  }
  const segRe = globToRegex(p).source.slice(1, -1);
  const full = anchored ? `^${segRe}$` : `(^|/)${segRe}$`;
  return { negate, dirOnly, anchored, re: new RegExp(full) };
}

export class IgnoreMatcher {
  private rules: Rule[] = [];

  add(patterns: string): void {
    for (const line of patterns.split(/\r?\n/)) {
      const rule = compileLine(line);
      if (rule) this.rules.push(rule);
    }
  }

  ignores(path: string, isDir: boolean): boolean {
    let ignored = false;
    for (const rule of this.rules) {
      if (rule.dirOnly && !isDir) continue;
      if (rule.re.test(path)) ignored = !rule.negate;
    }
    return ignored;
  }
}
