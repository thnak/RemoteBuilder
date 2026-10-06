import { createHash } from "node:crypto";
import { mkdir, readFile, writeFile } from "node:fs/promises";
import os from "node:os";
import path, { dirname } from "node:path";

import { packTar, parseTar, type PackEntry, type ParsedEntry } from "./tar.js";
import {
  buildManifest,
  diffManifest,
  serializeManifest,
  type FileEntry,
} from "./sync.js";

export interface Peer {
  name: string;
  host: string;
  port: number;
  token: string;
}

export interface WorkspaceInfo {
  ws: string;
  countFiles: number;
  bytes: number;
}

export interface Inventory {
  name: string;
  agentVer: string;
  os: string;
  arch: string;
  cores: number;
  cpuLoadPct: number;
  memTotalMB: number;
  memFreeMB: number;
  runningJobs: number;
  queuedJobs: number;
  workspaces: WorkspaceInfo[];
}

export interface SyncResult {
  ok: boolean;
  applied: number;
  bytes: number;
  manifestFiles: number;
  pruned: number;
}

export type JobState =
  | "queued"
  | "running"
  | "done"
  | "killed"
  | "failed";

export interface JobStatus {
  jobId: string;
  ws: string;
  cmd: string;
  status: JobState;
  exitCode: number;
  timeoutSec: number;
  startedAt: string;
  endedAt: string;
  logBytes: number;
}

export interface ExecRequest {
  cmd: string;
  cwd?: string;
  env?: Record<string, string>;
  timeoutSec?: number;
}

export class PeerError extends Error {
  constructor(
    message: string,
    readonly peer: string,
    readonly status?: number,
  ) {
    super(message);
  }
}

export function defaultPeersFile(): string {
  const envFile = process.env.RB_PEERS_FILE;
  if (envFile) return envFile;
  return path.join(os.homedir(), ".remotebuilder", "peers.json");
}

export async function loadPeers(file = defaultPeersFile()): Promise<Peer[]> {
  try {
    const text = await readFile(file, "utf8");
    const parsed: unknown = JSON.parse(text);
    if (!parsed || typeof parsed !== "object") return [];
    const peers = (parsed as { peers?: unknown[] }).peers ?? [];
    return peers.filter(
      (p): p is Peer =>
        !!p &&
        typeof (p as Peer).name === "string" &&
        typeof (p as Peer).host === "string" &&
        typeof (p as Peer).port === "number" &&
        typeof (p as Peer).token === "string",
    );
  } catch {
    return [];
  }
}

export async function savePeers(
  peers: Peer[],
  file = defaultPeersFile(),
): Promise<void> {
  await mkdir(dirname(file), { recursive: true });
  await writeFile(file, JSON.stringify({ peers }, null, 2), "utf8");
}

function assertOk(res: Response, peerName: string): void {
  if (!res.ok) {
    throw new PeerError(
      `${res.status} ${res.statusText}`,
      peerName,
      res.status,
    );
  }
}

export class PeerClient {
  readonly base: string;

  constructor(readonly peer: Peer) {
    this.base = `http://${peer.host}:${peer.port}`;
  }

  get name(): string {
    return this.peer.name;
  }

  private async req(
    method: string,
    pathAndQuery: string,
    body?: string | Uint8Array,
  ): Promise<Response> {
    const headers: Record<string, string> = {
      Authorization: `Bearer ${this.peer.token}`,
    };
    if (body !== undefined) {
      headers["Content-Type"] =
        typeof body === "string"
          ? "application/json"
          : "application/octet-stream";
    }
    const init: RequestInit = { method, headers };
    if (body !== undefined) {
      init.body = typeof body === "string" ? body : new Uint8Array(body);
    }
    return fetch(this.base + pathAndQuery, init);
  }

