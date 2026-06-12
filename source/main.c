#include <stdlib.h>
#include <string.h>
#include <switch.h>

#include "common/config.h"
#include "common/input.h"
#include "common/oauth.h"
#include "common/power.h"
#include "common/server.h"

// Inner heap: socket transfer memory + stdio buffers + dir listing JSON.
// The large fixed buffers (JPEG, I/O, HDLS workmem) are static bss.
#define INNER_HEAP_SIZE 0x100000

#ifdef __cplusplus
extern "C" {
#endif

u32 __nx_applet_type = AppletType_None;
u32 __nx_fs_num_sessions = 1;

// Sysmodules must use time:s (the npdm grants it); used for the
// "# issued <date>" stamps in the OAuth tokens file.
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
    .num_bsd_sessions    = 2,
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

    smExit();
}

void __appExit(void)
{
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
