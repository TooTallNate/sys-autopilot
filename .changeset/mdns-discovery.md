---
"sys-autopilot": minor
---

Add mDNS / DNS-SD network discovery. The console now advertises itself on the
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
