#include "input.h"
#include "buttons.h"
#include "log.h"

#include <assert.h>
#include <string.h>
#include <strings.h>

// buttons.h mirrors the libnx bit values so it stays host-testable; make
// sure they can never drift.
static_assert(BTN_A == HidNpadButton_A, "button mask drift");
static_assert(BTN_B == HidNpadButton_B, "button mask drift");
static_assert(BTN_X == HidNpadButton_X, "button mask drift");
static_assert(BTN_Y == HidNpadButton_Y, "button mask drift");
static_assert(BTN_LSTICK == HidNpadButton_StickL, "button mask drift");
static_assert(BTN_RSTICK == HidNpadButton_StickR, "button mask drift");
static_assert(BTN_L == HidNpadButton_L, "button mask drift");
static_assert(BTN_R == HidNpadButton_R, "button mask drift");
static_assert(BTN_ZL == HidNpadButton_ZL, "button mask drift");
static_assert(BTN_ZR == HidNpadButton_ZR, "button mask drift");
static_assert(BTN_PLUS == HidNpadButton_Plus, "button mask drift");
static_assert(BTN_MINUS == HidNpadButton_Minus, "button mask drift");
static_assert(BTN_LEFT == HidNpadButton_Left, "button mask drift");
static_assert(BTN_UP == HidNpadButton_Up, "button mask drift");
static_assert(BTN_RIGHT == HidNpadButton_Right, "button mask drift");
static_assert(BTN_DOWN == HidNpadButton_Down, "button mask drift");
static_assert(BTN_HOME == HiddbgNpadButton_Home, "button mask drift");
static_assert(BTN_CAPTURE == HiddbgNpadButton_Capture, "button mask drift");

static u8 g_workmem[0x1000] __attribute__((aligned(0x1000)));
static HiddbgHdlsSessionId g_session_id;
static HiddbgHdlsHandle g_handle;
static HiddbgHdlsState g_state;
static bool g_initialized;
static bool g_attached;

Result input_init(void) {
    Result rc = hiddbgAttachHdlsWorkBuffer(&g_session_id, g_workmem, sizeof(g_workmem));
    if (R_FAILED(rc)) {
        LOGF("input: hiddbgAttachHdlsWorkBuffer failed rc=0x%x\n", rc);
        return rc;
    }
    g_initialized = true;

    memset(&g_state, 0, sizeof(g_state));
    g_state.battery_level = 4; // full
    return 0;
}

void input_exit(void) {
    if (!g_initialized)
        return;
    if (g_attached)
        input_detach();
    hiddbgReleaseHdlsWorkBuffer(g_session_id);
    g_initialized = false;
}

Result input_attach(void) {
    if (!g_initialized)
        return MAKERESULT(Module_Libnx, LibnxError_NotInitialized);
    if (g_attached)
        return 0;

    HiddbgHdlsDeviceInfo device = {0};
    device.deviceType = HidDeviceType_FullKey3; // Pro Controller
    device.npadInterfaceType = HidNpadInterfaceType_Bluetooth;
    device.singleColorBody = RGBA8_MAXALPHA(60, 60, 60);
    device.singleColorButtons = RGBA8_MAXALPHA(230, 230, 230);
    device.colorLeftGrip = RGBA8_MAXALPHA(60, 60, 230);
    device.colorRightGrip = RGBA8_MAXALPHA(230, 60, 60);

    Result rc = hiddbgAttachHdlsVirtualDevice(&g_handle, &device);
    if (R_FAILED(rc)) {
        LOGF("input: hiddbgAttachHdlsVirtualDevice failed rc=0x%x\n", rc);
        return rc;
    }
    g_attached = true;

    // Push an initial neutral state so the controller registers.
    memset(&g_state, 0, sizeof(g_state));
    g_state.battery_level = 4;
    return hiddbgSetHdlsState(g_handle, &g_state);
}

Result input_detach(void) {
    if (!g_attached)
        return 0;
    Result rc = hiddbgDetachHdlsVirtualDevice(g_handle);
    if (R_SUCCEEDED(rc))
        g_attached = false;
    return rc;
}

bool input_is_attached(void) {
    return g_attached;
}

static Result apply_state(void) {
    return hiddbgSetHdlsState(g_handle, &g_state);
}

static int clamp_duration(int ms) {
    if (ms <= 0)
        return INPUT_DEFAULT_TAP_MS;
    if (ms > INPUT_MAX_DURATION_MS)
        return INPUT_MAX_DURATION_MS;
    return ms;
}

Result input_tap(u64 mask, int duration_ms) {
    Result rc = input_attach();
    if (R_FAILED(rc))
        return rc;

    duration_ms = clamp_duration(duration_ms);

    g_state.buttons |= mask;
    rc = apply_state();
    if (R_FAILED(rc))
        return rc;

    svcSleepThread((s64)duration_ms * 1000000LL);

    g_state.buttons &= ~mask;
    return apply_state();
}

Result input_hold(u64 mask) {
    Result rc = input_attach();
    if (R_FAILED(rc))
        return rc;
    g_state.buttons |= mask;
    return apply_state();
}

Result input_release(u64 mask) {
    Result rc = input_attach();
    if (R_FAILED(rc))
        return rc;
    g_state.buttons &= ~mask;
    return apply_state();
}

Result input_clear(void) {
    if (!g_attached)
        return 0;
    g_state.buttons = 0;
    memset(&g_state.analog_stick_l, 0, sizeof(g_state.analog_stick_l));
    memset(&g_state.analog_stick_r, 0, sizeof(g_state.analog_stick_r));
    return apply_state();
}

static s32 stick_value(float v) {
    if (v > 1.0f) v = 1.0f;
    if (v < -1.0f) v = -1.0f;
    return (s32)(v * JOYSTICK_MAX);
}

Result input_stick(int side, float x, float y, int duration_ms) {
    Result rc = input_attach();
    if (R_FAILED(rc))
        return rc;

    HidAnalogStickState *stick = (side == 0) ? &g_state.analog_stick_l
                                             : &g_state.analog_stick_r;
    stick->x = stick_value(x);
    stick->y = stick_value(y);
    rc = apply_state();
    if (R_FAILED(rc))
        return rc;

    if (duration_ms > 0) {
        if (duration_ms > INPUT_MAX_DURATION_MS)
            duration_ms = INPUT_MAX_DURATION_MS;
        svcSleepThread((s64)duration_ms * 1000000LL);
        stick->x = 0;
        stick->y = 0;
        rc = apply_state();
    }
    return rc;
}
