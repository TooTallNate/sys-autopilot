---
"sys-autopilot": minor
---

Add a `create_token` MCP tool that mints a bearer token over the
already-authenticated MCP channel. This lets agents use the raw HTTP
API (e.g. `curl -T` for large file uploads, where base64 tool-call
content would be impractical) without being handed credentials out of
band. Tokens share the `tokens.txt` store and revocation model with the
OAuth flow.
