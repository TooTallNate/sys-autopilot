#include "network.h"

#ifdef __SWITCH__
#include <switch.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "log.h"

// Parse a dotted-quad IPv4 string into a NifmIpV4Address (a.b.c.d -> addr[0..3]).
// Returns false on malformed input. Avoids inet_pton so there's no dependency
// on the socket layer being initialized.
static bool str_to_ip(const char *s, NifmIpV4Address *out) {
    if (!s || !s[0]) return false;
    unsigned vals[4];
    int n = 0;
    const char *p = s;
    for (; n < 4; n++) {
        if (*p < '0' || *p > '9') return false;
        unsigned v = 0;
        int digits = 0;
        while (*p >= '0' && *p <= '9') {
            v = v * 10 + (unsigned)(*p - '0');
            if (++digits > 3 || v > 255) return false;
            p++;
        }
        vals[n] = v;
        if (n < 3) { if (*p != '.') return false; p++; }
    }
    if (*p != '\0') return false; // trailing junk
    for (int i = 0; i < 4; i++) out->addr[i] = (u8)vals[i];
    return true;
}

bool network_get_dns(DnsConfig *out, char *err, size_t errsz) {
    memset(out, 0, sizeof(*out));
    // nifmGetCurrentIpConfigInfo works with nifm:u (already initialized at boot
    // for mDNS). It reports the *effective* DNS regardless of auto/manual.
    u32 addr = 0, mask = 0, gw = 0, dns1 = 0, dns2 = 0;
    Result rc = nifmGetCurrentIpConfigInfo(&addr, &mask, &gw, &dns1, &dns2);
    if (R_FAILED(rc)) {
        snprintf(err, errsz, "no active connection (0x%x)", rc);
        return false;
    }
    // dns1/dns2 are network byte order (a.b.c.d in memory order).
    const u8 *b1 = (const u8 *)&dns1, *b2 = (const u8 *)&dns2;
    snprintf(out->primary, sizeof(out->primary), "%u.%u.%u.%u",
             b1[0], b1[1], b1[2], b1[3]);
    snprintf(out->secondary, sizeof(out->secondary), "%u.%u.%u.%u",
             b2[0], b2[1], b2[2], b2[3]);

    // Whether DNS is manual comes from the saved profile.
    NifmNetworkProfileData prof;
    if (R_SUCCEEDED(nifmGetCurrentNetworkProfile(&prof)))
        out->is_automatic = prof.ip_setting_data.dns_setting.is_automatic != 0;
    else
        out->is_automatic = true;
    return true;
}

bool network_set_dns(bool automatic, const char *primary, const char *secondary,
                     char *err, size_t errsz) {
    NifmIpV4Address p = {0}, s = {0};
    if (!automatic) {
        if (!str_to_ip(primary, &p)) {
            snprintf(err, errsz, "invalid primary DNS"); return false;
        }
        if (secondary && secondary[0] && !str_to_ip(secondary, &s)) {
            snprintf(err, errsz, "invalid secondary DNS"); return false;
        }
    }

    // Setting a profile requires an nifm:a (admin) session. nifm is opened as
    // Admin once at boot (netif_init); libnx's refcounted nifmInitialize ignores
    // the service type on later calls, so we must NOT re-init/exit here (that
    // would either no-op over a wrong session or close the boot session). We
    // rely on the existing admin session.
    NifmNetworkProfileData prof;
    Result rc = nifmGetCurrentNetworkProfile(&prof);
    if (R_FAILED(rc)) {
        snprintf(err, errsz, "no active profile to modify (0x%x)", rc);
        return false;
    }

    prof.ip_setting_data.dns_setting.is_automatic = automatic ? 1 : 0;
    if (!automatic) {
        prof.ip_setting_data.dns_setting.primary_dns_server = p;
        prof.ip_setting_data.dns_setting.secondary_dns_server = s;
    }

    Uuid uuid = prof.uuid;
    rc = nifmSetNetworkProfile(&prof, &uuid);
    if (R_FAILED(rc)) {
        snprintf(err, errsz, "set profile failed (0x%x)", rc);
        return false;
    }
    LOGF("network: DNS set to %s (%s/%s)\n",
         automatic ? "automatic" : "manual",
         automatic ? "-" : primary,
         automatic ? "-" : (secondary && secondary[0] ? secondary : "-"));
    return true;
}

#else // !__SWITCH__ : host stubs so the REST/MCP layer links in tests.

#include <string.h>
#include <stdio.h>

bool network_get_dns(DnsConfig *out, char *err, size_t errsz) {
    (void)out;
    snprintf(err, errsz, "network config unavailable on host");
    return false;
}

bool network_set_dns(bool automatic, const char *primary, const char *secondary,
                     char *err, size_t errsz) {
    (void)automatic; (void)primary; (void)secondary;
    snprintf(err, errsz, "network config unavailable on host");
    return false;
}

#endif // __SWITCH__
