import { mkdir, readFile, writeFile } from "node:fs/promises";
import path from "node:path";

import { z } from "zod";
import type { McpServer } from "@modelcontextprotocol/sdk/server/mcp.js";

import { parseTar } from "./tar.js";
import {
  PeerClient,
  syncWorkspace,
  type ExecRequest,
  type JobStatus,
} from "./peer.js";
import {
  fanOut,
  isPeerSelector,
  resolvePeers,
  type PeerSelector,
} from "./fleet.js";

const peerSelectorSchema = z.union([
  z.literal("auto"),
  z.string(),
  z.array(z.string().min(1)),
]);

const jobIdSchema = z.object({
  peer: z.string().min(1),
  jobId: z.string().min(1),
});

function text(value: unknown): { content: { type: "text"; text: string }[] } {
  return {
    content: [
      { type: "text", text: JSON.stringify(value, null, 2) },
    ],
  };
}

function safeJoin(base: string, rel: string): string {
  if (
    rel.includes("\\") ||
    rel.includes("..") ||
    path.isAbsolute(rel)
  ) {
    throw new Error(`unsafe path: ${rel}`);
  }
  const dest = path.join(base, rel);
  if (!dest.startsWith(base)) {
    throw new Error(`unsafe path: ${rel}`);
  }
  return dest;
}

function isTerminal(status: string): boolean {
  return status === "done" || status === "killed" || status === "failed";
}

const sleep = (ms: number) =>
  new Promise<void>((resolve) => setTimeout(resolve, ms));

