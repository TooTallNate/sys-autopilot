---
"sys-autopilot": minor
---

Add game save data backup and restore. `GET /saves` (and the `list_saves` MCP
tool) lists the saves in the NAND user save space; `GET /saves/files` browses
one, `GET /saves/export` streams it as a tar archive, and `GET /saves/raw`
streams the raw on-NAND save container. `PUT /saves/import` replaces an existing save's contents
with an exported tar, committed atomically when it fits the save's journal.
System saves are never listed or modified. No NPDM changes.
