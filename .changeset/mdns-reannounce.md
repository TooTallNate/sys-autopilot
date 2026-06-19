---
"sys-autopilot": patch
---

Re-advertise mDNS when the network changes (wifi reconnect, airplane-mode
toggle, DHCP renewal) so the console's `<hostname>.local` name keeps resolving
without a reboot. Previously the advertisement only happened at startup, so it
went stale after connectivity changes.

- Subscribes to nifm's connectivity-change event as a fast trigger, backed by
  a periodic (~2s) IP-change check so changes are caught even if the event
  fires before the address settles or is missed.
- On a detected IP change, the HTTP listener and mDNS socket are both rebuilt
  (a network teardown can otherwise leave the listener silently not accepting).
- Unsolicited announcements now retry until a send actually succeeds, instead
  of giving up while routing is still coming up after the network returns.

Sleep-safety: the nifm request is created only to obtain its event and is not
submitted (we don't need active network demand), and no nifm IPC is issued
during the sleep transition. The only confirmed sleep/wake hazard is
filesystem I/O during the transition (fixed separately by suspending the log
sink); the IP-change poll runs only while awake.
