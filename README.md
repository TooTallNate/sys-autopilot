# sys-autopilot

A Nintendo Switch (Atmosphère) sysmodule that runs a persistent HTTP server on
the console. It exposes endpoints for taking screenshots, injecting controller
input, and reading/writing files on the SD card — everything an AI agent (or
any remote tool) needs to drive the Switch while testing homebrew applications.

Typical agent loop:

1. `PUT /files?path=/switch/myapp.nro` — deploy a fresh build
2. `GET /screenshot` + `POST /input/tap?...` — navigate to hbmenu and launch it
3. `GET /screenshot` / `GET /files?path=/switch/myapp/log.txt` — observe and iterate

## Requirements

- Switch running [Atmosphère](https://github.com/Atmosphere-NX/Atmosphere) CFW
- Firmware **10.0.0+** (uses the native `caps:sc` JPEG screenshot command)
- Building: [devkitPro](https://devkitpro.org/) with `switch-dev` (devkitA64 + libnx)

## Building

```sh
make            # builds sys-autopilot.nsp (the sysmodule exefs)
make dist       # assembles an SD-card-ready tree under dist/
make -C app     # builds the dev .nro flavor (see below)
```

## Installing

Copy the contents of `dist/` to the root of your SD card:

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

`sdmc:/config/sys-autopilot/config.ini` (created automatically on first
boot if missing):

```ini
[server]
; TCP port the HTTP server listens on.
port = 4150

; Optional HTTP Basic authentication.
; Auth is enforced only when BOTH username and password are set.
username =
password =
```

Changes take effect after a reboot. Note this is plain HTTP — Basic auth
protects against casual LAN access only.

## HTTP API

All endpoints return JSON unless noted. When auth is configured, send an
`Authorization: Basic ...` header with every request.

### Screenshots

```
GET /screenshot[?stack=screenshot|default|lcd|recording|lastframe]
```

Returns the current screen as `image/jpeg` (1280x720). Default layer stack is
`screenshot` (what the Capture button sees).

```sh
curl -o screen.jpg http://<switch-ip>:4150/screenshot
```

### Controller input

A virtual Pro Controller is attached automatically on the first input request
(it becomes player 1 when no physical controllers are connected).

Button names: `A B X Y L R ZL ZR PLUS MINUS UP DOWN LEFT RIGHT LSTICK RSTICK
HOME CAPTURE` (aliases: `START`, `SELECT`, `DUP/DDOWN/DLEFT/DRIGHT`).

```
POST /input/tap?buttons=A[,B,...][&durationMs=100]    press + release (synchronous)
POST /input/hold?buttons=ZL                           press and keep held
POST /input/release?buttons=ZL                        release held buttons
POST /input/clear                                     release everything / center sticks
POST /input/stick?side=left&x=1.0&y=0.0[&durationMs=500]
POST /controller/attach                               attach the virtual pad explicitly
POST /controller/detach                               detach the virtual pad
```

`x`/`y` are floats in `[-1.0, 1.0]`. If `durationMs` is given to
`/input/stick`, the stick recenters afterwards. Durations are capped at 10s.

```sh
# Navigate right twice and confirm
curl -X POST "http://<ip>:4150/input/tap?buttons=RIGHT"
curl -X POST "http://<ip>:4150/input/tap?buttons=RIGHT"
curl -X POST "http://<ip>:4150/input/tap?buttons=A"
# Go to the HOME menu
curl -X POST "http://<ip>:4150/input/tap?buttons=HOME"
```

### Files

All paths are rooted at the SD card (`sdmc:`); `..` traversal is rejected.

```
GET    /files?path=/switch/myapp/log.txt          download / read a file
GET    /files?path=/switch/myapp/log.txt&offset=-4096   tail: last 4 KB
GET    /files?path=/switch/myapp/log.txt&offset=0&length=1024
GET    /files?path=/switch/                       directory listing (JSON)
PUT    /files?path=/switch/myapp.nro              upload (raw request body)
DELETE /files?path=/switch/myapp.nro              delete file / empty dir
```

A negative `offset` reads from the end of the file (handy for tailing logs).

Directory listing response:

```json
{"path":"/switch/","entries":[
  {"name":"myapp.nro","type":"file","size":1234567,"mtime":1760000000},
  {"name":"myapp","type":"dir"}
]}
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
{"version":"1.0.0","firmware":"19.0.1","controllerAttached":true,"uptimeSeconds":4242}
```

## Dev flavor (.nro)

Reloading a sysmodule requires a reboot, so for fast iteration the same server
core also builds as a regular homebrew application:

```sh
make -C app
nxlink -s app/sys-autopilot-app.nro
```

It reads the same `config.ini`, logs requests to the console, and exits with
the `+` button. (While it runs, screenshots show the app's own console, and
`HOME` taps behave as usual — use it for exercising the API, not full flows.)

## Project layout

```
source/main.c          sysmodule entry (heap, __appInit service setup)
source/common/         shared server core
  config.c             INI config loader (writes default on first boot)
  http.c               minimal HTTP/1.1 parser + responses + basic auth
  server.c             listen/accept loop
  routes.c             endpoint dispatch, /status
  screen.c             caps:sc JPEG capture
  input.c              HDLS virtual Pro Controller
  files.c              SD card file/directory endpoints
app/                   dev .nro flavor
sys-autopilot.json  NPDM descriptor (title ID 4200000000004150,
                       services: bsd:u, caps:sc, hid:dbg, set:sys, fsp-srv)
```

## Notes & limitations

- Screenshots fail (HTTP 500 with the Horizon result code) in the rare
  contexts where the OS blocks capture.
- The server is single-threaded by design: input taps with a duration block
  until released, which conveniently serializes agent actions.
- No TLS; treat the API as LAN-trusted.
