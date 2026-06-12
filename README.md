# sys-autopilot

A Nintendo Switch (Atmosphère) sysmodule that runs a persistent HTTP server on
the console. It exposes a REST API **and a native MCP (Model Context Protocol)
endpoint** for taking screenshots, injecting controller input, and
reading/writing files on the SD card — everything an AI agent needs to drive
the Switch while testing homebrew applications.

Typical agent loop:

1. `curl -T myapp.nro` — deploy a fresh build
2. `screenshot` + `tap_buttons` MCP tools — navigate to hbmenu and launch it
3. `screenshot` / `read_file` — observe the app and its log files, iterate

## Requirements

- Switch running [Atmosphère](https://github.com/Atmosphere-NX/Atmosphere) CFW
- Firmware **10.0.0+** (uses the native `caps:sc` JPEG screenshot command)
- Building: [devkitPro](https://devkitpro.org/) with `switch-dev` (devkitA64 + libnx)

## Building

```sh
make            # builds sys-autopilot.nsp (the sysmodule exefs)
make dist       # assembles an SD-card-ready tree under dist/
make -C app     # builds the dev .nro flavor (see below)
./tests/run.sh  # host-side test suite (no devkitPro required)
```

## Installing

Download the latest `sys-autopilot-<version>.zip` from
[Releases](https://github.com/TooTallNate/sys-autopilot/releases) and extract
it to the root of your SD card. Or build from source and copy the contents of
`dist/` to the root of your SD card:

```
atmosphere/contents/4200000000004150/exefs.nsp
atmosphere/contents/4200000000004150/flags/boot2.flag
config/sys-autopilot/config.ini
```

Reboot. The server starts automatically at boot (boot2) and listens on port
4150 by default.

> Tip: disable auto-sleep in System Settings while driving the console
> remotely — sleep mode drops the network connection.

## Configuration

`sdmc:/config/sys-autopilot/config.ini` (created automatically on first boot
if missing):

```ini
[server]
; TCP port the HTTP server listens on.
port = 4150

; Optional authentication. Auth is enforced when EITHER a bearer token
; is set, or both username and password are set (HTTP Basic).
; Clients may then use 'Authorization: Bearer <token>' or Basic auth.
; Setting username+password also enables the OAuth browser login flow
; for MCP clients (see below).
token =
username =
password =
```

Changes take effect after a reboot. Note this is plain HTTP — auth protects
against casual LAN access only. OAuth-issued tokens live in
`config/sys-autopilot/tokens.txt` next to this file.

## MCP (Model Context Protocol)

The sysmodule speaks MCP natively at `POST /mcp` (stateless Streamable HTTP
transport, JSON-RPC 2.0). Point any MCP client directly at the console:

```jsonc
// .mcp.json (Claude Code), or equivalent in Cursor/VS Code/etc.
{
  "mcpServers": {
    "switch": {
      "type": "http",
      "url": "http://<switch-ip>:4150/mcp",
      "headers": {
        "Authorization": "Bearer <token from config.ini>"
      }
    }
  }
}
```

Omit `headers` when auth is not configured.

### OAuth browser login (no manual headers)

When `username` and `password` are set in `config.ini`, sys-autopilot also
acts as a minimal OAuth 2.1 authorization server. MCP clients that support
the spec's auth flow (Claude Code, etc.) need **zero manual configuration**:
add the server URL, and on the first 401 the client discovers the OAuth
metadata, opens your browser at a login page served by the Switch, and after
you sign in with the config.ini credentials it receives a bearer token
automatically.

```sh
claude mcp add --transport http switch http://<switch-ip>:4150/mcp
# first use triggers the browser login
```

Details:

- Issued tokens are **non-expiring** and stored one-per-line in
  `sdmc:/config/sys-autopilot/tokens.txt` (with an `# issued <date>` comment).
  Revoke a token by deleting its line — the file is re-read when it changes.
- Implements RFC 9728 protected-resource metadata, RFC 8414 AS metadata,
  RFC 7591 dynamic client registration, and the authorization-code grant
  with PKCE S256 (required).
- The static `token =` and HTTP Basic options still work for clients without
  OAuth support.
- Note: it's OAuth over plain HTTP on your LAN — the flow is for
  *convenience*, not transport security. A few clients hard-require HTTPS
  for OAuth; for those, fall back to a static token header.

### Tools

| Tool | Description |
|---|---|
| `screenshot` | Returns the current screen as an **image content block** — the agent sees it directly |
| `tap_buttons` | Press + release buttons (`buttons: ["A"]`, optional `durationMs`) |
| `tap_sequence` | Up to 32 taps in one call (menu navigation without round-trips) |
| `hold_buttons` / `release_buttons` | Persistent button state |
| `set_stick` | Analog stick (`side`, `x`/`y` in -1..1, optional `durationMs`) |
| `clear_input` | Release everything, recenter sticks |
| `status` | Server version, firmware, controller state, uptime |
| `list_directory` | JSON listing of an SD card directory |
| `read_file` | Read text files (32 KB pages, negative `offset` = tail) — ideal for logs |
| `upload_file` | Write a file (base64 `content`, **streamed to SD — no size cap**) |
| `delete_file` | Delete a file / empty directory |

Button names: `A B X Y L R ZL ZR PLUS MINUS UP DOWN LEFT RIGHT LSTICK RSTICK
HOME CAPTURE` (aliases: `START`, `SELECT`, `DUP/DDOWN/DLEFT/DRIGHT`).

`upload_file` note: content is streamed through a JSON scanner and
base64-decoded straight to disk, so the file size is bounded by the SD card,
not RAM. But MCP tool arguments are generated token-by-token by the model, so
multi-megabyte uploads are context-expensive — deploy `.nro` builds with
`curl -T` against the raw HTTP API instead.

## REST API

All endpoints return JSON unless noted. POST endpoints take JSON request
bodies. When auth is configured, send `Authorization: Bearer <token>` (or
Basic) with every request.

### Screenshots

```
GET /screenshot[?stack=screenshot|default|lcd|recording|lastframe]
```

Returns the current screen as `image/jpeg` (1280x720).

```sh
curl -o screen.jpg http://<switch-ip>:4150/screenshot
```

### Controller input

A virtual Pro Controller is attached automatically on the first input request
(it becomes player 1 when no physical controllers are connected).

```
POST /input/tap      {"buttons":["A","B"],"durationMs":100}
POST /input/hold     {"buttons":["ZL"]}
POST /input/release  {"buttons":["ZL"]}
POST /input/stick    {"side":"left","x":1.0,"y":0.0,"durationMs":500}
POST /input/clear    (empty body)
POST /controller/attach | /controller/detach
```

`x`/`y` are floats in `[-1.0, 1.0]`. With `durationMs`, `/input/stick`
recenters afterwards. Durations are capped at 10s.

```sh
curl -X POST http://<ip>:4150/input/tap \
     -H 'Content-Type: application/json' \
     -d '{"buttons":["RIGHT"]}'
```

### Files

All paths are rooted at the SD card (`sdmc:`); `..` traversal is rejected.

```
GET    /files?path=/switch/myapp/log.txt          download / read a file
GET    /files?path=/switch/myapp/log.txt&offset=-4096   tail: last 4 KB
GET    /files?path=/switch/                       directory listing (JSON)
PUT    /files?path=/switch/myapp.nro              upload (raw request body)
DELETE /files?path=/switch/myapp.nro              delete file / empty dir
```

```sh
# Deploy a build
curl -T myapp.nro "http://<ip>:4150/files?path=/switch/myapp.nro"
# Tail a log
curl "http://<ip>:4150/files?path=/switch/myapp/debug.log&offset=-2048"
```

### Status

```
GET /status
```

```json
{"version":"1.1.0","firmware":"19.0.1","controllerAttached":true,"uptimeSeconds":4242}
```

## Dev flavor (.nro)

Reloading a sysmodule requires a reboot, so for fast iteration the same server
core also builds as a regular homebrew application:

```sh
make -C app
nxlink -s app/sys-autopilot-app.nro
```

It reads the same `config.ini`, logs requests to the console, and exits with
the `+` button.

## Project layout

```
source/main.c          sysmodule entry (heap, __appInit service setup)
source/common/         shared server core
  config.c             INI config loader (writes default on first boot)
  http.c               minimal HTTP/1.1 parser + responses + Bearer/Basic auth
  server.c             non-blocking listen/accept loop
  routes.c             REST endpoint dispatch, /status
  mcp.c                MCP endpoint: JSON-RPC 2.0 dispatch + tools
  mcp_tools.h          generated tools/list payload (scripts/gen_tools.py)
  jstream.c            streaming JSON pre-pass (diverts upload content to disk)
  json.c               jsmn wrapper helpers (parse/get/escape)
  base64.c             streaming base64 encoder/decoder
  buttons.c            button name table (host-testable)
  apiargs.c            shared JSON argument parsing (REST + MCP)
  screen.c             caps:sc JPEG capture
  input.c              HDLS virtual Pro Controller
  files.c              SD card file/directory endpoints + helpers
lib/jsmn/              vendored JSON tokenizer (MIT)
app/                   dev .nro flavor
tests/                 host-side test suite (./tests/run.sh)
sys-autopilot.json     NPDM descriptor (title ID 4200000000004150,
                       services: bsd:u, caps:sc, hid:dbg, set:sys, fsp-srv)
```

## Releases & contributing

CI (GitHub Actions) runs the host test suite on every push/PR and builds the
sysmodule inside the `devkitpro/devkita64` container, uploading the SD card
layout as an artifact.

Releases are managed with [changesets](https://github.com/changesets/changesets):

```sh
pnpm install
pnpm changeset        # record a change + semver intent
```

When changesets land on `main`, the release workflow opens a "Version
Packages" PR; merging it bumps `package.json` (which the Makefiles inject
into the build as `APP_VERSION`), updates `CHANGELOG.md`, and publishes a
GitHub Release with the SD-card zip and dev `.nro` attached.

## Notes & limitations

- Screenshots fail (HTTP 500 / tool error with the Horizon result code) in the
  rare contexts where the OS blocks capture.
- The server is single-threaded by design: input taps with a duration block
  until released, which conveniently serializes agent actions. Socket I/O is
  non-blocking under the hood (poll()-based with a 10s inactivity timeout),
  so a stalled or misbehaving client cannot wedge the server.
- MCP transport details: stateless (no session IDs), plain JSON responses (no
  SSE), `GET /mcp` returns 405, JSON-RPC batching unsupported (removed in MCP
  2025-06-18 anyway).
- No TLS; treat the API as LAN-trusted.
