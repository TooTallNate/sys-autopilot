#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Network configuration (currently DNS) for the active connection profile.
// Setting DNS requires the nifm:a (admin) service; reading the current config
// works with nifm:u. Useful for pointing the console at a custom/black-hole DNS
// (e.g. 90DNS) so a console can stay on the LAN while Nintendo servers are
// blocked — required for offline use of fake-linked accounts.

typedef struct {
    bool is_automatic;   // true = DHCP-provided DNS, false = manual
    char primary[16];    // dotted IPv4, e.g. "207.246.121.77" ("" if unset)
    char secondary[16];  // dotted IPv4 ("" if unset)
} DnsConfig;

// Read the current connection's DNS configuration. Returns true on success.
// (Host build: a stub that reports failure.)
bool network_get_dns(DnsConfig *out, char *err, size_t errsz);

// Set the active profile's DNS. If automatic is true, reverts to DHCP DNS and
// primary/secondary are ignored. Otherwise primary must be a valid dotted IPv4;
// secondary may be NULL/empty. Persists to the saved network profile. Returns
// true on success.
bool network_set_dns(bool automatic, const char *primary, const char *secondary,
                     char *err, size_t errsz);
