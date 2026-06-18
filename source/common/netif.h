#pragma once

#include <stdbool.h>
#include <stdint.h>

// Network interface helper around nifm (the Network Interface Manager).
//
// gethostid() in a sysmodule returns loopback (127.0.0.1) and never the real
// LAN address, so mDNS must learn the console's IP from nifm instead.

// Initializes nifm. MUST be called from __appInit while the sm session is open
// (nifmInitialize goes through smGetService, which fails after smExit). The
// session is held for the process lifetime so the IP can be polled later.
// No-op / returns false on host builds. Returns true on success.
bool netif_init(void);

// Tears down nifm (call from __appExit). No-op on host builds.
void netif_exit(void);

// Writes the current IPv4 into *out_ipv4_be in the same byte layout as
// struct in_addr.s_addr (network byte order). Returns true when a usable
// (non-zero, non-loopback) address is available. On host builds returns a
// fixed placeholder so callers can be exercised deterministically.
bool netif_current_ipv4(uint32_t *out_s_addr);
