#include "power.h"
#include "log.h"

// Scheduling is pure (host-testable); execution is Switch-only below.
static PowerAction g_scheduled;

void power_schedule(PowerAction action) {
    g_scheduled = action;
}

PowerAction power_take_scheduled(void) {
    PowerAction a = g_scheduled;
    g_scheduled = PowerAction_None;
    return a;
}

#ifdef __SWITCH__

#include <switch.h>

// Arbitrary unused PM module id ("AP"). Registration fails if it collides
// with another module, in which case we run without sleep handling.
#define POWER_MODULE_ID ((PscPmModuleId)0x4150)

static PscPmModule g_module;
static PscPmState g_pending_state;
static bool g_initialized;

bool power_init(void) {
    Result rc = pscmInitialize();
    if (R_FAILED(rc)) {
        LOGF("power: pscmInitialize failed rc=0x%x\n", rc);
        return false;
    }

    // Declaring Fs as a dependency makes PSC notify us early in the sleep
    // sequence (and late in the wake sequence). This matches working
    // sysmodules in the wild (kdeconnect-nx); referencing modules that may
    // not be registered (e.g. WlanSockets) risks breaking psc's dispatch.
    static const u32 deps[] = { PscPmModuleId_Fs };
    rc = pscmGetPmModule(&g_module, POWER_MODULE_ID, deps,
                         sizeof(deps) / sizeof(deps[0]), true);
    if (R_FAILED(rc)) {
        LOGF("power: pscmGetPmModule failed rc=0x%x\n", rc);
        pscmExit();
        return false;
    }

    g_initialized = true;
    return true;
}

PowerEvent power_poll(void) {
    if (!g_initialized)
        return PowerEvent_None;

    // Non-blocking: timeout 0 returns immediately when no request is pending.
    if (R_FAILED(eventWait(&g_module.event, 0)))
        return PowerEvent_None;

    u32 flags = 0;
    if (R_FAILED(pscPmModuleGetRequest(&g_module, &g_pending_state, &flags)))
        return PowerEvent_None;

    switch (g_pending_state) {
        case PscPmState_ReadySleep:
        case PscPmState_ReadySleepCritical:
        case PscPmState_ReadyShutdown:
            return PowerEvent_Sleep;
        case PscPmState_ReadyAwaken:
        case PscPmState_ReadyAwakenCritical:
        case PscPmState_Awake:
        default:
            return PowerEvent_Wake;
    }
}

void power_ack(void) {
    if (g_initialized)
        pscPmModuleAcknowledge(&g_module, g_pending_state);
}

void power_exit(void) {
    if (!g_initialized)
        return;
    pscPmModuleFinalize(&g_module);
    pscPmModuleClose(&g_module);
    eventClose(&g_module.event);
    pscmExit();
    g_initialized = false;
}

static bool g_spsm_ok;

bool power_spsm_init(void) {
    Result rc = spsmInitialize();
    if (R_FAILED(rc)) {
        LOGF("power: spsmInitialize failed rc=0x%x\n", rc);
        return false;
    }
    g_spsm_ok = true;
    return true;
}

void power_spsm_exit(void) {
    if (g_spsm_ok) {
        spsmExit();
        g_spsm_ok = false;
    }
}

bool power_actions_available(void) {
    return g_spsm_ok;
}

bool power_perform(PowerAction action) {
    if (!g_spsm_ok)
        return false;
    Result rc;
    switch (action) {
        case PowerAction_Sleep: {
            // spsm cmd 1 SleepSystemAndWaitAwake: returns an event handle
            // immediately and kicks off the sleep sequence; PSC then walks
            // us through the transition like any other sleep.
            Handle h = INVALID_HANDLE;
            rc = serviceDispatch(spsmGetServiceSession(), 1,
                                 .out_handle_attrs = { SfOutHandleAttr_HipcCopy },
                                 .out_handles = &h);
            if (R_SUCCEEDED(rc) && h != INVALID_HANDLE)
                svcCloseHandle(h);
            break;
        }
        case PowerAction_Restart:
            rc = spsmShutdown(true);
            break;
        case PowerAction_PowerOff:
            rc = spsmShutdown(false);
            break;
        default:
            return false;
    }
    if (R_FAILED(rc))
        LOGF("power: action %d failed rc=0x%x\n", action, rc);
    return R_SUCCEEDED(rc);
}

#else // host (tests): no PSC / spsm

static PowerAction g_last_performed;

bool power_init(void) { return false; }
PowerEvent power_poll(void) { return PowerEvent_None; }
void power_ack(void) {}
void power_exit(void) {}

bool power_spsm_init(void) { return false; }
void power_spsm_exit(void) {}
bool power_actions_available(void) { return true; } // tools testable on host

bool power_perform(PowerAction action) {
    g_last_performed = action;
    return true;
}

#endif
