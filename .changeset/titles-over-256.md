---
"sys-autopilot": patch
---

`GET /titles` and the `list_installed_titles` MCP tool no longer stop at 256
titles. The content-meta listing used a fixed 256-entry buffer, so consoles
with more installed games silently got a truncated list. `GET /titles` now
returns every installed title by default and adds `total`/`offset` to the
response, plus optional `?offset=`/`&limit=` query parameters for fetching a
window. `list_installed_titles` takes `offset`/`limit` (default 256) and says
which offset to request next when more titles remain. Long display names are
now cut on a UTF-8 character boundary instead of mid-character, which could
produce invalid UTF-8 in the JSON responses.
