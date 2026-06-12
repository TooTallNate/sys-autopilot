// Host-side stubs for the Switch-coupled modules (input, screen, routes
// metadata) so mcp.c can be exercised end-to-end in tests.
#include <switch.h>
#include <string.h>
#include <strings.h>

#include "input.h"
#include "screen.h"
#include "routes.h"

u64 stub_tap_mask;
int stub_tap_duration;
int stub_tap_count;
u64 stub_hold_mask;
int stub_stick_side;
float stub_stick_x, stub_stick_y;
bool stub_cleared;

void input_suspend(void) {}
void input_exit(void) {}
Result input_attach(void) { return 0; }
Result input_detach(void) { return 0; }
bool input_is_attached(void) { return true; }

Result input_tap(u64 mask, int duration_ms) {
    stub_tap_mask = mask;
    stub_tap_duration = duration_ms;
    stub_tap_count++;
    return 0;
}

Result input_hold(u64 mask) { stub_hold_mask |= mask; return 0; }
Result input_release(u64 mask) { stub_hold_mask &= ~mask; return 0; }
Result input_clear(void) { stub_cleared = true; stub_hold_mask = 0; return 0; }

Result input_stick(int side, float x, float y, int duration_ms) {
    (void)duration_ms;
    stub_stick_side = side;
    stub_stick_x = x;
    stub_stick_y = y;
    return 0;
}

static const u8 kFakeJpeg[] = "FAKEJPEGDATA";

Result screen_capture_jpeg(ViLayerStack stack, const u8 **out_buf, u64 *out_size) {
    (void)stack;
    *out_buf = kFakeJpeg;
    *out_size = sizeof(kFakeJpeg) - 1;
    return 0;
}

bool screen_parse_stack(const char *name, ViLayerStack *out) {
    if (strcasecmp(name, "screenshot") == 0)     *out = ViLayerStack_Screenshot;
    else if (strcasecmp(name, "default") == 0)   *out = ViLayerStack_Default;
    else if (strcasecmp(name, "lcd") == 0)       *out = ViLayerStack_Lcd;
    else if (strcasecmp(name, "recording") == 0) *out = ViLayerStack_Recording;
    else if (strcasecmp(name, "lastframe") == 0) *out = ViLayerStack_LastFrame;
    else return false;
    return true;
}

const char *routes_app_version(void) { return "test"; }
uint64_t routes_uptime_seconds(void) { return 42; }
