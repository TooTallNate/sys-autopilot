---
"sys-autopilot": minor
---

Add DNS configuration for the active network connection. `GET /network/dns`
reads the current config (automatic/manual + primary/secondary servers) and
`POST /network/dns` sets manual DNS servers or reverts to DHCP — handy for
pointing the console at a custom/black-hole DNS while keeping it on the LAN.
Also exposed as the `get_dns` / `set_dns` MCP tools.

Setting the profile requires the `nifm:a` admin service. Because libnx's
`nifmInitialize` is refcounted and ignores the service type after the first
call, the sysmodule now opens nifm as Admin at boot (a superset of the User
session used for mDNS) so the profile write is authorized.
