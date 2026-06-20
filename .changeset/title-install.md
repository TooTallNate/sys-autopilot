---
"sys-autopilot": minor
---

Add network title installation. Stream an NSP straight to the console with a
single `curl -T` and it is installed into NCM content storage and registered
so it appears on the HOME menu — no SD-card staging, nothing to clean up.

```sh
curl -g -T "Game [0100...000][v0].nsp" http://<ip>:4150/install
```

- New `POST`/`PUT /install[?storage=sd|nand]` endpoint. The NSP is streamed
  (sized from `Content-Length`), so multi-GB titles install without buffering
  to disk.
- Parses the PFS0/NSP, writes each NCA via the NCM placeholder/register flow,
  verifies each NCA's SHA-256 against its content id, parses the CNMT, sets the
  content-meta database entry, imports the ticket (`es`), and pushes the
  application record (`ns`). Rolls back written content on failure.
- No external keys required: NCAs are copied byte-for-byte, and the CNMT is read
  from the registered meta NCA via the firmware's own decryption.
- Adds the `ncm`, `ns:am2`, and `es` services to the NPDM and vendors small
  ns/es IPC wrappers that libnx does not expose.

NSP only for now; compressed NSZ is planned as a follow-up.
