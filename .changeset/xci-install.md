---
"sys-autopilot": minor
---

Install **XCI** (gamecard image) titles via the existing `/install` endpoint.
The container format is auto-detected from the stream — `PFS0`/NSP installs as
before, and an XCI is recognized by its `HEAD` card-header magic (both trimmed
and full layouts). For XCI, the installer skips to the root HFS0, locates the
`secure` partition, and streams its NCAs into NCM using the same
placeholder/register → CNMT → application-record flow as NSP. Fully streaming
(sized from `Content-Length`); no extra sysmodule memory is reserved.

Verified on hardware: a retail XCI installs and launches.

Note: gamecard NCAs are not guaranteed to hash to their filename content id, so
the per-NCA SHA-256 check (kept for NSP) is skipped for XCI; the meta NCA is
still verified and the CNMT drives registration.
