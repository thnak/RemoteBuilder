# RemoteBuilder agent protocol

Base URL: `http://<host>:<port>`. Every request carries
`Authorization: Bearer <token>`; missing or wrong tokens get `401`.
JSON error bodies look like `{"error": "..."}`.

## GET /pair

Only served while the agent runs with `--pair`; **no auth required**.
Returns the peer's registration info so `remotebuilder pair <name> <host>`
can register it without copying tokens by hand:

```json
{ "name": "BUILD1", "port": 7333, "token": "<token>" }
```

Stop the agent (Ctrl+C) when pairing is done — `--pair` is the only way
the token is ever served unauthenticated.

## GET /inventory

Machine inventory:

```json
{
  "name": "BUILD1",
  "agentVer": "0.1.0",
  "os": "windows",
  "arch": "x64",
  "cores": 12,
  "cpuLoadPct": 17,
  "memTotalMB": 32768,
  "memFreeMB": 18432,
  "jobs": { "running": 0, "queued": 0 },
  "workspaces": [{ "ws": "app", "countFiles": 3, "bytes": 30 }]
}
```

## GET /manifest?ws=<ws>

The workspace's authoritative manifest, or `404` if the workspace has never
been synced:

```json
{ "v": 1, "files": [{ "p": "src/main.ts", "s": 1234, "h": "<sha1>" }] }
```

## POST /sync?ws=<ws>

Body (raw): a ustar tar of changed files plus a `manifest.json` entry.
The agent applies files, stores the manifest, and **prunes** files that are
in the workspace but not in the new manifest (except `manifest.json` itself).

Response:

```json
{ "ok": true, "applied": 3, "bytes": 30, "manifestFiles": 3, "pruned": 1 }
```

While a job runs in a workspace, `/sync` returns `409` (workspace busy).

## POST /jobs?ws=<ws>

Queue a command. Body:

```json
{
  "cmd": "npm run build",
  "cwd": "src",            // optional, relative to the workspace
  "env": { "CI": "true" }, // optional, merged into the environment
  "timeoutSec": 600        // optional, 0 = no timeout
}
```

Response `201`: `{"jobId":"j1","status":"queued"}`.

Jobs run via `cmd.exe /d /s /c "<cmd> 2>&1"` inside a **Windows Job Object**
with kill-on-close, so kill/timeout terminate the entire process tree. A
single dispatcher thread runs up to `--maxjobs` jobs concurrently (FIFO
otherwise). stdout+stderr go to `<ws>/jobs/<id>/log.txt`.

## GET /jobs/<id>

```json
{
  "jobId": "j1", "ws": "app", "cmd": "npm run build",
  "status": "done",          // queued | running | done | killed | failed
  "exitCode": 0,
  "timeoutSec": 0,
  "startedAt": "2026-10-06T12:00:00Z",
  "endedAt": "2026-10-06T12:00:41Z",
  "logBytes": 4096
}
```

## GET /jobs/<id>/log?offset=<n>

Log bytes from `offset` (octet-stream). The `X-RB-Log-Bytes` header carries
the current total, so clients poll the tail by tracking their offset.

## DELETE /jobs/<id>

Kill the job (whole process tree). `200 {"killed":true}`, `404` if the job
already finished or does not exist.

## POST /fetch-tar?ws=<ws>

Body: `{"paths":["dist","out.log"]}`. Returns a ustar tar (octet-stream)
containing the named files, or every file under named directories (recursive,
sorted). `404` for missing paths, `409` while a job runs in the workspace.

## DELETE /ws/<ws>

Delete the whole workspace directory. `200 {"removed":true}`.

## Workspace names

1-64 chars of `[a-zA-Z0-9-_.]`, must not start with `.` and must not
contain `..`. Tar entry paths and `paths` entries must be relative,
forward-slash, no `..`, no drive letters, no Windows reserved device names.
