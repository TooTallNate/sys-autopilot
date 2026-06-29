---
"sys-autopilot": patch
---

Support HTTP/1.1 keep-alive so MCP clients can reuse one connection across
initialize → notifications/initialized → tools/list. Responses previously
forced `Connection: close`, which made the Streamable HTTP transport's reused
socket get reset ("socket connection closed unexpectedly"). The server now
keeps the connection alive when the client speaks HTTP/1.1, drains the request
body between requests, and half-closes cleanly to avoid RSTs. `GET /mcp`
returns 405 (no server-initiated SSE stream offered), the spec-sanctioned way
to decline the optional channel.
