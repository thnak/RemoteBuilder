#!/usr/bin/env node
import { createInterface } from "node:readline/promises";

import { McpServer } from "@modelcontextprotocol/sdk/server/mcp.js";
import { StdioServerTransport } from "@modelcontextprotocol/sdk/server/stdio.js";

import {
  defaultPeersFile,
  loadPeers,
  type Peer,
  PeerClient,
  savePeers,
} from "./peer.js";
import { registerTools } from "./tools.js";

const version = "0.1.0";

function printHelp(): void {
  console.log(`RemoteBuilder ${version} — offload builds to Windows machines on your LAN

usage:
  remotebuilder                        start the MCP server (stdio)
  remotebuilder pair <name> <host>     pair with an agent running --pair
      [--port N]                       (default port 7333)
  remotebuilder setup                  add a peer by prompting for name/host/port/token
  remotebuilder list                   list configured peers
  remotebuilder help                   show this help

peers file: ${defaultPeersFile()} (override with RB_PEERS_FILE)`);
}

async function addPeer(peer: Peer): Promise<string> {
  const file = defaultPeersFile();
  const peers = await loadPeers(file);
  if (peers.some((p) => p.name === peer.name)) {
    throw new Error(
      `peer "${peer.name}" already exists in ${file}; ` +
        `remove it first or pick another name`,
    );
  }
  peers.push(peer);
  await savePeers(peers, file);
  return file;
}

async function cmdPair(
  name: string,
  host: string,
  port: number,
): Promise<void> {
  let info: { name?: string; port?: number; token?: string };
  try {
    const res = await fetch(`http://${host}:${port}/pair`);
    if (!res.ok) {
      throw new Error(`${res.status} ${res.statusText}`);
    }
    info = (await res.json()) as typeof info;
  } catch (e) {
    throw new Error(
      `pairing failed (${e instanceof Error ? e.message : e}); ` +
        `is the agent running on ${host}:${port} with --pair?`,
    );
  }
  if (!info.token) throw new Error("agent returned no token");
  const file = await addPeer({ name, host, port, token: info.token });
  console.log(`paired "${name}" -> ${info.name ?? host}:${info.port ?? port}`);
  console.log(`saved to ${file}`);
}

async function cmdSetup(): Promise<void> {
  let name = "";
  let host = "";
  let portRaw = "";
  let token = "";
  if (process.stdin.isTTY) {
    const rl = createInterface({
      input: process.stdin,
      output: process.stdout,
    });
    try {
      name = (await rl.question("peer name: ")).trim();
      host = (await rl.question("host (IP or hostname): ")).trim();
      portRaw = (await rl.question("port [7333]: ")).trim();
      token = (
        await rl.question("token (agent prints it on first run): ")
      ).trim();
    } finally {
      rl.close();
    }
  } else {
    // non-TTY stdin: read every piped answer as a line
    const chunks: Buffer[] = [];
    for await (const chunk of process.stdin) {
      chunks.push(chunk as Buffer);
    }
    const lines = Buffer.concat(chunks)
      .toString("utf8")
      .split(/\r?\n/)
      .map((s) => s.trim());
    [name, host, portRaw, token] = lines;
  }
  const port = portRaw ? Number(portRaw) : 7333;
  if (!name || !host || !token) {
    throw new Error("name, host and token are required");
  }
  if (!Number.isInteger(port) || port <= 0 || port > 65535) {
    throw new Error("bad port");
  }
  const file = await addPeer({ name, host, port, token });
  console.log(`saved "${name}" to ${file}`);
}

async function cmdList(): Promise<void> {
  const peers = await loadPeers();
  if (peers.length === 0) {
    console.log("no peers configured (run `remotebuilder pair` or `setup`)");
    return;
  }
  for (const p of peers) {
    console.log(`${p.name}\t${p.host}:${p.port}`);
  }
}

function parsePort(argv: string[]): number {
  const idx = argv.indexOf("--port");
  if (idx === -1) return 7333;
  const v = Number(argv[idx + 1]);
  if (!Number.isInteger(v) || v <= 0 || v > 65535) {
    console.error("bad --port");
    process.exit(2);
  }
  return v;
}

async function main(): Promise<void> {
  const argv = process.argv.slice(2);
  const cmd = argv[0];

  if (cmd === "help" || cmd === "--help" || cmd === "-h") {
    printHelp();
    return;
  }
  if (cmd === "list") {
    await cmdList();
    return;
  }
  if (cmd === "setup") {
    await cmdSetup();
    return;
  }
  if (cmd === "pair") {
    const name = argv[1];
    const host = argv[2];
    if (!name || !host) {
      console.error("usage: remotebuilder pair <name> <host> [--port N]");
      process.exit(2);
    }
    await cmdPair(name, host, parsePort(argv));
    return;
  }
  if (cmd !== undefined) {
    console.error(`unknown command: ${cmd}`);
    printHelp();
    process.exit(2);
  }

  const peers = await loadPeers();
  const clients = peers.map((peer) => new PeerClient(peer));
  const server = new McpServer({ name: "remotebuilder", version });
  registerTools(server, clients);
  const transport = new StdioServerTransport();
  await server.connect(transport);
}

main().catch((e) => {
  console.error(e instanceof Error ? e.message : e);
  process.exit(1);
});
