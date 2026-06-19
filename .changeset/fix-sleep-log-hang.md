---
"sys-autopilot": patch
---

Fix the console hanging on wake-from-sleep when file logging is enabled
(`log = true`). The log sink writes to the SD card, and a write triggered
during the PSC sleep transition (e.g. the "power: sleeping" line) issued
fsp-srv I/O inside the window between the sleep notification and
acknowledgement, which hangs the wake sequence. File writes are now suspended
for the duration of the sleep/wake transition.
