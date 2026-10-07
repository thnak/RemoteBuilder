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
npm install -g @thnak/remotebuilder
```

or run it straight from npm without installing:

```
npx -y @thnak/remotebuilder
```

Add it to your MCP client (Command Code or Claude Code):

```
cmdc mcp add remotebuilder -- npx -y @thnak/remotebuilder
claude mcp add remotebuilder -- npx -y @thnak/remotebuilder
```

For the beta channel: `npx -y @thnak/remotebuilder@beta` /
`@thnak/remotebuilder@beta`.

**On each Windows build machine** — the easiest path is the installer
wizard, which does the whole job in one shot:

**`RemoteBuilder-Setup.exe`** (from the
[GitHub Releases](https://github.com/thnak/RemoteBuilder/releases) page, or
built locally with `agent\installer\build.ps1`) installs:

- the agent (`rbagent.exe`) into `%ProgramFiles%\RemoteBuilder`
- a **Windows service** `RemoteBuilderAgent` — auto-start, survives reboots
- a firewall rule for TCP 7333
- the **WinUI 3 tray monitor**, launched at login from the Startup folder

The agent generates its own token on first start; the installer mirrors it
to `%LOCALAPPDATA%\rb-agent\token.txt` and
`%ProgramData%\rb-agent\token.txt` so the tray monitor, the PowerShell
fallback and `install-service.ps1` all agree.

Run it, click through the wizard, done. Service setup is performed by
`setup-service.ps1`, which ships inside the install directory — re-run it
in an elevated PowerShell to repair or re-point the service.

Manual alternative (no installer) — the agent ships as a **prebuilt
binary**, no compiler needed:

- Download `rbagent.exe` from the
  [GitHub Releases](https://github.com/thnak/RemoteBuilder/releases) page, or
- On a Windows machine with Node:
  `npm install -g @thnak/remotebuilder`, then
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

The installer ships a **WinUI 3 tray monitor** (`agent\tray-app\`,
self-contained — no runtime prerequisite). It starts with the session
and gives you:

- live inventory (cores, load, memory, running/queued jobs)
- job list with status, exit code and log size (progress)
- balloon alerts when a job starts or finishes
- log tail pane for the selected job (auto-follows the log)
- **Check for updates** and **Open releases page** menu items

Left-click the icon to toggle the window; right-click for the menu.

A dependency-free PowerShell fallback lives in `agent/tray/tray.cmd`
(same features, no build step) for machines where you'd rather not run
the installer.

Both talk to `127.0.0.1:7333` by default; configure with
`RB_AGENT_HOST`, `RB_AGENT_PORT`, `RB_AGENT_TOKEN` (the token defaults
to `%LOCALAPPDATA%\rb-agent\token.txt`, then
`%ProgramData%\rb-agent\token.txt`).

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
(`npm i -g @thnak/remotebuilder`), run the agent on each Windows box, then
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

When a newer **agent** is available, download the new
`RemoteBuilder-Setup.exe` from the
[releases page](https://github.com/thnak/RemoteBuilder/releases) and run
it again — it replaces the files, recreates the service and restarts it.
For a manual install, re-run `rbagent`, or stop any running `rbagent` and
re-run the elevated `install-service.ps1`:

```powershell
cd agent\service
.\install-service.ps1
```

The tray monitor also has **Check for updates** and
**Open releases page** in its menu; it compares its own version against
the latest GitHub release tag.

The WinUI tray monitor is a compiled exe, so it updates with the
installer. The PowerShell fallback lives in the repo checkout — `git pull`
in the RemoteBuilder directory, then restart `agent\tray\tray.cmd`.

## MCP tools

The MCP server exposes six tools. `sync` and `exec` accept a
**peer selector**: a single peer name, a list of names (fan-out —
every peer runs it, results aggregated per peer), or `"auto"`
(least-loaded reachable peer). `get_result` and `kill` address
jobs by `{peer, jobId}` pairs, so fan-out results can be
tracked individually.

| Tool | Purpose | Key parameters |
| --- | --- | --- |
| `list_peers` | Live inventory of all configured peers | — |
| `sync` | Gitignore-aware directory sync to a workspace | `peer`, `dir`, `ws`, `extraIgnore?` |
| `exec` | Start a build/test command on a workspace | `peer`, `ws`, `cmd`, `cwd?`, `env?`, `timeoutSec?` |
| `get_result` | Poll jobs to completion; returns status, exit code, log tail | `jobs`, `waitMs?`, `pollMs?`, `tailBytes?` |
| `kill` | Kill running jobs (whole process tree) | `jobs` |
| `fetch_artifacts` | Pull files/directories from a workspace to the local machine | `peer`, `ws`, `paths`, `outDir?` |

### `list_peers`

No parameters. Returns each peer with live inventory:

```json
{
  "peers": [
    { "name": "winagent", "host": "192.168.1.80", "port": 7333,
      "inventory": { "name": "THNAK", "cores": 12, "cpuLoadPct": 4,
                     "memFreeMB": 28554, "memTotalMB": 40347,
                     "jobs": { "running": 0, "queued": 0 } } }
  ]
}
```

Unreachable peers appear with an `inventory.error` message
instead of crashing the call.

### `sync`

- `peer` — peer name, list of names, or `"auto"`
- `dir` — local directory to sync (absolute or relative)
- `ws` — workspace name on the peer (1–64 chars)
- `extraIgnore` — extra gitignore-style patterns to skip

Walks `dir` (honoring `.gitignore` at every level, skipping
`.git`), hashes every file (sha1), compares with the peer's
manifest, and uploads only changed/new files. Files deleted
locally are pruned on the peer. Returns per-peer
`{applied, bytes, manifestFiles, pruned, changed}`.

### `exec`

- `peer` — peer name, list of names, or `"auto"`
- `ws` — workspace to run in (must have been synced first)
- `cmd` — command line; runs via `cmd.exe /d /s /c` with
  `cwd` inside the workspace
- `cwd` — subdirectory of the workspace (optional)
- `env` — extra environment variables merged into the job
- `timeoutSec` — kill the whole process tree after this many
  seconds (`0` = no timeout, default)

Returns one `jobId` per peer:

```json
{ "ws": "app", "cmd": "npm run build",
  "results": [{ "jobId": "j1" }] }
