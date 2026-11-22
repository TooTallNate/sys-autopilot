---
"sys-autopilot": minor
---

Report system memory in `GET /status` (and the `status` MCP tool) and shrink
the sysmodule's memory footprint.

`status` now includes `systemMemoryTotal` / `systemMemoryUsed` /
`systemMemoryFree` for the System memory pool that all sysmodules share, plus
`heapSize`, `heapInUse` and `heapPeak` for sys-autopilot's own heap. The heap
is filled with a pattern at startup, so `heapPeak` is the most it has ever
used. Reading the pool needs `svcGetSystemInfo`, added to the NPDM.

The inner heap goes from 4 MiB to 1.5 MiB (measured peak ~420 KiB across a
43 MB save export, a 4 MiB upload, large directory listings and title/save
listings) and the static install chunk from 1 MiB to 512 KiB. That frees about
3 MiB of the System pool. On HOS 22.5.0 with a few other sysmodules installed,
the pool ran out and system services (`am`, `wlan`) aborted with 2001-0132
(`0x10801`, resource limit exceeded).
