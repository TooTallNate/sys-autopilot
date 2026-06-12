---
"sys-autopilot": minor
---

Add `sleep`, `restart`, and `power_off` MCP tools (and matching
`POST /power/sleep|restart|off` REST endpoints) so agents can manage
console power state. Actions execute only after the confirmation
response has been delivered; tool descriptions carry explicit warnings
about server availability (sleep and power-off require physical human
interaction to recover).