export function registerTools(
  server: McpServer,
  clients: PeerClient[],
): void {
  server.registerTool(
    "list_peers",
    {
      description:
        "List configured RemoteBuilder build peers with live inventory (cores, load, memory, active jobs).",
      inputSchema: {},
    },
    async () => {
      const peers = await Promise.all(
        clients.map(async (client) => {
          let inventory;
          try {
            inventory = await client.inventory();
          } catch (e) {
            inventory = {
              error: e instanceof Error ? e.message : String(e),
            };
          }
          return {
            name: client.name,
            host: client.peer.host,
            port: client.peer.port,
            inventory,
          };
        }),
      );
      return text({ peers });
    },
  );

  server.registerTool(
    "sync",
    {
      description:
        "Sync a local directory to a workspace on one or more build peers. " +
        "Gitignore-aware: only changed files are uploaded (sha1 diff), and files " +
        "deleted locally are pruned on the peer. The peer can be a peer name, " +
        "a list of peer names, or \"auto\" for the least-loaded reachable peer.",
      inputSchema: {
        peer: peerSelectorSchema,
        dir: z.string().min(1),
        ws: z.string().min(1).max(64),
        extraIgnore: z.array(z.string()).optional(),
      },
    },
    async (args) => {
      if (!isPeerSelector(args.peer)) {
        throw new Error("peer must be a name, list of names, or \"auto\"");
      }
      const selector = args.peer as PeerSelector;
      const resolved = await resolvePeers(selector, clients);
      const results = await fanOut(resolved.map((r) => r.client), async (
        client,
      ) => {
        const result = await syncWorkspace(client, {
          dir: args.dir,
          ws: args.ws,
          extraIgnore: args.extraIgnore ?? [],
        });
        return {
          applied: result.applied,
          bytes: result.bytes,
          manifestFiles: result.manifestFiles,
          pruned: result.pruned,
          changed: result.changed,
        };
      });
      return text({
        ws: args.ws,
        dir: args.dir,
        results,
      });
    },
  );

  server.registerTool(
    "exec",
    {
      description:
        "Start a command (build, test, anything) on a workspace on one or more " +
        "build peers. Returns job ids; poll with get_result. The command runs " +
        "via cmd.exe with cwd inside the workspace; env vars are merged; " +
        "timeoutSec kills the whole process tree when exceeded.",
      inputSchema: {
        peer: peerSelectorSchema,
        ws: z.string().min(1).max(64),
        cmd: z.string().min(1),
        cwd: z.string().optional(),
        env: z.record(z.string()).optional(),
        timeoutSec: z.number().int().min(0).optional(),
      },
    },
    async (args) => {
      if (!isPeerSelector(args.peer)) {
        throw new Error("peer must be a name, list of names, or \"auto\"");
      }
      const request: ExecRequest = {
        cmd: args.cmd,
        cwd: args.cwd,
        env: args.env,
        timeoutSec: args.timeoutSec,
      };
      const resolved = await resolvePeers(args.peer as PeerSelector, clients);
      const results = await fanOut(resolved.map((r) => r.client), async (
        client,
      ) => ({ jobId: await client.startJob(args.ws, request) }));
      return text({ ws: args.ws, cmd: args.cmd, results });
    },
  );

  server.registerTool(
    "get_result",
    {
      description:
        "Poll jobs started with exec until they finish (or waitMs elapses) and " +
        "return status, exit code, and the log tail. Pass jobs as an array of " +
        "{peer, jobId}.",
      inputSchema: {
        jobs: z.array(jobIdSchema).min(1),
        waitMs: z.number().int().min(0).default(0),
        pollMs: z.number().int().min(50).default(500),
        tailBytes: z.number().int().min(0).default(8192),
      },
    },
    async (args) => {
      const byName = new Map(clients.map((c) => [c.name, c]));
      const results = await Promise.all(
        args.jobs.map(async (job) => {
          const client = byName.get(job.peer);
          if (!client) {
            return {
              peer: job.peer,
              jobId: job.jobId,
              error: `unknown peer: ${job.peer}`,
            };
          }
          const deadline = Date.now() + args.waitMs;
          for (;;) {
            const status: JobStatus = await client.getJob(job.jobId);
            if (isTerminal(status.status)) {
              const { data, total } = await client.jobLog(
                job.jobId,
                0,
              );
              const tailStart = Math.max(
                0,
                data.length - args.tailBytes,
              );
              return {
                peer: job.peer,
                jobId: job.jobId,
                status: status.status,
                exitCode: status.exitCode,
                logBytes: total,
                log: data.subarray(tailStart).toString("utf8"),
              };
            }
            if (Date.now() >= deadline) {
              return {
                peer: job.peer,
                jobId: job.jobId,
                status: status.status,
                logBytes: status.logBytes,
                log: null,
              };
            }
            await sleep(args.pollMs);
          }
        }),
      );
      return text({ jobs: results });
    },
  );

  server.registerTool(
    "kill",
    {
      description:
        "Kill running jobs on build peers (whole process tree via Windows Job " +
        "Objects). Pass jobs as an array of {peer, jobId}.",
      inputSchema: {
        jobs: z.array(jobIdSchema).min(1),
      },
    },
    async (args) => {
      const byName = new Map(clients.map((c) => [c.name, c]));
      const results = await Promise.all(
        args.jobs.map(async (job) => {
          const client = byName.get(job.peer);
          if (!client) {
            return {
              peer: job.peer,
              jobId: job.jobId,
              error: `unknown peer: ${job.peer}`,
            };
          }
          try {
            await client.killJob(job.jobId);
            return { peer: job.peer, jobId: job.jobId, killed: true };
          } catch (e) {
            return {
              peer: job.peer,
              jobId: job.jobId,
              killed: false,
              error: e instanceof Error ? e.message : String(e),
            };
          }
        }),
      );
      return text({ results });
    },
  );

  server.registerTool(
    "fetch_artifacts",
    {
      description:
        "Fetch files or whole directories from a peer workspace to the local " +
        "machine (default under .remotebuilder-out/<peer>/<ws>). Returns the " +
        "list of fetched files with sizes.",
      inputSchema: {
        peer: z.string().min(1),
        ws: z.string().min(1).max(64),
        paths: z.array(z.string().min(1)).min(1),
        outDir: z.string().optional(),
      },
    },
    async (args) => {
      const client = clients.find((c) => c.name === args.peer);
      if (!client) {
        throw new Error(`unknown peer: ${args.peer}`);
      }
      const tar = await client.fetchTar(args.ws, args.paths);
      const outDir =
        args.outDir ??
        path.join(".remotebuilder-out", client.name, args.ws);
      const entries: { path: string; data: Buffer; size: number }[] =
        [];
      parseTar(tar, (entry) => {
        entries.push({
          path: entry.path,
          data: entry.data,
          size: entry.size,
        });
      });
      const files: { path: string; size: number }[] = [];
      for (const entry of entries) {
        const dest = safeJoin(outDir, entry.path);
        await mkdir(path.dirname(dest), { recursive: true });
        await writeFile(dest, entry.data);
        files.push({ path: entry.path, size: entry.size });
      }
      return text({ peer: args.peer, outDir, files });
    },
  );
}
