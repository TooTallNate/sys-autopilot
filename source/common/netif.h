#pragma once

#include <stdbool.h>
#include <stdint.h>

// Network interface helper around nifm (the Network Interface Manager).
//
// gethostid() in a sysmodule returns loopback (127.0.0.1) and never the real
// LAN address, so mDNS must learn the console's IP from nifm instead.
//
// We also subscribe to nifm's connectivity-change event so mDNS can be
// re-announced exactly when the network changes (wifi connect/disconnect,
// airplane mode, DHCP renewal) rather than guessing from sleep/wake.
//
// We create the request only to obtain its event handle and do not submit it:
// submitting expresses active network demand (keeps the connection up), which
// we don't need for passive change notification. Polling the event is a local
// kernel wait, not a nifm IPC.
//
// SLEEP SAFETY: the one confirmed sleep/wake hazard is doing filesystem I/O
// during the PSC transition (the log sink, handled separately). As a
// precaution we also keep nifm IPC (nifmGetCurrentIpAddress) out of the sleep
// window by only checking connectivity while awake.

// Initializes nifm and creates (without submitting) a request whose system
// event signals on connectivity changes. MUST be called from __appInit while
// the sm session is open (nifmInitialize goes through smGetService, which
// fails after smExit). No-op / returns false on host builds.
bool netif_init(void);

// Tears down nifm (call from __appExit). No-op on host builds.
void netif_exit(void);

// Non-blocking check of the connectivity-change event. Returns true if it
// signaled since the last call (autoclearing it). This is a local kernel wait,
// not a nifm IPC, so it is safe to call frequently. Always false on host.
bool netif_connectivity_changed(void);

// Returns true if the current IPv4 differs from the value seen on the previous
// call (including transitions to/from "no address"), updating the cached
// value. The first call establishes the baseline and returns false. This
// performs a nifm IPC (do not call during the sleep window) and is the
// reliable backstop for detecting connectivity changes when the event is
// missed or fires before the address has settled. Always false on host.
bool netif_ipv4_changed(void);

// Writes the current IPv4 into *out_s_addr in the same byte layout as
// struct in_addr.s_addr (network byte order). Returns true when a usable
// (non-zero, non-loopback) address is available. This performs a nifm IPC, so
// it must NOT be called during the sleep transition. On host builds returns a
// fixed placeholder so callers can be exercised deterministically.
bool netif_current_ipv4(uint32_t *out_s_addr);
