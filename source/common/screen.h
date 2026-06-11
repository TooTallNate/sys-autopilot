#pragma once

#include <switch.h>

// Captures the current screen as a JPEG using caps:sc.
// On success, *out_buf points at an internal static buffer valid until the
// next capture, and *out_size is the JPEG size.
Result screen_capture_jpeg(ViLayerStack stack, const u8 **out_buf, u64 *out_size);

// Maps a query-param name to a ViLayerStack. Returns true if recognized.
bool screen_parse_stack(const char *name, ViLayerStack *out);
