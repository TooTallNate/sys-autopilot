#include "routes.h"
#include "files.h"
#include "input.h"
#include "screen.h"
#include "log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <switch.h>

#define APP_VERSION "1.0.0"

static u64 g_boot_tick;

__attribute__((constructor)) static void init_boot_tick(void) {
    g_boot_tick = armGetSystemTick();
}

// --- /screenshot -------------------------------------------------------------

static void handle_screenshot(HttpRequest *req) {
    ViLayerStack stack = ViLayerStack_Screenshot;
    char val[32];
    if (http_query_get(req, "stack", val, sizeof(val))) {
        if (!screen_parse_stack(val, &stack)) {
            http_send_error(req->fd, 400, "invalid 'stack' (use screenshot|default|lcd|recording|lastframe)");
            return;
        }
    }

    const u8 *jpeg = NULL;
    u64 size = 0;
    Result rc = screen_capture_jpeg(stack, &jpeg, &size);
    if (R_FAILED(rc)) {
        http_send_json(req->fd, 500, "{\"error\":\"capture failed\",\"rc\":\"0x%x\"}", rc);
        return;
    }
    http_send_response(req->fd, 200, "image/jpeg", jpeg, (size_t)size);
}

// --- /input/* ----------------------------------------------------------------

static bool get_buttons_param(HttpRequest *req, u64 *mask) {
    char val[256];
    if (!http_query_get(req, "buttons", val, sizeof(val)) || val[0] == '\0') {
        http_send_error(req->fd, 400, "missing 'buttons' query parameter");
        return false;
    }
    if (!input_parse_buttons(val, mask)) {
        http_send_error(req->fd, 400, "unknown button name in 'buttons'");
        return false;
    }
    return true;
}

static int get_duration_param(HttpRequest *req, int fallback) {
    char val[32];
    if (http_query_get(req, "durationMs", val, sizeof(val)))
        return atoi(val);
    return fallback;
}

static void send_input_result(HttpRequest *req, Result rc) {
    if (R_FAILED(rc))
        http_send_json(req->fd, 500, "{\"error\":\"input failed\",\"rc\":\"0x%x\"}", rc);
    else
        http_send_json(req->fd, 200, "{\"ok\":true}");
}

static void handle_input_tap(HttpRequest *req) {
    u64 mask;
    if (!get_buttons_param(req, &mask))
        return;
    int duration = get_duration_param(req, INPUT_DEFAULT_TAP_MS);
    send_input_result(req, input_tap(mask, duration));
}

static void handle_input_hold(HttpRequest *req) {
    u64 mask;
    if (!get_buttons_param(req, &mask))
        return;
    send_input_result(req, input_hold(mask));
}

static void handle_input_release(HttpRequest *req) {
    u64 mask;
    if (!get_buttons_param(req, &mask))
        return;
    send_input_result(req, input_release(mask));
}

static void handle_input_stick(HttpRequest *req) {
    char val[32];
    int side = -1;
    if (http_query_get(req, "side", val, sizeof(val))) {
        if (strcasecmp(val, "left") == 0) side = 0;
        else if (strcasecmp(val, "right") == 0) side = 1;
    }
    if (side < 0) {
        http_send_error(req->fd, 400, "missing or invalid 'side' (left|right)");
        return;
    }

    float x = 0.0f, y = 0.0f;
    if (http_query_get(req, "x", val, sizeof(val)))
        x = strtof(val, NULL);
    if (http_query_get(req, "y", val, sizeof(val)))
        y = strtof(val, NULL);
    int duration = get_duration_param(req, 0);

    send_input_result(req, input_stick(side, x, y, duration));
}

// --- /status -----------------------------------------------------------------

static void handle_status(HttpRequest *req) {
    u32 ver = hosversionGet();
    u64 uptime_s = armTicksToNs(armGetSystemTick() - g_boot_tick) / 1000000000ULL;

    http_send_json(req->fd, 200,
                   "{\"version\":\"" APP_VERSION "\","
                   "\"firmware\":\"%u.%u.%u\","
                   "\"controllerAttached\":%s,"
                   "\"uptimeSeconds\":%llu}",
                   HOSVER_MAJOR(ver), HOSVER_MINOR(ver), HOSVER_MICRO(ver),
                   input_is_attached() ? "true" : "false",
                   (unsigned long long)uptime_s);
}

// --- dispatch ----------------------------------------------------------------

typedef struct {
    const char *method;
    const char *path;
    void (*handler)(HttpRequest *req);
} Route;

static void handle_controller_attach(HttpRequest *req) { send_input_result(req, input_attach()); }
static void handle_controller_detach(HttpRequest *req) { send_input_result(req, input_detach()); }
static void handle_input_clear(HttpRequest *req)       { send_input_result(req, input_clear()); }

static const Route kRoutes[] = {
    { "GET",    "/screenshot",        handle_screenshot },
    { "GET",    "/status",            handle_status },
    { "POST",   "/input/tap",         handle_input_tap },
    { "POST",   "/input/hold",        handle_input_hold },
    { "POST",   "/input/release",     handle_input_release },
    { "POST",   "/input/stick",       handle_input_stick },
    { "POST",   "/input/clear",       handle_input_clear },
    { "POST",   "/controller/attach", handle_controller_attach },
    { "POST",   "/controller/detach", handle_controller_detach },
    { "GET",    "/files",             files_handle_get },
    { "PUT",    "/files",             files_handle_put },
    { "DELETE", "/files",             files_handle_delete },
};

void routes_handle(HttpRequest *req) {
    bool path_found = false;
    for (size_t i = 0; i < sizeof(kRoutes) / sizeof(kRoutes[0]); i++) {
        if (strcmp(req->path, kRoutes[i].path) != 0)
            continue;
        path_found = true;
        if (strcmp(req->method, kRoutes[i].method) == 0) {
            kRoutes[i].handler(req);
            return;
        }
    }
    if (path_found)
        http_send_error(req->fd, 405, "method not allowed");
    else
        http_send_error(req->fd, 404, "not found");
}
