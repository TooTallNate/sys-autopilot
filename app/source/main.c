#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <switch.h>

#include "common/config.h"
#include "common/device_info.h"
#include "common/input.h"
#include "common/netif.h"
#include "common/oauth.h"
#include "common/power.h"
#include "common/server.h"
#include "common/settings.h"

static PadState g_pad;

// Called from the server loop ~every 100ms and between requests.
static bool app_idle(void)
{
    if (!appletMainLoop())
        return false;

    padUpdate(&g_pad);
    if (padGetButtonsDown(&g_pad) & HidNpadButton_Plus)
        return false;

    consoleUpdate(NULL);
    return true;
}

int main(int argc, char* argv[])
{
    consoleInit(NULL);

    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    padInitializeDefault(&g_pad);

    socketInitializeDefault();

    Result rc = hiddbgInitialize();
    if (R_FAILED(rc))
        printf("hiddbgInitialize() failed: 0x%x\n", rc);

    rc = capsscInitialize();
    if (R_FAILED(rc))
        printf("capsscInitialize() failed: 0x%x\n", rc);

    Config cfg;
    config_load(&cfg);

    // Device facts + network interface for mDNS (best-effort under HBL).
    device_info_init();
    netif_init();
    settings_init();

    oauth_init(&cfg);

    // Best-effort: applets are suspended during sleep anyway, but register
    // with PSC when available so the loop quiesces sockets like the
    // sysmodule does.
    if (!power_init())
        printf("power_init() unavailable (running without sleep handling)\n");
    if (!power_spsm_init())
        printf("spsm unavailable (power tools disabled)\n");

    struct in_addr addr = { .s_addr = gethostid() };
    char hostname[64];
    config_hostname(&cfg, device_info_get()->serial, hostname, sizeof(hostname));
    printf("sys-autopilot (dev app)\n");
    printf("Listening on http://%s:%d\n", inet_ntoa(addr), cfg.port);
    printf("Discoverable at http://%s.local:%d\n", hostname, cfg.port);
    printf("Press + to exit.\n\n");
    consoleUpdate(NULL);

    server_run(&cfg, app_idle);

    settings_exit();
    input_exit();
    capsscExit();
    hiddbgExit();
    socketExit();
    consoleExit(NULL);
    return 0;
}
