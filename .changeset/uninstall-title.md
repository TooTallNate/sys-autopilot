---
"sys-autopilot": minor
---

Add uninstalling installed titles. `DELETE /titles?id=<titleId>` permanently
deletes an application — its base title, updates, and DLC on every storage —
and removes it from the HOME menu, keeping its save data. Also exposed as the
`uninstall_title` MCP tool. Uses `nsDeleteApplicationCompletely` over the `ns`
access the installer already declares (no new NPDM services).
