# RemoteBuilder

Offload CPU-heavy work (builds, tests, anything) to Windows machines on
your LAN, from inside your editor via MCP.

Two components:

- **MCP server / client** (TypeScript) — runs wherever your MCP client
  runs: **Linux, macOS, or Windows** (Node 18+). Exposes the
  `sync`, `exec`, `get_result`, `kill`, `fetch_artifacts`, `list_peers`
  tools. The client computes all file hashes and drives sync diffs.
- **Agent daemon** (`rbagent.exe`, C++, **Windows-only**) — runs on each
  build machine. Token-authed HTTP, streaming tar workspace sync, Windows
  Job Object process runner (kill takes down the whole process tree),
  per-job log files with byte-offset tails.

## Install

**On your machine** (any OS with Node 18+):

```
npm install -g remotebuilder
```

or run it straight from npm without installing:

```
npx -y remotebuilder
```

Add it to your MCP client (Command Code or Claude Code):

```
cmdc mcp add remotebuilder -- npx -y remotebuilder
claude mcp add remotebuilder -- npx -y remotebuilder
```

For the beta channel: `npx -y remotebuilder@beta` / `remotebuilder@beta`.

**On each Windows build machine** — the agent ships as a **prebuilt
binary**, no compiler needed:

- Download `rbagent.exe` from the
  [GitHub Releases](https://github.com/thnak/RemoteBuilder/releases) page, or
- On a Windows machine with Node: `npm install -g remotebuilder`, then
  run `rbagent` (the package ships the exe as a `rbagent` command).

First run generates a token and stores it in
`%LOCALAPPDATA%\rb-agent\token.txt`, then listens on port 7333.

To run it permanently as a Windows service (auto-start, survives
reboots) with the firewall port opened, run in an **elevated**
PowerShell:

```powershell
cd agent\service
.\install-service.ps1            # -Name/-Port/-Root to customize
```

For a quick start, just run `rbagent` (or `rbagent --pair` while
registering a peer).

## System tray

`agent/tray/tray.cmd` opens a tray monitor for the agent:

- live inventory (cores, load, memory, running/queued jobs)
- job list with status, exit code and log size (progress)
- balloon alerts when a job starts or finishes
- log tail pane for the selected job (auto-follows the log)

It talks to `127.0.0.1:7333` by default; configure with
`RB_AGENT_HOST`, `RB_AGENT_PORT`, `RB_AGENT_TOKEN` (the token
defaults to `%LOCALAPPDATA%\rb-agent\token.txt`).

## Register a peer (one command)

On the build machine, run the agent in pairing mode:

```
rbagent --pair
```

On your machine, pair with it (fetches the token over the LAN and writes
your peers file automatically):

```
remotebuilder pair build1 192.168.1.20
# custom port:  remotebuilder pair build1 192.168.1.20 --port 7334
```

That's it — `~/.remotebuilder/peers.json` now holds
`{name, host, port, token}` for the peer.

Alternatives:

- `remotebuilder setup` — prompt for name/host/port/token manually
  (copy the token the agent prints on first run)
- `remotebuilder list` — show configured peers
- `RB_PEERS_FILE` env var — use a different peers file

Pairing mode only opens `GET /pair` on the LAN while it runs; stop it
(Ctrl+C) when done. Without `--pair` every endpoint requires the token.

The client runs on any OS and the agent on Windows, so a typical
fleet is a Linux/macOS dev machine driving one or more Windows
build machines — e.g. install the client on the Linux box
(`npm i -g remotebuilder`), run the agent on each Windows box, then
`remotebuilder pair <name> <windows-ip>` from the Linux box.

## Updating

Check for updates (client on npm, agent on GitHub Releases):

```
remotebuilder update
```

Install the newest client (add `--beta` for the beta channel):

```
remotebuilder update --install
remotebuilder update --install --beta
```

When a newer **agent** is available, download the new `rbagent.exe`
from the [releases page](https://github.com/thnak/RemoteBuilder/releases),
then restart it — either re-run `rbagent`, or (if installed as a
service) stop any running `rbagent` and run the elevated installer
again:

```powershell
cd agent\service
.\install-service.ps1
```

The tray app also has **Check for updates** and
**Open releases page** in its menu.

The tray app itself lives in the repo checkout, so it updates
with the repo: `git pull` in the RemoteBuilder directory, then
restart `agent\tray\tray.cmd`. Its **Check for updates** menu
item reports when a newer release is out (and compares the
local repo version against the GitHub release tag).

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
rbagent.exe [--port N] [--token T] [--root DIR] [--name NAME] [--maxjobs N] [--pair]
```

- `--port` HTTP listen port (default 7333)
- `--token` auth token (default: auto-generated, persisted)
- `--root` workspaces root (default `%LOCALAPPDATA%\rb-agent\workspaces`)
- `--name` machine name reported to the coordinator
- `--maxjobs` concurrent jobs (default 2)
- `--pair` expose unauthenticated `GET /pair` for one-command registration

See [PROTOCOL.md](PROTOCOL.md) for the HTTP protocol.

## Development

```powershell
npm install --include=dev
npm run build     # tsc -> mcp/dist
npm test          # 22 unit tests (tar, sync, gitignore)
npm run e2e       # two-agent e2e on ports 7333/7334
```

Rebuild the agent (auto-detects MSVC / clang++ / g++):

```powershell
cd agent
.\build.ps1       # outputs agent/build/rbagent.exe
```
