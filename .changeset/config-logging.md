---
"sys-autopilot": minor
---

Add optional file logging for the sysmodule. Since a sysmodule has no console
output, diagnostics can now be captured by setting `log = true` in
`config.ini`; output is appended to `sdmc:/config/sys-autopilot/log.txt`.
Logging is off by default and gated at runtime, so it has no cost when unused.
