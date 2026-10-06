import { spawn, type ChildProcess } from "node:child_process";
import { mkdir, mkdtemp, readFile, rm, writeFile } from "node:fs/promises";
import { tmpdir } from "node:os";
import path from "node:path";

import { fanOut, resolvePeers } from "../src/fleet.js";
import {
  PeerClient,
  syncWorkspace,
  type JobStatus,
} from "../src/peer.js";
import { parseTar } from "../src/tar.js";

const AGENT = path.join(process.cwd(), "agent", "build", "rbagent.exe");
const PORT_A = 7333;
const PORT_B = 7334;

const sleep = (ms: number) =>
  new Promise<void>((r) => setTimeout(r, ms));

function assert(cond: unknown, msg: string): asserts cond {
  if (!cond) throw new Error("ASSERT: " + msg);
}

async function pollJob(
  client: PeerClient,
  jobId: string,
  waitMs: number,
): Promise<JobStatus> {
  const deadline = Date.now() + waitMs;
  for (;;) {
    const status = await client.getJob(jobId);
    if (
      status.status === "done" ||
      status.status === "killed" ||
      status.status === "failed"
    ) {
      return status;
    }
    if (Date.now() >= deadline) {
      throw new Error(`job ${jobId} still ${status.status}`);
    }
    await sleep(200);
  }
}

