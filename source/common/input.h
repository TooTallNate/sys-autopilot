#pragma once

#include <switch.h>

#define INPUT_DEFAULT_TAP_MS 100
#define INPUT_MAX_DURATION_MS 10000

// Attaches the HDLS work buffer. Call once at startup (after hiddbgInitialize).
Result input_init(void);

// Detaches virtual device + work buffer. Call before hiddbgExit.
void input_exit(void);

// Attach/detach the virtual Pro Controller.
Result input_attach(void);
Result input_detach(void);
bool   input_is_attached(void);

// Parses a comma-separated button list ("A,B,HOME") into a button mask.
// Returns false on any unknown button name.
bool input_parse_buttons(const char *csv, u64 *out_mask);

// Press+release: applies mask, sleeps duration_ms, releases mask.
Result input_tap(u64 mask, int duration_ms);

Result input_hold(u64 mask);
Result input_release(u64 mask);
Result input_clear(void);

// side: 0 = left stick, 1 = right stick. x/y in [-1.0, 1.0].
// If duration_ms > 0, the stick returns to neutral afterwards.
Result input_stick(int side, float x, float y, int duration_ms);
