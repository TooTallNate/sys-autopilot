---
"sys-autopilot": minor
---

Add an OAuth 2.1 browser login flow for MCP clients. When username/password
are configured, clients like Claude Code now authenticate with zero manual
header configuration: the first 401 triggers metadata discovery (RFC 9728 /
RFC 8414), dynamic client registration (RFC 7591), and a browser login page
served by the Switch; after signing in, the client receives a non-expiring
bearer token via the authorization-code + PKCE S256 grant. Tokens are
persisted to `config/sys-autopilot/tokens.txt` (revoke by deleting a line).
Also adds CORS support for browser-based MCP clients.
