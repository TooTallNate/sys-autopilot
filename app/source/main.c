#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <switch.h>

#include "common/config.h"
#include "common/input.h"
#include "common/oauth.h"
#include "common/server.h"

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

    rc = input_init();
    if (R_FAILED(rc))
        printf("input_init() failed: 0x%x\n", rc);

    oauth_init(&cfg);

    struct in_addr addr = { .s_addr = gethostid() };
    printf("sys-autopilot (dev app)\n");
    printf("Listening on http://%s:%d\n", inet_ntoa(addr), cfg.port);
    printf("Press + to exit.\n\n");
    consoleUpdate(NULL);

    server_run(&cfg, app_idle);

    input_exit();
    capsscExit();
    hiddbgExit();
    socketExit();
    consoleExit(NULL);
    return 0;
}