```

Jobs run inside a **Windows Job Object** with kill-on-close,
so kill/timeout take down the entire process tree. stdout+stderr
go to `<ws>/jobs/<id>/log.txt`. Up to `--maxjobs` jobs run
concurrently per agent (FIFO queue beyond that).

### `get_result`

- `jobs` — array of `{peer, jobId}` (the ids `exec` returned)
- `waitMs` — how long to keep polling before returning
  (default `0` = return current state immediately)
- `pollMs` — poll interval (default `500`, minimum `50`)
- `tailBytes` — how many log bytes to return (default `8192`)

For each job returns `status` (`queued`/`running`/`done`/
`killed`/`failed`), `exitCode`, `logBytes`, and the last
`tailBytes` of the log (`log` is `null` if the job is still
running when `waitMs` elapses).

### `kill`

- `jobs` — array of `{peer, jobId}`

Kills each running job's whole process tree. Returns
`{peer, jobId, killed}` per job (with `error` if it failed).

### `fetch_artifacts`

- `peer` — a single peer name (no fan-out, no `"auto"`)
- `ws` — workspace to fetch from
- `paths` — files or directories inside the workspace
- `outDir` — local destination (default
  `.remotebuilder-out/<peer>/<ws>`)

Streams a tar of the requested paths from the peer and writes
them locally. Returns the fetched file list with sizes:

```json
{ "peer": "winagent", "outDir": ".remotebuilder-out/winagent/app",
  "files": [{ "path": "dist/app.exe", "size": 40960 }] }
```

Paths are validated: absolute paths and `..` escapes are
rejected.

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

Build the installer — publishes the WinUI tray monitor self-contained
(needs the .NET 10 SDK) and compiles the wizard with Inno Setup 7:

```powershell
cd agent\installer
.\build.ps1       # outputs agent/build/RemoteBuilder-Setup.exe
```
