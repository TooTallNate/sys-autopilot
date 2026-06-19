#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// System-settings helpers wrapping Switch services (set:sys, lbl, audctl,
// nifm, psm). Each get/set returns true on success. The required services are
// initialized once at startup by settings_init() (called while the sm session
// is open) and torn down by settings_exit(). On host (test) builds the getters
// return deterministic placeholders and setters succeed as no-ops.

// Initializes the services used here (lbl, audctl, psm). set:sys and nifm are
// initialized elsewhere. Best-effort: a service that fails to open simply
// makes its corresponding get/set return false. Call from __appInit.
void settings_init(void);

// Tears down services opened by settings_init(). Call from __appExit.
void settings_exit(void);

// --- theme (UI color set) -----------------------------------------------------
// theme is "light" or "dark".
bool settings_get_theme(bool *out_dark);
bool settings_set_theme(bool dark);

// --- device nickname ----------------------------------------------------------
bool settings_get_nickname(char *out, size_t outsz);
bool settings_set_nickname(const char *name);

// --- screen brightness --------------------------------------------------------
// Brightness is 0.0..1.0. Setting also applies it to the backlight immediately.
bool settings_get_brightness(float *out);
bool settings_set_brightness(float value);

// --- master volume ------------------------------------------------------------
// Normalized 0.0..1.0 (mapped to the system's target volume min/max range).
bool settings_get_volume(float *out);
bool settings_set_volume(float value);

// --- airplane mode (wireless) -------------------------------------------------
// One-way by design: disabling wireless cuts the server's own connectivity, so
// only disabling is exposed. Returns true if the request was issued.
bool settings_disable_wireless(void);

// --- battery (read-only) ------------------------------------------------------
// Reports charge percentage (0..100) and whether a charger is connected.
bool settings_get_battery(uint32_t *out_percent, bool *out_charging);