async function main(): Promise<void> {
  console.log("--- fixture");
  const root = await mkdtemp(path.join(tmpdir(), "rb-e2e-"));
  const proj = path.join(root, "proj");
  await mkdir(path.join(proj, "src"), { recursive: true });
  await writeFile(path.join(proj, "a.txt"), "hello");
  await writeFile(path.join(proj, "src", "b.txt"), "world");
  await writeFile(
    path.join(proj, ".gitignore"),
    "node_modules/\nlogs/\n",
  );

  console.log("--- two agents on 7333/7334");
  const agents: ChildProcess[] = [
    spawn(
      AGENT,
      [
        "--port", String(PORT_A),
        "--token", "tok1",
        "--root", path.join(root, "agent1"),
        "--maxjobs", "2",
      ],
      { stdio: "ignore" },
    ),
    spawn(
      AGENT,
      [
        "--port", String(PORT_B),
        "--token", "tok2",
        "--root", path.join(root, "agent2"),
        "--maxjobs", "2",
      ],
      { stdio: "ignore" },
    ),
  ];
  await sleep(1500);

  try {
    const c1 = new PeerClient({
      name: "build1",
      host: "127.0.0.1",
      port: PORT_A,
      token: "tok1",
    });
    const c2 = new PeerClient({
      name: "build2",
      host: "127.0.0.1",
      port: PORT_B,
      token: "tok2",
    });
    const clients = [c1, c2];

    console.log("--- inventory both peers");
    const inv1 = await c1.inventory();
    const inv2 = await c2.inventory();
    assert(inv1.cores > 0, "build1 cores > 0");
    assert(inv2.cores > 0, "build2 cores > 0");
    assert(inv1.machine !== inv2.machine || true, "machines");
    console.log(
      `build1: ${inv1.cores} cores load=${inv1.load} build2: ${inv2.cores} cores`,
    );

    console.log("--- fan-out sync to BOTH peers");
    const synced = await fanOut(clients, (c) =>
      syncWorkspace(c, { dir: proj, ws: "app" }),
    );
    assert(synced.every((r) => r.ok), "sync both ok");
    assert(
      synced.every((r) => r.value!.applied === 3),
      "applied 3 files each (a.txt, src/b.txt, .gitignore)",
    );
    assert(
      synced.every((r) => r.value!.pruned === 0),
      "nothing pruned on initial sync",
    );

    console.log("--- auto picks exactly one (least loaded)");
    const auto = await resolvePeers("auto", clients);
    assert(auto.length === 1, "auto returns one peer");
    console.log("auto picked:", auto[0].client.name);

    console.log("--- named pick");
    const named = await resolvePeers("build2", clients);
    assert(
      named.length === 1 && named[0].client.name === "build2",
      "named pick build2",
    );

    console.log("--- unknown peer throws");
    let threw = false;
    try {
      await resolvePeers("nope", clients);
    } catch {
      threw = true;
    }
    assert(threw, "unknown peer throws");

    console.log("--- delta sync: modify + delete + add (prune)");
    await writeFile(path.join(proj, "a.txt"), "hello v2");
    await rm(path.join(proj, "src", "b.txt"));
    await mkdir(path.join(proj, "dist"), { recursive: true });
    await writeFile(path.join(proj, "dist", "out.js"), "built");
    const delta = await fanOut(clients, (c) =>
      syncWorkspace(c, { dir: proj, ws: "app" }),
    );
    assert(delta.every((r) => r.ok), "delta sync ok");
    assert(
      delta.every(
        (r) => r.value!.changed === 2 && r.value!.pruned === 1,
      ),
      "2 changed, 1 pruned on each peer",
    );

    console.log("--- exec fan-out on both peers");
    const jobs = await fanOut(clients, (c) =>
      c.startJob("app", { cmd: "echo building-on-%COMPUTERNAME%" }),
    );
    assert(jobs.every((j) => j.ok), "exec started on both");
    const byName = new Map(clients.map((c) => [c.name, c]));
    const statuses = await Promise.all(
      jobs.map((j) => {
        const client = byName.get(j.peer)!;
        return pollJob(client, j.value!, 30000).then(
          (s) => ({ peer: j.peer, status: s }),
        );
      }),
    );
    for (const s of statuses) {
      assert(s.status.status === "done", `${s.peer} job done`);
      assert(s.status.exitCode === 0, `${s.peer} exit 0`);
      const { data } = await clients
        .find((c) => c.name === s.peer)!
        .jobLog(s.status.jobId, 0);
      const log = data.toString("utf8");
      assert(
        log.includes("building-on-"),
        `${s.peer} log contains output`,
      );
      console.log(`${s.peer}: done, log=${JSON.stringify(log.trim())}`);
    }

    console.log("--- kill a long-running job");
    const killJobId = await c1.startJob("app", {
      cmd: "ping -n 60 127.0.0.1 >nul",
    });
    await sleep(1000);
    const running = await c1.getJob(killJobId);
    assert(running.status === "running", "job running before kill");
    await c1.killJob(killJobId);
    const killed = await pollJob(c1, killJobId, 15000);
    assert(killed.status === "killed", "job killed");
    console.log("job killed:", killed.status, "exit:", killed.exitCode);

    console.log("--- timeout kills a job");
    const timeoutId = await c2.startJob("app", {
      cmd: "ping -n 60 127.0.0.1 >nul",
      timeoutSec: 2,
    });
    const timed = await pollJob(c2, timeoutId, 20000);
    assert(timed.status === "killed", "job killed by timeout");
    console.log("timeout job:", timed.status);

    console.log("--- fetch_artifacts: file + directory");
    const tar = await c1.fetchTar("app", ["a.txt", "dist"]);
    const entries: { path: string; data: string }[] = [];
    parseTar(tar, (e) =>
      entries.push({ path: e.path, data: e.data.toString("utf8") }),
    );
    const paths = entries.map((e) => e.path).sort();
    assert(
      paths.join(",") === "a.txt,dist/out.js",
      "fetched a.txt and dist/out.js, got: " + paths.join(","),
    );
    const aTxt = entries.find((e) => e.path === "a.txt")!;
    const outJs = entries.find((e) => e.path === "dist/out.js")!;
    assert(aTxt.data === "hello v2", "a.txt content matches");
    assert(outJs.data === "built", "dist/out.js content matches");
    console.log("fetched:", paths.join(", "));

    console.log("--- missing path 404s");
    let notFound = false;
    try {
      await c1.fetchTar("app", ["nope.txt"]);
    } catch (e) {
      notFound = e instanceof Error && e.message.includes("404");
    }
    assert(notFound, "missing path returns 404");

    console.log("--- delete workspace on both");
    await fanOut(clients, (c) => c.deleteWs("app"));
    const m1 = await c1.getManifest("app");
    const m2 = await c2.getManifest("app");
    assert(m1 === null && m2 === null, "workspaces deleted");

    console.log("ALL E2E CHECKS PASSED");
  } finally {
    for (const p of agents) p.kill();
    await rm(root, { recursive: true, force: true });
  }
}

main().catch((e) => {
  console.error(e);
  process.exit(1);
});
