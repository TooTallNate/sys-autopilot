---
"sys-autopilot": minor
---

Add file hashing for upload verification. A new `hash_file` MCP tool computes a
file's SHA-256 (hardware-accelerated and streamed in 32 KB chunks, so memory use
is constant for any file size) and returns just the 64-char hex digest. Passing
an `expected` digest also reports a `matched` boolean, so an agent can confirm an
upload landed intact in a single round-trip. The same digest is available over
the raw HTTP API via `GET /files/hash?path=...`.