  async inventory(): Promise<Inventory> {
    const res = await this.req("GET", "/inventory");
    assertOk(res, this.name);
    const j = (await res.json()) as Inventory & {
      jobs?: { running?: number; queued?: number };
    };
    return {
      name: j.name,
      agentVer: j.agentVer,
      os: j.os,
      arch: j.arch,
      cores: j.cores,
      cpuLoadPct: j.cpuLoadPct,
      memTotalMB: j.memTotalMB,
      memFreeMB: j.memFreeMB,
      runningJobs: j.jobs?.running ?? 0,
      queuedJobs: j.jobs?.queued ?? 0,
      workspaces: j.workspaces ?? [],
    };
  }

  async getManifest(ws: string): Promise<FileEntry[] | null> {
    const res = await this.req("GET", `/manifest?ws=${encodeURIComponent(ws)}`);
    if (res.status === 404) return null;
    assertOk(res, this.name);
    const text = await res.text();
    const parsed = JSON.parse(text) as { files?: FileEntry[] };
    return parsed.files ?? null;
  }

  async sync(ws: string, tar: Buffer): Promise<SyncResult> {
    const res = await this.req(
      "POST",
      `/sync?ws=${encodeURIComponent(ws)}`,
      tar,
    );
    assertOk(res, this.name);
    return (await res.json()) as SyncResult;
  }

  async startJob(ws: string, request: ExecRequest): Promise<string> {
    const res = await this.req(
      "POST",
      `/jobs?ws=${encodeURIComponent(ws)}`,
      JSON.stringify(request),
    );
    assertOk(res, this.name);
    const j = (await res.json()) as { jobId: string };
    return j.jobId;
  }

  async getJob(jobId: string): Promise<JobStatus> {
    const res = await this.req("GET", `/jobs/${encodeURIComponent(jobId)}`);
    assertOk(res, this.name);
    return (await res.json()) as JobStatus;
  }

  async killJob(jobId: string): Promise<void> {
    const res = await this.req("DELETE", `/jobs/${encodeURIComponent(jobId)}`);
    assertOk(res, this.name);
  }

  async jobLog(
    jobId: string,
    offset: number,
  ): Promise<{ data: Buffer; total: number }> {
    const res = await this.req(
      "GET",
      `/jobs/${encodeURIComponent(jobId)}/log?offset=${offset}`,
    );
    assertOk(res, this.name);
    const total = Number(res.headers.get("X-RB-Log-Bytes") ?? "0");
    const buf = Buffer.from(await res.arrayBuffer());
    return { data: buf, total };
  }

  async fetchTar(ws: string, paths: string[]): Promise<Buffer> {
    const res = await this.req(
      "POST",
      `/fetch-tar?ws=${encodeURIComponent(ws)}`,
      JSON.stringify({ paths }),
    );
    assertOk(res, this.name);
    return Buffer.from(await res.arrayBuffer());
  }

  async deleteWs(ws: string): Promise<void> {
    const res = await this.req(
      "DELETE",
      `/ws/${encodeURIComponent(ws)}`,
    );
    assertOk(res, this.name);
  }
}

export interface SyncWorkspaceOptions {
  dir: string;
  ws: string;
  extraIgnore?: string[];
}

export async function syncWorkspace(
  client: PeerClient,
  options: SyncWorkspaceOptions,
): Promise<SyncResult & { changed: number }> {
  const manifest = await buildManifest(options.dir, options.extraIgnore ?? []);
  const remote = await client.getManifest(options.ws);
  const changed = diffManifest(manifest.files, remote);
  const entries: PackEntry[] = [];
  for (const f of changed) {
    entries.push({
      path: f.p,
      data: await readFile(path.join(options.dir, f.p)),
    });
  }
  entries.push({
    path: "manifest.json",
    data: Buffer.from(serializeManifest(manifest), "utf8"),
  });
  const tar = packTar(entries);
  const result = await client.sync(options.ws, tar);
  return { ...result, changed: changed.length };
}

export function hashString(data: string): string {
  return createHash("sha1").update(data).digest("hex");
}

export type { ParsedEntry };
