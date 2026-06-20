#include <stdlib.h>
#include <string.h>
#include <switch.h>

#include "common/config.h"
#include "common/device_info.h"
#include "common/input.h"
#include "common/install.h"
#include "common/log.h"
#include "common/netif.h"
#include "common/oauth.h"
#include "common/power.h"
#include "common/server.h"
#include "common/settings.h"

// Inner heap: socket transfer memory + stdio buffers + dir listing JSON +
// headroom for the title installer (ncm IPC, mounting the cnmt NCA). The large
// fixed buffers (JPEG, I/O, HDLS workmem, install chunk) are static bss.
#define INNER_HEAP_SIZE 0x400000

#ifdef __cplusplus
extern "C" {
#endif

u32 __nx_applet_type = AppletType_None;
u32 __nx_fs_num_sessions = 1;

// Sysmodules use time:s (the npdm grants it); used for the OAuth
// "# issued <date>" stamps and the read-only get_datetime tool.
u32 __nx_time_service_type = TimeServiceType_System;

// Internal libnx helper that wires newlib's time() to the time service.
void __libnx_init_time(void);

void __libnx_initheap(void)
{
    static u8 inner_heap[INNER_HEAP_SIZE];
    extern void* fake_heap_start;
    extern void* fake_heap_end;

    fake_heap_start = inner_heap;
    fake_heap_end   = inner_heap + sizeof(inner_heap);
}

// Keep socket buffers small to fit the sysmodule memory budget.
static const SocketInitConfig kSocketConfig = {
    .tcp_tx_buf_size     = 0x8000,
    .tcp_rx_buf_size     = 0x10000,
    .tcp_tx_buf_max_size = 0,       // fixed size
    .tcp_rx_buf_max_size = 0,       // fixed size
    .udp_tx_buf_size     = 0x2400,
    .udp_rx_buf_size     = 0xA500,
    .sb_efficiency       = 2,
    // Sessions in concurrent use: the TCP listener, one accepted client, and
    // the persistent mDNS/DNS-SD UDP socket. 3 is the minimum; use 4 for a
    // little headroom.
    .num_bsd_sessions    = 4,
    .bsd_service_type    = BsdServiceType_User,
};

void __appInit(void)
{
    Result rc;

    rc = smInitialize();
    if (R_FAILED(rc))
        diagAbortWithResult(MAKERESULT(Module_Libnx, LibnxError_InitFail_SM));

    rc = setsysInitialize();
    if (R_SUCCEEDED(rc)) {
        SetSysFirmwareVersion fw;
        rc = setsysGetFirmwareVersion(&fw);
        if (R_SUCCEEDED(rc))
            hosversionSet(MAKEHOSVERSION(fw.major, fw.minor, fw.micro));
        setsysExit();
    }

    rc = fsInitialize();
    if (R_FAILED(rc))
        diagAbortWithResult(MAKERESULT(Module_Libnx, LibnxError_InitFail_FS));

    rc = fsdevMountSdmc();
    if (R_FAILED(rc))
        diagAbortWithResult(rc);

    rc = hiddbgInitialize();
    if (R_FAILED(rc))
        diagAbortWithResult(rc);

    rc = capsscInitialize();
    if (R_FAILED(rc))
        diagAbortWithResult(rc);

    // Optional: wall-clock time for OAuth token issuance stamps.
    rc = timeInitialize();
    if (R_SUCCEEDED(rc))
        __libnx_init_time();

    rc = socketInitialize(&kSocketConfig);
    if (R_FAILED(rc))
        diagAbortWithResult(rc);

    // Register with PSC so we can quiesce sockets across sleep (must happen
    // while the sm session is open). Failure is non-fatal but means sleep
    // will crash the console, so abort loudly in that case.
    if (!power_init())
        diagAbortWithResult(MAKERESULT(Module_Libnx, LibnxError_NotFound));

    // For agent-requested sleep/restart/power-off. Non-fatal when missing:
    // the endpoints report unavailability.
    power_spsm_init();

    // Gather device facts (model/firmware/Atmosphère) for the mDNS TXT record
    // now, while the sm session is still open: the underlying set:sys/spl
    // smGetService calls would fail after smExit(). Best-effort; failures just
    // leave the corresponding TXT fields empty.
    device_info_init();

    // Network Interface Manager: nifm gives us the real LAN IP for mDNS
    // (gethostid() only ever returns loopback in a sysmodule). Must be opened
    // while sm is up; the session is held for the process lifetime.
    if (!netif_init())
        LOGF("netif: nifm init failed; mDNS A records unavailable\n");

    // System-settings services (lbl/audctl/psm) for the settings tools. Opened
    // while sm is up; best-effort (a missing service just disables its tool).
    settings_init();

    // Title installation services (ncm/ns/es). Opened while sm is up; if these
    // fail the /install endpoint reports unavailability.
    if (!install_init())
        LOGF("install: services unavailable; /install disabled\n");

    smExit();
}

void __appExit(void)
{
    install_exit();
    settings_exit();
    netif_exit();
    power_spsm_exit();
    power_exit();
    timeExit();
    socketExit();
    capsscExit();
    hiddbgExit();
    fsdevUnmountAll();
    fsExit();
}

#ifdef __cplusplus
}
#endif

int main(int argc, char* argv[])
{
    Config cfg;
    config_load(&cfg);

    // OAuth state (config reference + persisted token list).
    oauth_init(&cfg);

    // Blocks forever (NULL idle callback).
    server_run(&cfg, NULL);

    input_exit();
    return 0;
}
