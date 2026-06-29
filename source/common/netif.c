#include "netif.h"

#ifdef __SWITCH__
#include <switch.h>
#include <netinet/in.h>

#include "log.h"

static bool g_nifm_ready;
static bool g_have_last_addr;
static uint32_t g_last_addr; // last sampled s_addr (0 = none/loopback)

bool netif_init(void) {
    if (g_nifm_ready)
        return true;
    // Open nifm as Admin (a superset of User). libnx's nifmInitialize is
    // refcounted and ignores the service type on subsequent calls, so the FIRST
    // init in the process decides the session type. Using Admin here means
    // later admin-only calls (e.g. nifmSetNetworkProfile for DNS config) work;
    // the read-only calls this module makes are fine over an admin session.
    Result rc = nifmInitialize(NifmServiceType_Admin);
    if (R_FAILED(rc)) {
        // Fall back to User if Admin is somehow unavailable (read-only still ok).
        LOGF("netif: nifm:a init failed rc=0x%x, trying nifm:u\n", rc);
        rc = nifmInitialize(NifmServiceType_User);
        if (R_FAILED(rc)) {
            LOGF("netif: nifmInitialize failed rc=0x%x\n", rc);
            return false;
        }
    }
    g_nifm_ready = true;
    return true;
}

void netif_exit(void) {
    if (g_nifm_ready) {
        nifmExit();
        g_nifm_ready = false;
    }
}

bool netif_current_ipv4(uint32_t *out_s_addr) {
    if (!g_nifm_ready)
        return false;
    u32 addr = 0; // struct in_addr.s_addr (network byte order)
    if (R_FAILED(nifmGetCurrentIpAddress(&addr)) || addr == 0)
        return false;
    // Reject loopback (first octet 127) regardless of host endianness.
    const uint8_t *o = (const uint8_t *)&addr;
    if (o[0] == 127 || o[3] == 127)
        return false;
    *out_s_addr = addr;
    return true;
}

bool netif_ipv4_changed(void) {
    if (!g_nifm_ready)
        return false;
    uint32_t cur = 0; // 0 means no usable address right now
    netif_current_ipv4(&cur);
    if (!g_have_last_addr) {
        g_have_last_addr = true;
        g_last_addr = cur;
        return false; // baseline
    }
    if (cur != g_last_addr) {
        g_last_addr = cur;
        return true;
    }
    return false;
}

#else // host / test build

bool netif_init(void) { return false; }
void netif_exit(void) {}
bool netif_ipv4_changed(void) { return false; }

bool netif_current_ipv4(uint32_t *out_s_addr) {
    // 127.0.0.1 in network byte order, for deterministic host tests.
    *out_s_addr = 0x0100007f;
    return true;
}

#endif
