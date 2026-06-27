#include "routes.h"
#include "apiargs.h"
#include "files.h"
#include "input.h"
#include "install.h"
#include "titles.h"
#include "json.h"
#include "mcp.h"
#include "oauth.h"
#include "power.h"
#include "screen.h"
#include "settings.h"
#include "log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
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
    uint32_t pct = 0;
    bool charging = false;
    bool have_batt = settings_get_battery(&pct, &charging);
    if (have_batt) {
        http_send_json(req->fd, 200,
                       "{\"version\":\"" APP_VERSION "\","
                       "\"firmware\":\"%u.%u.%u\","
                       "\"controllerAttached\":%s,"
                       "\"uptimeSeconds\":%llu,"
                       "\"batteryPercent\":%u,"
                       "\"charging\":%s}",
                       HOSVER_MAJOR(ver), HOSVER_MINOR(ver), HOSVER_MICRO(ver),
                       input_is_attached() ? "true" : "false",
                       (unsigned long long)routes_uptime_seconds(),
                       pct, charging ? "true" : "false");
    } else {
        http_send_json(req->fd, 200,
                       "{\"version\":\"" APP_VERSION "\","
                       "\"firmware\":\"%u.%u.%u\","
                       "\"controllerAttached\":%s,"
                       "\"uptimeSeconds\":%llu}",
                       HOSVER_MAJOR(ver), HOSVER_MINOR(ver), HOSVER_MICRO(ver),
                       input_is_attached() ? "true" : "false",
                       (unsigned long long)routes_uptime_seconds());
    }
}

// --- /settings/* --------------------------------------------------------------

static void handle_settings_theme(HttpRequest *req) {
    if (strcmp(req->method, "GET") == 0) {
        bool dark = false;
        if (!settings_get_theme(&dark)) {
            http_send_error(req->fd, 500, "failed to read theme");
            return;
        }
        http_send_json(req->fd, 200, "{\"theme\":\"%s\"}", dark ? "dark" : "light");
        return;
    }
    // POST {"theme":"light"|"dark"}
    static JsonDoc doc;
    int root = read_json_body(req, &doc);
    if (root < 0)
        return;
    char val[16] = {0};
    int t = json_obj_get(&doc, root, "theme");
    if (t < 0 || !json_get_string(&doc, t, val, sizeof(val))) {
        http_send_error(req->fd, 400, "missing 'theme' (\"light\" or \"dark\")");
        return;
    }
    bool dark;
    if (strcasecmp(val, "dark") == 0)       dark = true;
    else if (strcasecmp(val, "light") == 0) dark = false;
    else { http_send_error(req->fd, 400, "'theme' must be \"light\" or \"dark\""); return; }

    if (!settings_set_theme(dark))
        http_send_error(req->fd, 500, "failed to set theme");
    else
        http_send_json(req->fd, 200, "{\"ok\":true,\"theme\":\"%s\"}", dark ? "dark" : "light");
}

static void handle_settings_nickname(HttpRequest *req) {
    if (strcmp(req->method, "GET") == 0) {
        char name[128] = {0};
        if (!settings_get_nickname(name, sizeof(name))) {
            http_send_error(req->fd, 500, "failed to read nickname");
            return;
        }
        http_send_json(req->fd, 200, "{\"nickname\":\"%s\"}", name);
        return;
    }
    static JsonDoc doc;
    int root = read_json_body(req, &doc);
    if (root < 0)
        return;
    char name[128] = {0};
    int t = json_obj_get(&doc, root, "nickname");
    if (t < 0 || !json_get_string(&doc, t, name, sizeof(name)) || name[0] == '\0') {
        http_send_error(req->fd, 400, "missing non-empty 'nickname'");
        return;
    }
    if (!settings_set_nickname(name))
        http_send_error(req->fd, 500, "failed to set nickname");
    else
        http_send_json(req->fd, 200, "{\"ok\":true,\"nickname\":\"%s\"}", name);
}

// Shared get/set for the two normalized 0..1 float settings.
static void handle_settings_float(HttpRequest *req, const char *key,
                                  bool (*get)(float *), bool (*set)(float)) {
    if (strcmp(req->method, "GET") == 0) {
        float v = 0.0f;
        if (!get(&v)) {
            http_send_error(req->fd, 500, "failed to read setting");
            return;
        }
        http_send_json(req->fd, 200, "{\"%s\":%.3f}", key, v);
        return;
    }
    static JsonDoc doc;
    int root = read_json_body(req, &doc);
    if (root < 0)
        return;
    int t = json_obj_get(&doc, root, key);
    double v;
    if (t < 0 || !json_get_double(&doc, t, &v)) {
        http_send_error(req->fd, 400, "missing numeric value (0.0 - 1.0)");
        return;
    }
    if (!set((float)v))
        http_send_error(req->fd, 500, "failed to set setting");
    else
        http_send_json(req->fd, 200, "{\"ok\":true,\"%s\":%.3f}", key,
                       v < 0 ? 0 : (v > 1 ? 1 : v));
}

