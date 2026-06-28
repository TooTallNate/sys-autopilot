---
"sys-autopilot": minor
---

Add a tool to list installed titles. `GET /titles` (and the
`list_installed_titles` MCP tool) enumerates the base applications installed on
the console across SD, internal storage, and an inserted gamecard, returning
each title's id, content-meta version, storage, and display name (resolved from
its control data / NACP). Read-only — uses the `ncm`/`ns` access the installer
already declares, no new NPDM services.
