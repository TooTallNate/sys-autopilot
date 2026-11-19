---
"sys-autopilot": patch
---

Fix `/install` rejecting valid NSPs with `write meta nca failed (0x1759)`.
`install_stream` sniffs the first 0x1104 bytes to tell NSP from XCI, but the
NSP path only drained as much of that prefix as the PFS0 header needed and
then dropped the rest. Small NSPs have headers of a few hundred bytes, so the
leftover bytes (ticket, cert and the start of the meta NCA) were lost, every
entry was read from the wrong offset, and the meta NCA failed its SHA-256
check. The leftover prefix is now replayed ahead of the stream.
