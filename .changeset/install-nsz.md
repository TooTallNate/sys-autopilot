---
"sys-autopilot": minor
---

Add compressed **NSZ** install support via a host-side helper. The console only
understands plain NSP, so `scripts/install-nsz.mjs` decompresses + re-encrypts
each `.ncz` into a plain NCA on the host and streams a reconstructed NSP
straight to the existing `/install` endpoint — nothing is buffered to disk, and
the console still verifies every NCA's SHA-256 as it lands.

```sh
pnpm install
node scripts/install-nsz.mjs "Game [0100...000][v0].nsz" http://<ip>:4150 --netrc
```

- Uses `@tootallnate/ncz` (zstd decompression + AES-CTR re-encryption from the
  per-section keys embedded in the NCZ — no external keys required),
  `@tootallnate/pfs0`, and `@tootallnate/zstd-wasm`.
- Computes the reconstructed NSP's `Content-Length` up front from the NCZ
  headers (no full decompress) so the stream can be sent without staging.
- Options: `--storage sd|nand`, `--user`/`--pass` or `--netrc` for HTTP Basic
  auth, and `--dry-run` to decompress + validate the output size locally
  without uploading.
- No sysmodule changes — the on-device installer is unchanged.
