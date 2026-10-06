# RemoteBuilder

Offload CPU-heavy work (builds, tests, anything) to other Windows machines on
your LAN, from inside your editor via MCP.

Two components:

- **MCP server** (TypeScript, runs where Command Code runs) — exposes the
  `sync`, `exec`, `get_result`, `kill`, `fetch_artifacts`, `list_peers` tools.
  The client side computes all file hashes and drives sync diffs.
- **Agent daemon** (`rbagent.exe`, C++, Windows-only) — runs on each build
  machine. Token-authed HTTP, streaming tar workspace sync, Windows Job
  Object process runner (kill takes down the whole process tree), per-job
  log files with byte-offset tails.

## Setup

Build the agent on each build machine:

```powershell
cd agent
.\build.ps1        # auto-detects MSVC / g++ / clang
```

First run generates a token and stores it in
`%LOCALAPPDATA%\rb-agent\token.txt`, then listens on port 7333.

Register peers on the MCP side in `~/.remotebuilder/peers.json` (or set
`RB_PEERS_FILE`):

```json
{
  "peers": [
    { "name": "build1", "host": "192.168.1.20", "port": 7333, "token": "..." },
    { "name": "build2", "host": "192.168.1.21", "port": 7333, "token": "..." }
  ]
}
```

Run the MCP server over stdio (add to your MCP client config):

```
node <repo>/mcp/dist/index.js
```

## Tools

| Tool | Purpose |
| --- | --- |
| `list_peers` | Live inventory of all configured peers (cores, load, memory, jobs) |
| `sync` | Gitignore-aware whole-directory sync to a workspace. Peer can be a name, a list of names (fan-out), or `"auto"` (least-loaded reachable peer) |
| `exec` | Start a command on a workspace. Fan-out to several peers returns one job id per peer |
| `get_result` | Poll jobs until terminal state; returns status, exit code, log tail |
| `kill` | Kill running jobs (whole process tree) |
| `fetch_artifacts` | Pull files/directories from a peer workspace into `.remotebuilder-out/<peer>/<ws>` |

## How sync works

The client walks the directory (honoring `.gitignore` files at every level,
skipping `.git`), hashes every file (sha1), and fetches the peer's manifest.
Only changed/new files are packed into a ustar tar (plus an authoritative
`manifest.json`) and streamed to the peer. Files that no longer exist locally
are pruned on the peer. The agent never hashes anything.

## Fleet semantics

`sync` and `exec` accept a peer selector: a single peer name, a list of names
(every peer runs it, results aggregated per peer), or `"auto"` — which samples
inventory from all peers and picks the least-loaded one (running jobs, then
queued jobs, then CPU load).

## Agent CLI

```
rbagent.exe [--port N] [--token T] [--root DIR] [--name NAME] [--maxjobs N]
```

- `--port` HTTP listen port (default 7333)
- `--token` auth token (default: auto-generated, persisted)
- `--root` workspaces root (default `%LOCALAPPDATA%\rb-agent\workspaces`)
- `--name` machine name reported to the coordinator
- `--maxjobs` concurrent jobs (default 2)

See [PROTOCOL.md](PROTOCOL.md) for the HTTP protocol.

## Development

```powershell
npm install --include=dev
npm run build     # tsc -> mcp/dist
npm test          # 22 unit tests (tar, sync, gitignore)
npm run e2e       # two-agent e2e on ports 7333/7334
```
