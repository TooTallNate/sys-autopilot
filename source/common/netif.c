#include "netif.h"

#ifdef __SWITCH__
#include <switch.h>
#include <netinet/in.h>

static bool g_nifm_ready;

bool netif_init(void) {
    if (g_nifm_ready)
        return true;
    Result rc = nifmInitialize(NifmServiceType_User);
    g_nifm_ready = R_SUCCEEDED(rc);
    return g_nifm_ready;
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

#else // host / test build

bool netif_init(void) { return false; }
void netif_exit(void) {}

bool netif_current_ipv4(uint32_t *out_s_addr) {
    // 127.0.0.1 in network byte order, for deterministic host tests.
    *out_s_addr = 0x0100007f;
    return true;
}

#endif
