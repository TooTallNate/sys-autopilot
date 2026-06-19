#include "settings.h"

#include <stdio.h>
#include <string.h>

#ifdef __SWITCH__
#include <switch.h>

#include "log.h"

// Volume is read/written against the speaker target; this is the value the
// system Quick Settings slider controls for the active output.
#define VOLUME_TARGET AudioTarget_Speaker

static bool g_lbl_ok;
static bool g_audctl_ok;
static bool g_psm_ok;

void settings_init(void) {
    g_lbl_ok    = R_SUCCEEDED(lblInitialize());
    g_audctl_ok = R_SUCCEEDED(audctlInitialize());
    g_psm_ok    = R_SUCCEEDED(psmInitialize());
    if (!g_lbl_ok)    LOGF("settings: lbl init failed\n");
    if (!g_audctl_ok) LOGF("settings: audctl init failed\n");
    if (!g_psm_ok)    LOGF("settings: psm init failed\n");
}

void settings_exit(void) {
    if (g_lbl_ok)    { lblExit();    g_lbl_ok = false; }
    if (g_audctl_ok) { audctlExit(); g_audctl_ok = false; }
    if (g_psm_ok)    { psmExit();    g_psm_ok = false; }
}

// --- theme --------------------------------------------------------------------

bool settings_get_theme(bool *out_dark) {
    if (R_FAILED(setsysInitialize()))
        return false;
    ColorSetId id = ColorSetId_Light;
    bool ok = R_SUCCEEDED(setsysGetColorSetId(&id));
    setsysExit();
    if (ok)
        *out_dark = (id == ColorSetId_Dark);
    return ok;
}

bool settings_set_theme(bool dark) {
    if (R_FAILED(setsysInitialize()))
        return false;
    bool ok = R_SUCCEEDED(
        setsysSetColorSetId(dark ? ColorSetId_Dark : ColorSetId_Light));
    setsysExit();
    return ok;
}

// --- nickname -----------------------------------------------------------------

bool settings_get_nickname(char *out, size_t outsz) {
    if (R_FAILED(setsysInitialize()))
        return false;
    SetSysDeviceNickName nn = {0};
    bool ok = R_SUCCEEDED(setsysGetDeviceNickname(&nn));
    setsysExit();
    if (ok)
        snprintf(out, outsz, "%s", nn.nickname);
    return ok;
}

bool settings_set_nickname(const char *name) {
    if (R_FAILED(setsysInitialize()))
        return false;
    SetSysDeviceNickName nn = {0};
    snprintf(nn.nickname, sizeof(nn.nickname), "%s", name);
    bool ok = R_SUCCEEDED(setsysSetDeviceNickname(&nn));
    setsysExit();
    return ok;
}

// --- brightness ---------------------------------------------------------------

bool settings_get_brightness(float *out) {
    if (!g_lbl_ok)
        return false;
    return R_SUCCEEDED(lblGetCurrentBrightnessSetting(out));
}

bool settings_set_brightness(float value) {
    if (!g_lbl_ok)
        return false;
    if (value < 0.0f) value = 0.0f;
    if (value > 1.0f) value = 1.0f;
    if (R_FAILED(lblSetCurrentBrightnessSetting(value)))
        return false;
    // Apply immediately so the change is visible without a settings round-trip.
    lblApplyCurrentBrightnessSettingToBacklight();
    return true;
}

// --- volume -------------------------------------------------------------------

bool settings_get_volume(float *out) {
    if (!g_audctl_ok)
        return false;
    s32 vol = 0, vmin = 0, vmax = 0;
    if (R_FAILED(audctlGetTargetVolume(&vol, VOLUME_TARGET)) ||
        R_FAILED(audctlGetTargetVolumeMin(&vmin)) ||
        R_FAILED(audctlGetTargetVolumeMax(&vmax)))
        return false;
    if (vmax <= vmin) {
        *out = 0.0f;
        return true;
    }
    *out = (float)(vol - vmin) / (float)(vmax - vmin);
    return true;
}

bool settings_set_volume(float value) {
    if (!g_audctl_ok)
        return false;
    if (value < 0.0f) value = 0.0f;
    if (value > 1.0f) value = 1.0f;
    s32 vmin = 0, vmax = 0;
    if (R_FAILED(audctlGetTargetVolumeMin(&vmin)) ||
        R_FAILED(audctlGetTargetVolumeMax(&vmax)) || vmax <= vmin)
        return false;
    s32 vol = vmin + (s32)(value * (float)(vmax - vmin) + 0.5f);
    return R_SUCCEEDED(audctlSetTargetVolume(VOLUME_TARGET, vol));
}

// --- wireless / airplane mode -------------------------------------------------

bool settings_disable_wireless(void) {
    // nifm is initialized by netif_init(); this call goes through that session.
    return R_SUCCEEDED(nifmSetWirelessCommunicationEnabled(false));
}

// --- battery ------------------------------------------------------------------

bool settings_get_battery(uint32_t *out_percent, bool *out_charging) {
    if (!g_psm_ok)
        return false;
    u32 pct = 0;
    PsmChargerType charger = PsmChargerType_Unconnected;
    if (R_FAILED(psmGetBatteryChargePercentage(&pct)))
        return false;
    psmGetChargerType(&charger); // best-effort
    *out_percent = pct;
    *out_charging = (charger != PsmChargerType_Unconnected);
    return true;
}

#else // host / test build: deterministic placeholders

void settings_init(void) {}
void settings_exit(void) {}

bool settings_get_theme(bool *out_dark) { *out_dark = true; return true; }
bool settings_set_theme(bool dark) { (void)dark; return true; }

bool settings_get_nickname(char *out, size_t outsz) {
    snprintf(out, outsz, "Test Switch");
    return true;
}
bool settings_set_nickname(const char *name) { (void)name; return true; }

bool settings_get_brightness(float *out) { *out = 0.5f; return true; }
bool settings_set_brightness(float value) { (void)value; return true; }

bool settings_get_volume(float *out) { *out = 0.5f; return true; }
bool settings_set_volume(float value) { (void)value; return true; }

bool settings_disable_wireless(void) { return true; }

bool settings_get_battery(uint32_t *out_percent, bool *out_charging) {
    *out_percent = 75;
    *out_charging = true;
    return true;
}

#endif
