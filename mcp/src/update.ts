import { spawn } from "node:child_process";
import { readFile } from "node:fs/promises";
import { createRequire } from "node:module";
import os from "node:os";
import path from "node:path";

export const NPM_PACKAGE = "@thnak/remotebuilder";
const REGISTRY_URL =
  "https://registry.npmjs.org/@thnak%2Fremotebuilder";
const RELEASES_API =
  "https://api.github.com/repos/thnak/RemoteBuilder/releases/latest";
const RELEASES_PAGE =
  "https://github.com/thnak/RemoteBuilder/releases";

export interface UpdateCheck {
  local: string;
  onNpm: boolean;
  latest: string;
  beta: string;
  agentVer: string | null;
  agentLatest: string | null;
  agentExeUrl: string | null;
  releasesUrl: string;
}

export function cmpVer(a: string, b: string): number {
  const pa = a
    .replace(/^v/, "")
    .split(".")
    .map((n) => parseInt(n, 10) || 0);
  const pb = b
    .replace(/^v/, "")
    .split(".")
    .map((n) => parseInt(n, 10) || 0);
  for (let i = 0; i < 3; i++) {
    const x = pa[i] ?? 0;
    const y = pb[i] ?? 0;
    if (x !== y) return x > y ? 1 : -1;
  }
  return 0;
}

export function localVersion(): string {
  const require = createRequire(import.meta.url);
  const pkg = require("../../package.json") as { version: string };
  return pkg.version;
}

async function localAgentBase(): Promise<{
  host: string;
  port: number;
  token: string;
} | null> {
  if (process.platform !== "win32") return null;
  const host = process.env.RB_AGENT_HOST ?? "127.0.0.1";
  const port = Number(process.env.RB_AGENT_PORT ?? "7333");
  let token = process.env.RB_AGENT_TOKEN ?? "";
  if (!token) {
    const dir = process.env.LOCALAPPDATA;
    if (!dir) return null;
    try {
      token = (
        await readFile(path.join(dir, "rb-agent", "token.txt"), "utf8")
      ).trim();
    } catch {
      return null;
    }
  }
  return { host, port, token };
}

export async function checkUpdates(): Promise<UpdateCheck> {
  const local = localVersion();

  let onNpm = true;
  let latest = "";
  let beta = "";
  try {
    const res = await fetch(REGISTRY_URL, {
      headers: { "User-Agent": NPM_PACKAGE },
    });
    if (!res.ok) {
      onNpm = false;
    } else {
      const j = (await res.json()) as {
        "dist-tags"?: { latest?: string; beta?: string };
      };
      latest = j["dist-tags"]?.latest ?? "";
      beta = j["dist-tags"]?.beta ?? "";
    }
  } catch {
    onNpm = false;
  }

  let agentLatest: string | null = null;
  let agentExeUrl: string | null = null;
  try {
    const res = await fetch(RELEASES_API, {
      headers: { "User-Agent": NPM_PACKAGE },
    });
    if (res.ok) {
      const j = (await res.json()) as {
        tag_name?: string;
        assets?: { name: string; browser_download_url: string }[];
      };
      agentLatest = j.tag_name ?? null;
      agentExeUrl =
        j.assets?.find((a) => a.name === "rbagent.exe")
          ?.browser_download_url ?? null;
    }
  } catch {
    agentLatest = null;
  }

  let agentVer: string | null = null;
  const base = await localAgentBase();
  if (base) {
    try {
      const res = await fetch(
        `http://${base.host}:${base.port}/inventory`,
        { headers: { Authorization: `Bearer ${base.token}` } },
      );
      if (res.ok) {
        agentVer =
          ((await res.json()) as { agentVer?: string }).agentVer ?? null;
      }
    } catch {
      agentVer = null;
    }
  }

  return {
    local,
    onNpm,
    latest,
    beta,
    agentVer,
    agentLatest,
    agentExeUrl,
    releasesUrl: RELEASES_PAGE,
  };
}

export function installUpdate(
  channel: "latest" | "beta",
): Promise<void> {
  return new Promise((resolve, reject) => {
    const child = spawn(
      "npm",
      ["install", "-g", `${NPM_PACKAGE}@${channel}`],
      { stdio: "inherit" },
    );
    child.on("error", reject);
    child.on("exit", (code) =>
      code === 0 ? resolve() : reject(new Error(`npm exited with ${code}`)),
    );
  });
}
