---
"sys-autopilot": patch
---

Fix System Memory Pool 2 starvation (am crash 2001-0131) and FW 22.1.0 stability.
Slim inner heap from 4 MB to 512 KB and install chunk from 1 MB to 64 KB, reducing total ELF size from 6.69 MB down to 2.11 MB (66% reduction).
Stub deprecated nsGetApplicationControlData and bypass synchronous audctlInitialize on modern Horizon OS firmware.
