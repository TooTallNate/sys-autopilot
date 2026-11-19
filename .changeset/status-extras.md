---
"sys-autopilot": minor
---

`GET /status` (and the `status` MCP tool) now also report the Atmosphère
version (`atmosphere`), the connected Wi-Fi network (`ssid`, `null` when not on
Wi-Fi), and SD card capacity (`sdFreeBytes` / `sdTotalBytes`).

Also fixes Atmosphère version detection, which never ran in the sysmodule:
it was gated on libnx's `hosversionIsAtmosphere()`, which is only set for
homebrew launched through the loader, so the `atmosphere` mDNS TXT key was
always missing. The Exosphère API version is now queried directly.
