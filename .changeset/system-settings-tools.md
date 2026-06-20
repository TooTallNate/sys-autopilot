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
- `get_auto_time` / `set_auto_time` — read/set internet clock synchronization.
- `get_datetime` / `set_datetime` — read the local date/time/timezone, or set
  the date/time (written via the network system clock, which the displayed time
  follows). The timezone can't be changed remotely. If internet time sync is
  on, the OS may re-sync later; disable it first to make a manual time stick.
- `status` now also reports `batteryPercent` and `charging`.

Exposed both as MCP tools and `/settings/*` REST endpoints. Adds the `lbl`,
`audctl`, and `psm` services to the NPDM (theme/nickname/auto-time use the
existing `set:sys`, date/time uses `time:s`, airplane mode uses `nifm`).

Note: `set_theme` updates the stored setting immediately but the HOME menu
only reflects it after it reloads (sleep/wake or reboot); brightness and volume
take effect live.
