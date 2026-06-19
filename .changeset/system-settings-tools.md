---
"sys-autopilot": minor
---

Add system-settings tools (REST + MCP):

- `get_theme` / `set_theme` — read/set the UI theme (light or dark).
- `get_nickname` / `set_nickname` — read/set the console's device nickname.
- `get_brightness` / `set_brightness` — read/set screen brightness (0.0–1.0).
- `get_volume` / `set_volume` — read/set master volume (0.0–1.0).
- `airplane_mode` — enable airplane mode (disable wireless). One-way: it cuts
  the server's own connectivity and cannot be undone remotely, so there is no
  remote re-enable.
- `status` now also reports `batteryPercent` and `charging`.

Exposed both as MCP tools and `/settings/*` REST endpoints. Adds the `lbl`,
`audctl`, and `psm` services to the NPDM (theme/nickname use the existing
`set:sys`, airplane mode uses the existing `nifm`).
