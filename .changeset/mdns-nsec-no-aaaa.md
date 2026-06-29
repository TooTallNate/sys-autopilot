---
"sys-autopilot": patch
---

Fix ~5s latency on every request when reaching the console by its `.local`
hostname. The mDNS responder answered A queries but stayed silent for AAAA, so
dual-stack resolvers (macOS `getaddrinfo`, used by curl/ping and MCP clients)
blocked ~5s waiting on a nonexistent AAAA before falling back to IPv4. The A
record and the announcement now carry an NSEC record asserting "A only, no
AAAA" per RFC 6762 §6.1, so resolvers proceed immediately.
