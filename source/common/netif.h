#pragma once

#include <stdbool.h>
#include <stdint.h>

// Network interface helper around nifm (the Network Interface Manager).
//
// gethostid() in a sysmodule returns loopback (127.0.0.1) and never the real
// LAN address, so mDNS must learn the console's IP from nifm instead.
//
// Connectivity changes (wifi connect/disconnect, airplane mode, DHCP renewal)
// are detected by polling the current IP for changes. We tried nifm's
// connectivity-change event, but on an unsubmitted request it never fires on
// hardware, and submitting a request to get the event would express active
// network demand we don't want; polling is the reliable mechanism.
//
// SLEEP SAFETY: the one confirmed sleep/wake hazard is doing filesystem I/O
// during the PSC transition (the log sink, handled separately). As a
// precaution we also keep nifm IPC (nifmGetCurrentIpAddress) out of the sleep
// window by only checking connectivity while awake.

// Initializes nifm. MUST be called from __appInit while the sm session is open
// (nifmInitialize goes through smGetService, which fails after smExit). No-op
// / returns false on host builds.
bool netif_init(void);

// Tears down nifm (call from __appExit). No-op on host builds.
void netif_exit(void);

// Returns true if the current IPv4 differs from the value seen on the previous
// call (including transitions to/from "no address"), updating the cached
// value. The first call establishes the baseline and returns false. This
// performs a nifm IPC (do not call during the sleep window) and is how
// connectivity changes are detected. Always false on host builds.
bool netif_ipv4_changed(void);

// Writes the current IPv4 into *out_s_addr in the same byte layout as
// struct in_addr.s_addr (network byte order). Returns true when a usable
// (non-zero, non-loopback) address is available. This performs a nifm IPC, so
// it must NOT be called during the sleep transition. On host builds returns a
// fixed placeholder so callers can be exercised deterministically.
bool netif_current_ipv4(uint32_t *out_s_addr);