static void handle_settings_brightness(HttpRequest *req) {
    handle_settings_float(req, "brightness",
                          settings_get_brightness, settings_set_brightness);
}

static void handle_settings_volume(HttpRequest *req) {
    handle_settings_float(req, "volume",
                          settings_get_volume, settings_set_volume);
}

static void handle_settings_airplane(HttpRequest *req) {
    // Enable-only: disabling wireless cuts our own connectivity.
    if (!settings_disable_wireless()) {
        http_send_error(req->fd, 500, "failed to disable wireless");
        return;
    }
    http_send_json(req->fd, 200,
                   "{\"ok\":true,\"note\":\"wireless disabled; the server is now "
                   "unreachable until wireless is re-enabled on the console\"}");
}

static void handle_settings_auto_time(HttpRequest *req) {
    if (strcmp(req->method, "GET") == 0) {
        bool en = false;
        if (!settings_get_auto_time(&en)) {
            http_send_error(req->fd, 500, "failed to read auto-time");
            return;
        }
        http_send_json(req->fd, 200, "{\"autoTime\":%s}", en ? "true" : "false");
        return;
    }
    static JsonDoc doc;
    int root = read_json_body(req, &doc);
    if (root < 0)
        return;
    int t = json_obj_get(&doc, root, "autoTime");
    bool en;
    if (t < 0 || !json_get_bool(&doc, t, &en)) {
        http_send_error(req->fd, 400, "missing boolean 'autoTime'");
        return;
    }
    if (!settings_set_auto_time(en))
        http_send_error(req->fd, 500, "failed to set auto-time");
    else
        http_send_json(req->fd, 200, "{\"ok\":true,\"autoTime\":%s}", en ? "true" : "false");
}

// Reads an integer field from a JSON object; returns def if absent/invalid.
static int json_field_int(const JsonDoc *doc, int obj, const char *key, int def) {
    int t = json_obj_get(doc, obj, key);
    long long v;
    if (t >= 0 && json_get_int(doc, t, &v))
        return (int)v;
    return def;
}

static void handle_settings_datetime(HttpRequest *req) {
    if (strcmp(req->method, "GET") == 0) {
        DateTime dt = {0};
        if (!settings_get_datetime(&dt)) {
            http_send_error(req->fd, 500, "failed to read date/time");
            return;
        }
        http_send_json(req->fd, 200,
                       "{\"year\":%d,\"month\":%d,\"day\":%d,"
                       "\"hour\":%d,\"minute\":%d,\"second\":%d,"
                       "\"timezone\":\"%s\"}",
                       dt.year, dt.month, dt.day, dt.hour, dt.minute, dt.second,
                       dt.timezone);
        return;
    }
    static JsonDoc doc;
    int root = read_json_body(req, &doc);
    if (root < 0)
        return;

    DateTime dt = {0};
    settings_get_datetime(&dt);
    dt.year   = json_field_int(&doc, root, "year",   dt.year);
    dt.month  = json_field_int(&doc, root, "month",  dt.month);
    dt.day    = json_field_int(&doc, root, "day",    dt.day);
    dt.hour   = json_field_int(&doc, root, "hour",   dt.hour);
    dt.minute = json_field_int(&doc, root, "minute", dt.minute);
    dt.second = json_field_int(&doc, root, "second", dt.second);

    if (dt.month < 1 || dt.month > 12 || dt.day < 1 || dt.day > 31 ||
        dt.hour < 0 || dt.hour > 23 || dt.minute < 0 || dt.minute > 59 ||
        dt.second < 0 || dt.second > 59 || dt.year < 2000 || dt.year > 2100) {
        http_send_error(req->fd, 400, "invalid date/time fields");
        return;
    }
    if (!settings_set_datetime(&dt))
        http_send_error(req->fd, 500, "failed to set the clock");
    else
        http_send_json(req->fd, 200,
                       "{\"ok\":true,\"year\":%d,\"month\":%d,\"day\":%d,"
                       "\"hour\":%d,\"minute\":%d,\"second\":%d,\"timezone\":\"%s\"}",
                       dt.year, dt.month, dt.day, dt.hour, dt.minute, dt.second,
                       dt.timezone);
}

