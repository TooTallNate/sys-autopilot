#include "screen.h"
#include "log.h"

#include <string.h>

#define CAPTURE_TIMEOUT_NS 100000000ULL // 100ms

static u8 g_jpeg_buf[CAPSSC_JPEG_BUFFER_SIZE];

Result screen_capture_jpeg(ViLayerStack stack, const u8 **out_buf, u64 *out_size) {
    u64 size = 0;
    Result rc = capsscCaptureJpegScreenShot(&size, g_jpeg_buf, sizeof(g_jpeg_buf),
                                            stack, CAPTURE_TIMEOUT_NS);
    if (R_FAILED(rc)) {
        LOGF("screen: capture failed rc=0x%x\n", rc);
        return rc;
    }
    *out_buf = g_jpeg_buf;
    *out_size = size;
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
