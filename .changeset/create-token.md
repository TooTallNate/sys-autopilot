---
"sys-autopilot": minor
---

Add `create_token` and `revoke_token` MCP tools. `create_token` mints a
bearer token over the already-authenticated MCP channel; `revoke_token`
removes a previously issued token again (agents can clean up after
themselves). This lets agents use the raw HTTP
API (e.g. `curl -T` for large file uploads, where base64 tool-call
content would be impractical) without being handed credentials out of
band. Tokens share the `tokens.txt` store and revocation model with the
OAuth flow.
