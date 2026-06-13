#include "routes.h"
#include "apiargs.h"
#include "files.h"
#include "input.h"
#include "json.h"
#include "mcp.h"
#include "oauth.h"
#include "power.h"
#include "screen.h"
#include "log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <switch.h>

// Injected by the Makefile from package.json (the changesets-managed version).
#ifndef APP_VERSION
#define APP_VERSION "0.0.0-dev"
#endif

// Cap for JSON request bodies on the REST input endpoints.
#define REST_BODY_MAX 16384

static u64 g_boot_tick;

__attribute__((constructor)) static void init_boot_tick(void) {
    g_boot_tick = armGetSystemTick();
}

const char *routes_app_version(void) {
    return APP_VERSION;
}

u64 routes_uptime_seconds(void) {
    return armTicksToNs(armGetSystemTick() - g_boot_tick) / 1000000000ULL;
}

// Reads and parses a JSON request body into *doc. An absent/empty body parses
// as an empty object. Returns the root token index, or -1 after sending an
// error response.
static int read_json_body(HttpRequest *req, JsonDoc *doc) {
    static char body[REST_BODY_MAX + 1];
    size_t total = 0;

    if (req->has_content_length && req->content_length > REST_BODY_MAX) {
        http_send_error(req->fd, 413, "request body too large");
        return -1;
    }
    ssize_t n;
    while (total < REST_BODY_MAX &&
           (n = http_read_body(req, body + total, REST_BODY_MAX - total)) > 0)
        total += (size_t)n;

    if (total == 0) {
        strcpy(body, "{}");
        total = 2;
    }
    if (json_parse(doc, body, total) != 0 || doc->ntok < 1 ||
        doc->tok[0].type != JSMN_OBJECT) {
        http_send_error(req->fd, 400, "request body must be a JSON object");
        return -1;
    }
    return 0; // root token
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

static void send_input_result(HttpRequest *req, Result rc) {
    if (R_FAILED(rc))
        http_send_json(req->fd, 500, "{\"error\":\"input failed\",\"rc\":\"0x%x\"}", rc);
    else
        http_send_json(req->fd, 200, "{\"ok\":true}");
}

static void handle_input_tap(HttpRequest *req) {
    static JsonDoc doc;
    int root = read_json_body(req, &doc);
    if (root < 0)
        return;
    u64 mask;
    const char *err = NULL;
    if (!args_get_buttons(&doc, root, &mask, &err)) {
        http_send_error(req->fd, 400, err);
        return;
    }
    send_input_result(req, input_tap(mask, args_get_duration(&doc, root,
                                                             INPUT_DEFAULT_TAP_MS)));
}

static void handle_input_hold_release(HttpRequest *req, bool hold) {
    static JsonDoc doc;
    int root = read_json_body(req, &doc);
    if (root < 0)
        return;
    u64 mask;
    const char *err = NULL;
    if (!args_get_buttons(&doc, root, &mask, &err)) {
        http_send_error(req->fd, 400, err);
        return;
    }
    send_input_result(req, hold ? input_hold(mask) : input_release(mask));
}

static void handle_input_hold(HttpRequest *req)    { handle_input_hold_release(req, true); }
static void handle_input_release(HttpRequest *req) { handle_input_hold_release(req, false); }

static void handle_input_stick(HttpRequest *req) {
    static JsonDoc doc;
    int root = read_json_body(req, &doc);
    if (root < 0)
        return;
    int side, duration;
    float x, y;
    const char *err = NULL;
    if (!args_get_stick(&doc, root, &side, &x, &y, &duration, &err)) {
        http_send_error(req->fd, 400, err);
        return;
    }
    send_input_result(req, input_stick(side, x, y, duration));
}

// --- /status -----------------------------------------------------------------

static void handle_status(HttpRequest *req) {
    u32 ver = hosversionGet();
    http_send_json(req->fd, 200,
                   "{\"version\":\"" APP_VERSION "\","
                   "\"firmware\":\"%u.%u.%u\","
                   "\"controllerAttached\":%s,"
                   "\"uptimeSeconds\":%llu}",
                   HOSVER_MAJOR(ver), HOSVER_MINOR(ver), HOSVER_MICRO(ver),
                   input_is_attached() ? "true" : "false",
                   (unsigned long long)routes_uptime_seconds());
}

// --- /power/* ------------------------------------------------------------------

static void handle_power(HttpRequest *req, PowerAction action, const char *note) {
    if (!power_actions_available()) {
        http_send_error(req->fd, 500, "power control unavailable");
        return;
    }
    http_send_json(req->fd, 200, "{\"ok\":true,\"note\":\"%s\"}", note);
    power_schedule(action); // executed after this response is flushed
}

static void handle_power_sleep(HttpRequest *req) {
    handle_power(req, PowerAction_Sleep,
                 "entering sleep; server unreachable until console is woken physically");
}

static void handle_power_restart(HttpRequest *req) {
    handle_power(req, PowerAction_Restart,
                 "rebooting; server returns after the console boots back into CFW (bootloader menus may require manual intervention)");
}

static void handle_power_off(HttpRequest *req) {
    handle_power(req, PowerAction_PowerOff,
                 "powering off; physical power button required to turn back on");
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
    { "GET",    "/files/hash",        files_handle_hash },
    { "PUT",    "/files",             files_handle_put },
    { "DELETE", "/files",             files_handle_delete },
    { "POST",   "/mcp",               mcp_handle_post },
    { "POST",   "/power/sleep",       handle_power_sleep },
    { "POST",   "/power/restart",     handle_power_restart },
    { "POST",   "/power/off",         handle_power_off },
    { "POST",   "/oauth/register",    oauth_handle_register },
    { "GET",    "/oauth/authorize",   oauth_handle_authorize_get },
    { "POST",   "/oauth/authorize",   oauth_handle_authorize_post },
    { "POST",   "/oauth/token",       oauth_handle_token },
};

// CORS preflight: browsers send OPTIONS with no Authorization header before
// cross-origin requests (e.g. web-based MCP clients).
static void handle_options(HttpRequest *req) {
    static const char hdr[] =
        "HTTP/1.1 204 No Content\r\n"
        "Server: sys-autopilot\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "Access-Control-Allow-Methods: GET, POST, PUT, DELETE, OPTIONS\r\n"
        "Access-Control-Allow-Headers: Authorization, Content-Type, Mcp-Protocol-Version\r\n"
        "Access-Control-Max-Age: 86400\r\n"
        "Connection: close\r\n"
        "\r\n";
    http_write_all(req->fd, hdr, sizeof(hdr) - 1);
}

void routes_handle(HttpRequest *req) {
    if (strcmp(req->method, "OPTIONS") == 0) {
        handle_options(req);
        return;
    }

    // OAuth discovery documents (clients probe both the bare path and the
    // RFC 9728 path-suffix variant, e.g. .../oauth-protected-resource/mcp).
    if (strcmp(req->method, "GET") == 0) {
        if (strncmp(req->path, "/.well-known/oauth-protected-resource", 38) == 0) {
            oauth_handle_protected_resource(req);
            return;
        }
        if (strncmp(req->path, "/.well-known/oauth-authorization-server", 39) == 0 ||
            strncmp(req->path, "/.well-known/openid-configuration", 33) == 0) {
            oauth_handle_as_metadata(req);
            return;
        }
    }

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
