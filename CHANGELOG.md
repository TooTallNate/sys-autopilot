# sys-autopilot

## 1.4.0

### Minor Changes

- [#11](https://github.com/TooTallNate/sys-autopilot/pull/11) [`9392d7e`](https://github.com/TooTallNate/sys-autopilot/commit/9392d7e8e8b66da6955ce2c00a234fc35ec69fbb) Thanks [@TooTallNate](https://github.com/TooTallNate)! - Add optional file logging for the sysmodule. Since a sysmodule has no console
  output, diagnostics can now be captured by setting `log = true` in
  `config.ini`; output is appended to `sdmc:/config/sys-autopilot/log.txt`.
  Logging is off by default and gated at runtime, so it has no cost when unused.

- [#10](https://github.com/TooTallNate/sys-autopilot/pull/10) [`4851df6`](https://github.com/TooTallNate/sys-autopilot/commit/4851df624f862991392fd5f3fb8473da323b8bd9) Thanks [@TooTallNate](https://github.com/TooTallNate)! - Add mDNS / DNS-SD network discovery. The console now advertises itself on the
  local network as `<hostname>.local` and publishes the `_sys-autopilot._tcp`
  service with a TXT record describing the version/path/auth/model/firmware/
  Atmosphère version, so MCP clients and `curl` can target a stable name instead
  of a hard-coded IP.

  - New `hostname` key in `config.ini`. When blank it auto-generates
    `switch-<last 4 of serial>`, which is unique per console so two devices on
    the same network don't collide.
  - The LAN address is obtained from `nifm` (a sysmodule's `gethostid()` only
    ever returns loopback), with the server retrying until the interface is up.
  - New `scripts/discover.sh` helper browses for consoles on the LAN.

## 1.3.0

### Minor Changes

- [#8](https://github.com/TooTallNate/sys-autopilot/pull/8) [`4a246b9`](https://github.com/TooTallNate/sys-autopilot/commit/4a246b9cb581cc64744d2a19e6f0d7ea2dda076d) Thanks [@TooTallNate](https://github.com/TooTallNate)! - Add file hashing for upload verification. A new `hash_file` MCP tool computes a
  file's SHA-256 (hardware-accelerated and streamed in 32 KB chunks, so memory use
  is constant for any file size) and returns just the 64-char hex digest. Passing
  an `expected` digest also reports a `matched` boolean, so an agent can confirm an
  upload landed intact in a single round-trip. The same digest is available over
  the raw HTTP API via `GET /files/hash?path=...`.

## 1.2.0

### Minor Changes

- [#7](https://github.com/TooTallNate/sys-autopilot/pull/7) [`8552135`](https://github.com/TooTallNate/sys-autopilot/commit/8552135cb90a8a102b2857e0d73b64a9cd8916a2) Thanks [@TooTallNate](https://github.com/TooTallNate)! - Add `create_token` and `revoke_token` MCP tools. `create_token` mints a
  bearer token over the already-authenticated MCP channel; `revoke_token`
  removes a previously issued token again (agents can clean up after
  themselves). This lets agents use the raw HTTP
  API (e.g. `curl -T` for large file uploads, where base64 tool-call
  content would be impractical) without being handed credentials out of
  band. Tokens share the `tokens.txt` store and revocation model with the
  OAuth flow.

- [#6](https://github.com/TooTallNate/sys-autopilot/pull/6) [`f5f4be8`](https://github.com/TooTallNate/sys-autopilot/commit/f5f4be8855d892c438540863a6fedb8fa9fd287c) Thanks [@TooTallNate](https://github.com/TooTallNate)! - Input MCP tools (`tap_buttons`, `tap_sequence`, `hold_buttons`,
  `release_buttons`, `set_stick`, `clear_input`) accept an optional
  `"screenshot": true` argument that appends a screenshot (taken after an
  optional `screenshotDelayMs`, default 250ms) to the tool result as an
  image content block — saving the agent a separate screenshot round trip
  after every input.

- [#2](https://github.com/TooTallNate/sys-autopilot/pull/2) [`7930a11`](https://github.com/TooTallNate/sys-autopilot/commit/7930a11f81097124c3411eaaa56f8706344a28ea) Thanks [@TooTallNate](https://github.com/TooTallNate)! - Add an OAuth 2.1 browser login flow for MCP clients. When username/password
  are configured, clients like Claude Code now authenticate with zero manual
  header configuration: the first 401 triggers metadata discovery (RFC 9728 /
  RFC 8414), dynamic client registration (RFC 7591), and a browser login page
  served by the Switch; after signing in, the client receives a non-expiring
  bearer token via the authorization-code + PKCE S256 grant. Tokens are
  persisted to `config/sys-autopilot/tokens.txt` (revoke by deleting a line).
  Also adds CORS support for browser-based MCP clients.

- [#5](https://github.com/TooTallNate/sys-autopilot/pull/5) [`0a3b148`](https://github.com/TooTallNate/sys-autopilot/commit/0a3b14837ec838816bcc1281f7489cf2d398faf8) Thanks [@TooTallNate](https://github.com/TooTallNate)! - Add `sleep`, `restart`, and `power_off` MCP tools (and matching
  `POST /power/sleep|restart|off` REST endpoints) so agents can manage
  console power state. Actions execute only after the confirmation
  response has been delivered; tool descriptions carry explicit warnings
  about server availability (sleep and power-off require physical human
  interaction to recover; restart may too, depending on bootloader
  autoboot configuration).

### Patch Changes

- [#4](https://github.com/TooTallNate/sys-autopilot/pull/4) [`984167d`](https://github.com/TooTallNate/sys-autopilot/commit/984167df764f4ab3cb1f041e32070b5252faa56e) Thanks [@TooTallNate](https://github.com/TooTallNate)! - Fix console crash on sleep (hard restart on wake). Two changes were
  required: the HDLS work buffer (transfer memory mapped into the hid
  sysmodule) is now attached lazily and released when the console
  prepares to sleep, and the sysmodule registers as a PSC power
  management module so it can close its listener socket and quiesce all
  bsd IPC before acknowledging the sleep transition. Previously the held
  HDLS state and live socket activity made the sleep sequence fail (omm
  abort with psc error 2165-1001, occasional bsdsockets aborts), forcing
  a full reboot on wake. The virtual controller re-attaches automatically
  on the first input request after wake.

## 1.1.0

Initial public release.

- Persistent HTTP server sysmodule (auto-starts at boot via boot2)
- Native MCP endpoint at `POST /mcp` (stateless Streamable HTTP, JSON-RPC 2.0)
  with 12 tools: screenshots returned as image content, controller input
  (taps, sequences, holds, sticks), and SD card file access
- REST API with JSON request bodies for input endpoints
- JPEG screenshots via `caps:sc` (firmware 10.0.0+)
- Virtual Pro Controller input injection via `hiddbg` HDLS
- File upload/download/listing/deletion rooted at `sdmc:/`; MCP
  `upload_file` content is streamed to disk so size is bounded by the SD
  card, not RAM
- INI configuration (`/config/sys-autopilot/config.ini`): port, bearer
  token and/or HTTP Basic credentials
- Dev `.nro` flavor of the same server for fast iteration via nxlink
