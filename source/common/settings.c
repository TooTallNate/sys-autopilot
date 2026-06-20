#include "settings.h"

#include <stdio.h>
#include <string.h>

#ifdef __SWITCH__
#include <switch.h>

#include "log.h"

// Volume is read/written against the speaker target; this is the value the
// system Quick Settings slider controls for the active output.
#define VOLUME_TARGET AudioTarget_Speaker

static bool g_setsys_ok;
static bool g_lbl_ok;
static bool g_audctl_ok;
static bool g_psm_ok;

void settings_init(void) {
    // set:sys must be opened here, while the sm session is still open: it is
    // used by theme/nickname/auto-time, and a later setsysInitialize() after
    // smExit() would fail. Holding this reference keeps the session alive for
    // the process lifetime (setsysInitialize is refcounted).
    g_setsys_ok = R_SUCCEEDED(setsysInitialize());
    g_lbl_ok    = R_SUCCEEDED(lblInitialize());
    g_audctl_ok = R_SUCCEEDED(audctlInitialize());
    g_psm_ok    = R_SUCCEEDED(psmInitialize());
    if (!g_setsys_ok) LOGF("settings: set:sys init failed\n");
    if (!g_lbl_ok)    LOGF("settings: lbl init failed\n");
    if (!g_audctl_ok) LOGF("settings: audctl init failed\n");
    if (!g_psm_ok)    LOGF("settings: psm init failed\n");
}

void settings_exit(void) {
    if (g_setsys_ok) { setsysExit(); g_setsys_ok = false; }
    if (g_lbl_ok)    { lblExit();    g_lbl_ok = false; }
    if (g_audctl_ok) { audctlExit(); g_audctl_ok = false; }
    if (g_psm_ok)    { psmExit();    g_psm_ok = false; }
}

// --- theme --------------------------------------------------------------------

bool settings_get_theme(bool *out_dark) {
    if (!g_setsys_ok)
        return false;
    ColorSetId id = ColorSetId_Light;
    if (R_FAILED(setsysGetColorSetId(&id)))
        return false;
    *out_dark = (id == ColorSetId_Dark);
    return true;
}

bool settings_set_theme(bool dark) {
    if (!g_setsys_ok)
        return false;
    return R_SUCCEEDED(
        setsysSetColorSetId(dark ? ColorSetId_Dark : ColorSetId_Light));
}

// --- nickname -----------------------------------------------------------------

bool settings_get_nickname(char *out, size_t outsz) {
    if (!g_setsys_ok)
        return false;
    SetSysDeviceNickName nn = {0};
    if (R_FAILED(setsysGetDeviceNickname(&nn)))
        return false;
    snprintf(out, outsz, "%s", nn.nickname);
    return true;
}

bool settings_set_nickname(const char *name) {
    if (!g_setsys_ok)
        return false;
    SetSysDeviceNickName nn = {0};
    snprintf(nn.nickname, sizeof(nn.nickname), "%s", name);
    return R_SUCCEEDED(setsysSetDeviceNickname(&nn));
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

// --- automatic clock sync -----------------------------------------------------

bool settings_get_auto_time(bool *out_enabled) {
    if (!g_setsys_ok)
        return false;
    bool en = false;
    if (R_FAILED(setsysIsUserSystemClockAutomaticCorrectionEnabled(&en)))
        return false;
    *out_enabled = en;
    return true;
}

bool settings_set_auto_time(bool enabled) {
    if (!g_setsys_ok)
        return false;
    return R_SUCCEEDED(setsysSetUserSystemClockAutomaticCorrectionEnabled(enabled));
}

// --- date / time / timezone ---------------------------------------------------

bool settings_get_datetime(DateTime *out) {
    // time is initialized in __appInit (TimeServiceType_System).
    u64 ts = 0;
    if (R_FAILED(timeGetCurrentTime(TimeType_UserSystemClock, &ts)))
        return false;

    TimeCalendarTime cal = {0};
    TimeCalendarAdditionalInfo info = {0};
    if (R_FAILED(timeToCalendarTimeWithMyRule(ts, &cal, &info)))
        return false;

    out->year   = cal.year;
    out->month  = cal.month;
    out->day    = cal.day;
    out->hour   = cal.hour;
    out->minute = cal.minute;
    out->second = cal.second;

    TimeLocationName loc = {0};
    if (R_SUCCEEDED(timeGetDeviceLocationName(&loc)))
        snprintf(out->timezone, sizeof(out->timezone), "%s", loc.name);
    else
        out->timezone[0] = '\0';
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

bool settings_get_auto_time(bool *out_enabled) { *out_enabled = true; return true; }
bool settings_set_auto_time(bool enabled) { (void)enabled; return true; }

bool settings_get_datetime(DateTime *out) {
    out->year = 2026; out->month = 6; out->day = 19;
    out->hour = 12; out->minute = 0; out->second = 0;
    snprintf(out->timezone, sizeof(out->timezone), "America/New_York");
    return true;
}

#endif