// --- /titles -----------------------------------------------------------------

// Maps an NcmStorageId to the short label used in the JSON response.
static const char *storage_label(uint8_t storage_id) {
    switch (storage_id) {
        case 5: return "sd";
        case 4: return "nand";
        case 2: return "gamecard";
        default: return "other";
    }
}

static void handle_titles(HttpRequest *req) {
    static TitleInfo titles[TITLES_MAX];
    int count = 0;
    char err[128] = {0};
    if (!titles_list(titles, TITLES_MAX, &count, err, sizeof(err))) {
        http_send_error(req->fd, 500, err[0] ? err : "failed to list titles");
        return;
    }

    // Build the JSON into a static buffer (the list can be large). Each entry is
    // well under 256 bytes; TITLES_MAX entries fit comfortably.
    static char body[TITLES_MAX * 256 + 32];
    size_t pos = (size_t)snprintf(body, sizeof(body), "{\"titles\":[");
    for (int i = 0; i < count && pos < sizeof(body) - 256; i++) {
        char name[160];
        json_escape(titles[i].name, strlen(titles[i].name), name, sizeof(name));
        pos += (size_t)snprintf(body + pos, sizeof(body) - pos,
            "%s{\"titleId\":\"%016llx\",\"version\":%u,\"storage\":\"%s\",\"name\":\"%s\"}",
            i ? "," : "", (unsigned long long)titles[i].title_id,
            titles[i].version, storage_label(titles[i].storage_id), name);
    }
    pos += (size_t)snprintf(body + pos, sizeof(body) - pos, "]}");
    http_send_response(req->fd, 200, "application/json", body, pos);
}

// --- /install ----------------------------------------------------------------

// Adapts the HTTP request body into the installer's sequential read callback.
static long install_body_read(void *ctx, void *buf, size_t len) {
    return (long)http_read_body((HttpRequest *)ctx, buf, len);
}

static void handle_install(HttpRequest *req) {
    if (!req->has_content_length) {
        http_send_error(req->fd, 411, "Content-Length required (stream the NSP with curl -T)");
        return;
    }

    // ?storage=sd|nand (default sd)
    InstallStorage storage = INSTALL_STORAGE_SD;
    char sval[8];
    if (http_query_get(req, "storage", sval, sizeof(sval)) &&
        strcasecmp(sval, "nand") == 0)
        storage = INSTALL_STORAGE_NAND;

    InstallResult res;
    install_nsp_stream(install_body_read, req, req->content_length, storage, &res);

    char esc[256];
    json_escape(res.message, strlen(res.message), esc, sizeof(esc));
    if (res.ok) {
        http_send_json(req->fd, 200,
                       "{\"ok\":true,\"titleId\":\"%016llx\",\"version\":%u,\"message\":\"%s\"}",
                       (unsigned long long)res.title_id, res.version, esc);
    } else {
        http_send_json(req->fd, res.http_status ? res.http_status : 500,
                       "{\"ok\":false,\"error\":\"%s\"}", esc);
    }
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
    { "GET",    "/titles",            handle_titles },
    { "POST",   "/install",           handle_install },
    { "PUT",    "/install",           handle_install },
    { "POST",   "/mcp",               mcp_handle_post },
    { "POST",   "/power/sleep",       handle_power_sleep },
    { "POST",   "/power/restart",     handle_power_restart },
    { "POST",   "/power/off",         handle_power_off },
    { "GET",    "/settings/theme",        handle_settings_theme },
    { "POST",   "/settings/theme",        handle_settings_theme },
    { "GET",    "/settings/nickname",     handle_settings_nickname },
    { "POST",   "/settings/nickname",     handle_settings_nickname },
    { "GET",    "/settings/brightness",   handle_settings_brightness },
    { "POST",   "/settings/brightness",   handle_settings_brightness },
    { "GET",    "/settings/volume",       handle_settings_volume },
    { "POST",   "/settings/volume",       handle_settings_volume },
    { "POST",   "/settings/airplane",     handle_settings_airplane },
    { "GET",    "/settings/auto-time",    handle_settings_auto_time },
    { "POST",   "/settings/auto-time",    handle_settings_auto_time },
    { "GET",    "/settings/datetime",     handle_settings_datetime },
    { "POST",   "/settings/datetime",     handle_settings_datetime },
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
