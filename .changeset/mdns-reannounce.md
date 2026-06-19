---
"sys-autopilot": patch
---

Re-advertise mDNS when the network changes (wifi reconnect, airplane-mode
toggle, DHCP renewal) so the console's `<hostname>.local` name keeps resolving
without a reboot. Previously the advertisement only happened at startup, so it
went stale after connectivity changes.

- Detects connectivity changes by polling nifm for the current IP every ~2s
  and acting when it changes. (We tested nifm's connectivity-change event on
  hardware; on an unsubmitted request it never fires, so polling is the
  reliable trigger.)
- On a detected IP change, the HTTP listener and mDNS socket are both rebuilt
  (a network teardown can otherwise leave the listener silently not accepting).
- Unsolicited announcements now retry until a send actually succeeds, instead
  of giving up while routing is still coming up after the network returns.

Sleep-safety: the IP poll runs only while awake, so no nifm IPC hits the PSC
sleep window. (The one confirmed sleep/wake hazard is filesystem I/O during
the transition, fixed separately by suspending the log sink.)
