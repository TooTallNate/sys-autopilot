---
"sys-autopilot": patch
---

Fix console crash on sleep (hard restart on wake). Two changes were
required: the HDLS work buffer (transfer memory mapped into the hid
sysmodule) is now attached lazily and released when the console
prepares to sleep, and the sysmodule registers as a PSC power
management module so it can close its listener socket and quiesce all
bsd IPC before acknowledging the sleep transition. Previously the held
HDLS state and live socket activity made the sleep sequence fail (omm
abort with psc error 2165-1001, occasional bsdsockets aborts), forcing
a full reboot on wake. The virtual controller re-attaches automatically
on the first input request after wake.
