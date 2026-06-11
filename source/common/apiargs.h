#pragma once

#include "json.h"

#include <stdbool.h>
#include <stdint.h>

// Shared JSON argument extraction for input endpoints (REST bodies and MCP
// tool arguments use identical shapes).

// {"buttons":["A","B"], ...} -> combined mask. *err set on failure.
bool args_get_buttons(const JsonDoc *doc, int obj, uint64_t *out_mask,
                      const char **err);

// Optional {"durationMs": N}; returns fallback when absent.
int args_get_duration(const JsonDoc *doc, int obj, int fallback);

// {"side":"left|right","x":F,"y":F,"durationMs":N} -> parsed stick command.
bool args_get_stick(const JsonDoc *doc, int obj, int *out_side, float *out_x,
                    float *out_y, int *out_duration, const char **err);
