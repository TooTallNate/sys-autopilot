#include "netif.h"

#ifdef __SWITCH__
#include <switch.h>
#include <netinet/in.h>

#include "log.h"

static bool g_nifm_ready;
static NifmRequest g_request;
static bool g_request_ready;
static bool g_have_last_addr;
static uint32_t g_last_addr; // last sampled s_addr (0 = none/loopback)

bool netif_init(void) {
    if (g_nifm_ready)
        return true;
    Result rc = nifmInitialize(NifmServiceType_User);
    if (R_FAILED(rc)) {
        LOGF("netif: nifmInitialize failed rc=0x%x\n", rc);
        return false;
    }
    g_nifm_ready = true;

    // Create a request purely to obtain its connectivity-change event. We do
    // NOT submit it: a submitted request holds the network connection up and
    // hangs the PSC wake sequence. Creating it only gets the event handles.
    rc = nifmCreateRequest(&g_request, true /* autoclear */);
    if (R_SUCCEEDED(rc)) {
        g_request_ready = true;
    } else {
        LOGF("netif: nifmCreateRequest failed rc=0x%x\n", rc);
    }
    return true;
}

void netif_exit(void) {
    if (g_request_ready) {
        nifmRequestClose(&g_request);
        g_request_ready = false;
    }
    if (g_nifm_ready) {
        nifmExit();
        g_nifm_ready = false;
    }
}

bool netif_connectivity_changed(void) {
    if (!g_request_ready)
        return false;
    // Local kernel wait (timeout 0), not a nifm IPC. The event autoclears, so
    // a successful wait both reports and consumes the change notification.
    return R_SUCCEEDED(eventWait(&g_request.event_request_state, 0));
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
bool netif_connectivity_changed(void) { return false; }
bool netif_ipv4_changed(void) { return false; }

bool netif_current_ipv4(uint32_t *out_s_addr) {
    // 127.0.0.1 in network byte order, for deterministic host tests.
    *out_s_addr = 0x0100007f;
    return true;
}

#endif
