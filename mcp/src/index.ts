import { McpServer } from "@modelcontextprotocol/sdk/server/mcp.js";
import { StdioServerTransport } from "@modelcontextprotocol/sdk/server/stdio.js";

import { loadPeers, PeerClient } from "./peer.js";
import { registerTools } from "./tools.js";

async function main(): Promise<void> {
  const peers = await loadPeers();
  const clients = peers.map((peer) => new PeerClient(peer));
  const server = new McpServer({
    name: "remotebuilder",
    version: "0.1.0",
  });
  registerTools(server, clients);
  const transport = new StdioServerTransport();
  await server.connect(transport);
}

main().catch((e) => {
  console.error(e);
  process.exit(1);
});
