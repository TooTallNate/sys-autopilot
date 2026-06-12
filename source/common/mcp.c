#include "mcp.h"
#include "apiargs.h"
#include "base64.h"
#include "config.h"
#include "files.h"
#include "input.h"
#include "json.h"
#include "oauth.h"
#include "power.h"
#include "jstream.h"
#include "routes.h"
#include "screen.h"
#include "log.h"
#include "mcp_tools.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

// Cap for the *reduced* JSON-RPC document (everything except a streamed
// upload_file content field, which goes straight to disk).
#define MCP_DOC_MAX 16384

// Temp destination for streamed upload content (renamed into place on
// success, deleted otherwise).
#ifndef MCP_UPLOAD_TMP
#define MCP_UPLOAD_TMP CONFIG_DIR "/.upload.tmp"
#endif

#define MCP_PROTO_LATEST "2025-06-18"

#define READ_FILE_MAX 32768
#define TAP_SEQ_MAX 32

// --- upload content sink -------------------------------------------------------

typedef struct {
    FILE *f;
    B64Decoder dec;
    size_t bytes;
    bool failed;
} UploadCtx;

static UploadCtx g_upload;

// Note: failures (bad base64, write error) latch u->failed but return 0 so
// the rest of the request still streams through and the JSON-RPC layer can
// report a proper in-band tool error instead of a protocol error.
static int upload_sink(const char *data, size_t len, void *ctx) {
    UploadCtx *u = ctx;
    static uint8_t decbuf[0x8000];
    if (u->failed)
        return 0; // keep draining
    if (!u->f) {
        files_mkdirs_for(MCP_UPLOAD_TMP);
        u->f = fopen(MCP_UPLOAD_TMP, "wb");
        if (!u->f) {
            u->failed = true;
            return 0;
        }
    }
    while (len > 0) {
        // decbuf holds the decode of up to 0xA000 input chars; bound input so
        // (n/4+1)*3 always fits.
        size_t n = len > 0xA000 ? 0xA000 : len;
        ssize_t dn = b64dec_update(&u->dec, data, n, decbuf);
        if (dn < 0 || fwrite(decbuf, 1, (size_t)dn, u->f) != (size_t)dn) {
            u->failed = true;
            return 0;
        }
        u->bytes += (size_t)dn;
        data += n;
        len -= n;
    }
    return 0;
}

// Closes and removes any leftover upload temp state. No-op when nothing was
// streamed (the common case), so non-upload requests never touch the SD card.
static void upload_cleanup(void) {
    if (g_upload.f)
        fclose(g_upload.f);
    if (g_upload.f || g_upload.bytes > 0 || g_upload.failed)
        remove(MCP_UPLOAD_TMP);
    memset(&g_upload, 0, sizeof(g_upload));
}

// --- response helpers ----------------------------------------------------------

// Sends {"jsonrpc":"2.0","id":<id>,"<key>":<value>} with exact Content-Length.
static void send_rpc_value(int fd, const char *id, const char *key,
                           const char *value, size_t value_len) {
    char head[96];
    int hn = snprintf(head, sizeof(head), "{\"jsonrpc\":\"2.0\",\"id\":%s,\"%s\":",
                      id, key);
    http_send_header(fd, 200, "application/json", (size_t)hn + value_len + 1);
    http_write_all(fd, head, (size_t)hn);
    http_write_all(fd, value, value_len);
    http_write_all(fd, "}", 1);
}

static void send_rpc_error(int fd, const char *id, int code, const char *msg) {
    char val[256];
    int n = snprintf(val, sizeof(val), "{\"code\":%d,\"message\":\"%s\"}", code, msg);
    send_rpc_value(fd, id, "error", val, (size_t)n);
}

// Sends a result of the form <pre><payload><post> (payload may be huge).
static void send_result_stream(int fd, const char *id, const char *pre,
                               const char *payload, size_t payload_len,
                               const char *post) {
    char head[96];
    int hn = snprintf(head, sizeof(head), "{\"jsonrpc\":\"2.0\",\"id\":%s,\"result\":", id);
    size_t total = (size_t)hn + strlen(pre) + payload_len + strlen(post) + 1;
    http_send_header(fd, 200, "application/json", total);
    http_write_all(fd, head, (size_t)hn);
    http_write_all(fd, pre, strlen(pre));
    if (payload_len > 0)
        http_write_all(fd, payload, payload_len);
    http_write_all(fd, post, strlen(post));
    http_write_all(fd, "}", 1);
}

#define TEXT_PRE  "{\"content\":[{\"type\":\"text\",\"text\":\""
#define TEXT_POST_OK  "\"}],\"isError\":false}"
#define TEXT_POST_ERR "\"}],\"isError\":true}"

// Tool result with plain (unescaped) text content.
static void send_tool_text(int fd, const char *id, bool is_error, const char *text) {
    size_t tlen = strlen(text);
    size_t esc_len = json_escaped_len(text, tlen);
    char *escaped = malloc(esc_len + 1);
    if (!escaped) {
        send_rpc_error(fd, id, -32603, "out of memory");
        return;
    }
    json_escape(text, tlen, escaped, esc_len + 1);
    send_result_stream(fd, id, TEXT_PRE, escaped, esc_len,
                       is_error ? TEXT_POST_ERR : TEXT_POST_OK);
    free(escaped);
}

static void send_tool_ok(int fd, const char *id, const char *text) {
    send_tool_text(fd, id, false, text);
}

static void send_tool_error(int fd, const char *id, const char *text) {
    send_tool_text(fd, id, true, text);
}

static void send_input_tool_result(int fd, const char *id, Result rc, const char *ok_msg) {
    if (R_FAILED(rc)) {
        char msg[64];
        snprintf(msg, sizeof(msg), "input failed (rc=0x%x)", rc);
        send_tool_error(fd, id, msg);
    } else {
        send_tool_ok(fd, id, ok_msg);
    }
}

static int get_int_or(const JsonDoc *doc, int obj, const char *key, int fallback);

// Streams a result whose content is [text, image] (the screenshot-after-input
// case). text must not require JSON escaping (static messages only).
static void send_tool_text_and_image(int fd, const char *id, const char *text,
                                     const u8 *jpeg, u64 jpeg_size) {
    static const char img_pre[] = "\"},{\"type\":\"image\",\"data\":\"";
    static const char img_post[] = "\",\"mimeType\":\"image/jpeg\"}],\"isError\":false}";
    char head[96];
    int hn = snprintf(head, sizeof(head), "{\"jsonrpc\":\"2.0\",\"id\":%s,\"result\":", id);
    size_t b64len = b64_encoded_len((size_t)jpeg_size);
    size_t total = (size_t)hn + sizeof(TEXT_PRE) - 1 + strlen(text) +
                   sizeof(img_pre) - 1 + b64len + sizeof(img_post) - 1 + 1;

    http_send_header(fd, 200, "application/json", total);
    http_write_all(fd, head, (size_t)hn);
    http_write_all(fd, TEXT_PRE, sizeof(TEXT_PRE) - 1);
    http_write_all(fd, text, strlen(text));
    http_write_all(fd, img_pre, sizeof(img_pre) - 1);

    static char enc[4096];
    for (size_t off = 0; off < jpeg_size; off += 3072) {
        size_t chunk = jpeg_size - off > 3072 ? 3072 : (size_t)(jpeg_size - off);
        size_t n = b64_encode(jpeg + off, chunk, enc);
        if (!http_write_all(fd, enc, n))
            return;
    }

    http_write_all(fd, img_post, sizeof(img_post) - 1);
    http_write_all(fd, "}", 1);
}

// Input result with an optional trailing screenshot ({"screenshot":true} in
// the tool arguments), saving the agent a separate screenshot round trip.
static void send_input_result_opt_screenshot(HttpRequest *req, const char *id,
                                             Result rc, const char *ok_msg,
                                             const JsonDoc *doc, int args) {
    bool want = false;
    int tok = json_obj_get(doc, args, "screenshot");
    if (tok >= 0)
        json_get_bool(doc, tok, &want);

    if (R_FAILED(rc) || !want) {
        send_input_tool_result(req->fd, id, rc, ok_msg);
        return;
    }

    int delay = get_int_or(doc, args, "screenshotDelayMs", 250);
    if (delay < 0) delay = 0;
    if (delay > INPUT_MAX_DURATION_MS) delay = INPUT_MAX_DURATION_MS;
    if (delay > 0)
        svcSleepThread((s64)delay * 1000000LL);

    const u8 *jpeg = NULL;
    u64 size = 0;
    Result cap_rc = screen_capture_jpeg(ViLayerStack_Screenshot, &jpeg, &size);
    if (R_FAILED(cap_rc)) {
        char msg[96];
        snprintf(msg, sizeof(msg), "%s (screenshot failed: rc=0x%x)", ok_msg, cap_rc);
        send_tool_ok(req->fd, id, msg);
        return;
    }
    send_tool_text_and_image(req->fd, id, ok_msg, jpeg, size);
}

// --- tools -----------------------------------------------------------------------

static void tool_screenshot(HttpRequest *req, const char *id, const JsonDoc *doc, int args) {
    ViLayerStack stack = ViLayerStack_Screenshot;
    char val[24];
    int tok = json_obj_get(doc, args, "stack");
    if (tok >= 0 && json_get_string(doc, tok, val, sizeof(val)) &&
        !screen_parse_stack(val, &stack)) {
        send_tool_error(req->fd, id, "invalid 'stack'");
        return;
    }

    const u8 *jpeg = NULL;
    u64 size = 0;
    Result rc = screen_capture_jpeg(stack, &jpeg, &size);
    if (R_FAILED(rc)) {
        char msg[64];
        snprintf(msg, sizeof(msg), "capture failed (rc=0x%x)", rc);
        send_tool_error(req->fd, id, msg);
        return;
    }

    static const char pre[] = "{\"content\":[{\"type\":\"image\",\"data\":\"";
    static const char post[] = "\",\"mimeType\":\"image/jpeg\"}],\"isError\":false}";
    char head[96];
    int hn = snprintf(head, sizeof(head), "{\"jsonrpc\":\"2.0\",\"id\":%s,\"result\":", id);
    size_t b64len = b64_encoded_len((size_t)size);
    size_t total = (size_t)hn + sizeof(pre) - 1 + b64len + sizeof(post) - 1 + 1;

    http_send_header(req->fd, 200, "application/json", total);
    http_write_all(req->fd, head, (size_t)hn);
    http_write_all(req->fd, pre, sizeof(pre) - 1);

    // Encode in multiples of 3 so chunk concatenation equals whole-buffer
    // encoding (padding only on the final chunk).
    static char enc[4096];
    for (size_t off = 0; off < size; off += 3072) {
        size_t chunk = size - off > 3072 ? 3072 : (size_t)(size - off);
        size_t n = b64_encode(jpeg + off, chunk, enc);
        if (!http_write_all(req->fd, enc, n))
            return;
    }

    http_write_all(req->fd, post, sizeof(post) - 1);
    http_write_all(req->fd, "}", 1);
}

static void tool_tap_buttons(HttpRequest *req, const char *id, const JsonDoc *doc, int args) {
    uint64_t mask;
    const char *err = NULL;
    if (!args_get_buttons(doc, args, &mask, &err)) {
        send_tool_error(req->fd, id, err);
        return;
    }
    int duration = args_get_duration(doc, args, INPUT_DEFAULT_TAP_MS);
    send_input_result_opt_screenshot(req, id, input_tap(mask, duration), "ok",
                                     doc, args);
}

static int get_int_or(const JsonDoc *doc, int obj, const char *key, int fallback) {
    long long v;
    int tok = json_obj_get(doc, obj, key);
    if (tok >= 0 && json_get_int(doc, tok, &v))
        return (int)v;
    return fallback;
}

static void tool_tap_sequence(HttpRequest *req, const char *id, const JsonDoc *doc, int args) {
    int taps = json_obj_get(doc, args, "taps");
    int n = json_arr_len(doc, taps);
    if (n <= 0) {
        send_tool_error(req->fd, id, "missing 'taps' (non-empty array)");
        return;
    }
    if (n > TAP_SEQ_MAX) {
        send_tool_error(req->fd, id, "too many taps (max 32)");
        return;
    }
    for (int i = 0; i < n; i++) {
        int el = json_arr_get(doc, taps, i);
        uint64_t mask;
        const char *err = NULL;
        if (!args_get_buttons(doc, el, &mask, &err)) {
            char msg[96];
            snprintf(msg, sizeof(msg), "step %d: %s", i, err);
            send_tool_error(req->fd, id, msg);
            return;
        }
        int duration = args_get_duration(doc, el, INPUT_DEFAULT_TAP_MS);
        Result rc = input_tap(mask, duration);
        if (R_FAILED(rc)) {
            char msg[96];
            snprintf(msg, sizeof(msg), "step %d: input failed (rc=0x%x)", i, rc);
            send_tool_error(req->fd, id, msg);
            return;
        }
        if (i + 1 < n) {
            int delay = get_int_or(doc, el, "delayAfterMs", 150);
            if (delay < 0) delay = 0;
            if (delay > INPUT_MAX_DURATION_MS) delay = INPUT_MAX_DURATION_MS;
            svcSleepThread((s64)delay * 1000000LL);
        }
    }
    char msg[48];
    snprintf(msg, sizeof(msg), "performed %d taps", n);
    send_input_result_opt_screenshot(req, id, 0, msg, doc, args);
}

static void tool_hold_release(HttpRequest *req, const char *id, const JsonDoc *doc,
                              int args, bool hold) {
    uint64_t mask;
    const char *err = NULL;
    if (!args_get_buttons(doc, args, &mask, &err)) {
        send_tool_error(req->fd, id, err);
        return;
    }
    send_input_result_opt_screenshot(req, id,
                                     hold ? input_hold(mask) : input_release(mask),
                                     "ok", doc, args);
}

static void tool_set_stick(HttpRequest *req, const char *id, const JsonDoc *doc, int args) {
    int side, duration;
    float x, y;
    const char *err = NULL;
    if (!args_get_stick(doc, args, &side, &x, &y, &duration, &err)) {
        send_tool_error(req->fd, id, err);
        return;
    }
    send_input_result_opt_screenshot(req, id, input_stick(side, x, y, duration),
                                     "ok", doc, args);
}

static void tool_status(HttpRequest *req, const char *id) {
    u32 ver = hosversionGet();
    char text[192];
    snprintf(text, sizeof(text),
             "version: %s\nfirmware: %u.%u.%u\ncontrollerAttached: %s\nuptimeSeconds: %llu",
             routes_app_version(),
             HOSVER_MAJOR(ver), HOSVER_MINOR(ver), HOSVER_MICRO(ver),
             input_is_attached() ? "true" : "false",
             (unsigned long long)routes_uptime_seconds());
    send_tool_ok(req->fd, id, text);
}

// Extracts the "path" argument and resolves it. Sends a tool error on failure.
static bool get_path_arg(HttpRequest *req, const char *id, const JsonDoc *doc,
                         int args, char *fspath, size_t fspath_sz) {
    char path[512];
    int tok = json_obj_get(doc, args, "path");
    if (tok < 0 || !json_get_string(doc, tok, path, sizeof(path))) {
        send_tool_error(req->fd, id, "missing 'path'");
        return false;
    }
    const char *err = NULL;
    if (!files_resolve(path, fspath, fspath_sz, &err)) {
        send_tool_error(req->fd, id, err);
        return false;
    }
    return true;
}

static void tool_list_directory(HttpRequest *req, const char *id, const JsonDoc *doc, int args) {
    char fspath[768];
    if (!get_path_arg(req, id, doc, args, fspath, sizeof(fspath)))
        return;

    // Trim a trailing slash (but keep the root "sdmc:/").
    size_t plen = strlen(fspath);
    size_t rootlen = strlen(FILES_ROOT) + 1;
    if (plen > rootlen && fspath[plen - 1] == '/')
        fspath[plen - 1] = '\0';

    const char *err = NULL;
    size_t len = 0;
    char *json = files_build_listing(fspath, fspath + strlen(FILES_ROOT), &len, &err);
    if (!json) {
        send_tool_error(req->fd, id, err);
        return;
    }
    size_t esc_len = json_escaped_len(json, len);
    char *escaped = malloc(esc_len + 1);
    if (!escaped) {
        free(json);
        send_rpc_error(req->fd, id, -32603, "out of memory");
        return;
    }
    json_escape(json, len, escaped, esc_len + 1);
    free(json);
    send_result_stream(req->fd, id, TEXT_PRE, escaped, esc_len, TEXT_POST_OK);
    free(escaped);
}

// Replaces invalid UTF-8 sequences with '?' so the output is always a legal
// JSON string payload.
static void sanitize_utf8(char *buf, size_t len) {
    size_t i = 0;
    while (i < len) {
        unsigned char c = (unsigned char)buf[i];
        size_t extra;
        if (c < 0x80) { i++; continue; }
        else if ((c & 0xE0) == 0xC0) extra = 1;
        else if ((c & 0xF0) == 0xE0) extra = 2;
        else if ((c & 0xF8) == 0xF0) extra = 3;
        else { buf[i++] = '?'; continue; }

        bool ok = i + extra < len; // full sequence present
        for (size_t k = 1; ok && k <= extra; k++)
            ok = ((unsigned char)buf[i + k] & 0xC0) == 0x80;
        if (ok)
            i += extra + 1;
        else
            buf[i++] = '?';
    }
}

static void tool_read_file(HttpRequest *req, const char *id, const JsonDoc *doc, int args) {
    char fspath[768];
    if (!get_path_arg(req, id, doc, args, fspath, sizeof(fspath)))
        return;

    struct stat st;
    if (stat(fspath, &st) != 0 || S_ISDIR(st.st_mode)) {
        send_tool_error(req->fd, id, "no such file");
        return;
    }

    long long fsize = (long long)st.st_size;
    long long offset = get_int_or(doc, args, "offset", 0);
    long long length = get_int_or(doc, args, "length", READ_FILE_MAX);
    if (offset < 0) {
        offset = fsize + offset;
        if (offset < 0)
            offset = 0;
    }
    if (offset > fsize)
        offset = fsize;
    long long avail = fsize - offset;
    if (length < 0 || length > avail)
        length = avail;
    if (length > READ_FILE_MAX)
        length = READ_FILE_MAX;

    char *data = malloc((size_t)length + 1);
    if (!data) {
        send_rpc_error(req->fd, id, -32603, "out of memory");
        return;
    }
    FILE *f = fopen(fspath, "rb");
    if (!f) {
        free(data);
        send_tool_error(req->fd, id, "open failed");
        return;
    }
    if (offset > 0)
        fseek(f, (long)offset, SEEK_SET);
    size_t got = fread(data, 1, (size_t)length, f);
    fclose(f);

    sanitize_utf8(data, got);

    size_t esc_len = json_escaped_len(data, got);
    char *escaped = malloc(esc_len + 1);
    if (!escaped) {
        free(data);
        send_rpc_error(req->fd, id, -32603, "out of memory");
        return;
    }
    json_escape(data, got, escaped, esc_len + 1);
    free(data);
    send_result_stream(req->fd, id, TEXT_PRE, escaped, esc_len, TEXT_POST_OK);
    free(escaped);
}

static void tool_upload_file(HttpRequest *req, const char *id, const JsonDoc *doc,
                             int args, bool content_streamed) {
    char fspath[768];
    if (!get_path_arg(req, id, doc, args, fspath, sizeof(fspath)))
        return;

    if (!content_streamed) {
        send_tool_error(req->fd, id, "missing 'content' (base64 string)");
        return;
    }
    if (g_upload.failed || !g_upload.f || !b64dec_finish(&g_upload.dec)) {
        send_tool_error(req->fd, id, "invalid base64 content");
        return;
    }
    fclose(g_upload.f);
    g_upload.f = NULL;

    files_mkdirs_for(fspath);
    remove(fspath); // rename() does not overwrite on all newlib targets
    if (rename(MCP_UPLOAD_TMP, fspath) != 0) {
        send_tool_error(req->fd, id, "failed to move upload into place");
        return;
    }

    char msg[820];
    snprintf(msg, sizeof(msg), "wrote %zu bytes to %s", g_upload.bytes,
             fspath + strlen(FILES_ROOT));
    memset(&g_upload, 0, sizeof(g_upload));
    LOGF("mcp: %s\n", msg);
    send_tool_ok(req->fd, id, msg);
}

static void tool_delete_file(HttpRequest *req, const char *id, const JsonDoc *doc, int args) {
    char fspath[768];
    if (!get_path_arg(req, id, doc, args, fspath, sizeof(fspath)))
        return;
    const char *err = NULL;
    if (!files_delete_path(fspath, &err)) {
        send_tool_error(req->fd, id, err);
        return;
    }
    send_tool_ok(req->fd, id, "deleted");
}

// Mints a bearer token over the already-authenticated MCP channel so agents
// can use the raw HTTP API (e.g. curl for large file uploads) without being
// given credentials out of band.
static void tool_create_token(HttpRequest *req, const char *id) {
    char token[65];
    if (!oauth_mint_token(token, sizeof(token), "via create_token tool")) {
        send_tool_error(req->fd, id, "failed to persist token");
        return;
    }
    char host[160];
    snprintf(host, sizeof(host), "%s", req->host[0] ? req->host : "<switch-ip>:<port>");
    char text[640];
    snprintf(text, sizeof(text),
             "token: %s\n\n"
             "Use it as a Bearer credential with the raw HTTP API, e.g. to "
             "upload a file:\n"
             "  curl -H 'Authorization: Bearer %s' -T myapp.nro "
             "'http://%s/files?path=/switch/myapp.nro'\n\n"
             "The token does not expire; revoke it by deleting its line from "
             "config/sys-autopilot/tokens.txt on the SD card.",
             token, token, host);
    send_tool_ok(req->fd, id, text);
}

static void tool_revoke_token(HttpRequest *req, const char *id,
                              const JsonDoc *doc, int args) {
    char token[160];
    int tok = json_obj_get(doc, args, "token");
    if (tok < 0 || !json_get_string(doc, tok, token, sizeof(token))) {
        send_tool_error(req->fd, id, "missing 'token'");
        return;
    }
    if (!oauth_revoke_token(token)) {
        send_tool_error(req->fd, id,
                        "unknown token (note: the static config.ini token "
                        "cannot be revoked this way)");
        return;
    }
    LOGF("mcp: revoked a token\n");
    send_tool_ok(req->fd, id, "token revoked");
}

static void tool_power(HttpRequest *req, const char *id, PowerAction action,
                       const char *ok_msg) {
    if (!power_actions_available()) {
        send_tool_error(req->fd, id, "power control unavailable");
        return;
    }
    send_tool_ok(req->fd, id, ok_msg);
    power_schedule(action); // executed after this response is flushed
}

// --- JSON-RPC dispatch -------------------------------------------------------

static void handle_initialize(HttpRequest *req, const char *id, const JsonDoc *doc, int params) {
    // Respond with the client's protocol version when we know it; otherwise
    // the latest we support.
    char proto[24] = MCP_PROTO_LATEST;
    int vtok = json_obj_get(doc, params, "protocolVersion");
    if (vtok >= 0) {
        char v[24];
        if (json_get_string(doc, vtok, v, sizeof(v)) &&
            (strcmp(v, "2024-11-05") == 0 || strcmp(v, "2025-03-26") == 0 ||
             strcmp(v, "2025-06-18") == 0))
            snprintf(proto, sizeof(proto), "%s", v);
    }

    char val[256];
    int n = snprintf(val, sizeof(val),
                     "{\"protocolVersion\":\"%s\","
                     "\"capabilities\":{\"tools\":{}},"
                     "\"serverInfo\":{\"name\":\"sys-autopilot\",\"version\":\"%s\"}}",
                     proto, routes_app_version());
    send_rpc_value(req->fd, id, "result", val, (size_t)n);
}

static void handle_tools_call(HttpRequest *req, const char *id, const JsonDoc *doc,
                              int params, bool content_streamed) {
    char name[40] = "";
    int ntok = json_obj_get(doc, params, "name");
    if (ntok < 0 || !json_get_string(doc, ntok, name, sizeof(name))) {
        send_rpc_error(req->fd, id, -32602, "missing tool name");
        return;
    }
    int args = json_obj_get(doc, params, "arguments"); // may be -1

    if (strcmp(name, "screenshot") == 0)            tool_screenshot(req, id, doc, args);
    else if (strcmp(name, "tap_buttons") == 0)      tool_tap_buttons(req, id, doc, args);
    else if (strcmp(name, "tap_sequence") == 0)     tool_tap_sequence(req, id, doc, args);
    else if (strcmp(name, "hold_buttons") == 0)     tool_hold_release(req, id, doc, args, true);
    else if (strcmp(name, "release_buttons") == 0)  tool_hold_release(req, id, doc, args, false);
    else if (strcmp(name, "set_stick") == 0)        tool_set_stick(req, id, doc, args);
    else if (strcmp(name, "clear_input") == 0)
        send_input_result_opt_screenshot(req, id, input_clear(), "ok", doc, args);
    else if (strcmp(name, "status") == 0)           tool_status(req, id);
    else if (strcmp(name, "list_directory") == 0)   tool_list_directory(req, id, doc, args);
    else if (strcmp(name, "read_file") == 0)        tool_read_file(req, id, doc, args);
    else if (strcmp(name, "upload_file") == 0)      tool_upload_file(req, id, doc, args, content_streamed);
    else if (strcmp(name, "delete_file") == 0)      tool_delete_file(req, id, doc, args);
    else if (strcmp(name, "create_token") == 0)     tool_create_token(req, id);
    else if (strcmp(name, "revoke_token") == 0)     tool_revoke_token(req, id, doc, args);
    else if (strcmp(name, "sleep") == 0)
        tool_power(req, id, PowerAction_Sleep,
                   "entering sleep mode; the server will be unreachable until "
                   "a human wakes the console");
    else if (strcmp(name, "restart") == 0)
        tool_power(req, id, PowerAction_Restart,
                   "rebooting; the server returns once the console boots back "
                   "into CFW (bootloader menus may require human intervention)");
    else if (strcmp(name, "power_off") == 0)
        tool_power(req, id, PowerAction_PowerOff,
                   "powering off; a human must press the power button to turn "
                   "the console back on");
    else send_rpc_error(req->fd, id, -32602, "unknown tool");
}

void mcp_handle_post(HttpRequest *req) {
    static char doc_buf[MCP_DOC_MAX];
    static char chunk[0x8000];
    static JsonDoc doc;

    if (!req->has_content_length) {
        http_send_error(req->fd, 411, "Content-Length required");
        return;
    }

    memset(&g_upload, 0, sizeof(g_upload));
    b64dec_init(&g_upload.dec);

    Jstream js;
    jstream_init(&js, doc_buf, sizeof(doc_buf), upload_sink, &g_upload);

    ssize_t n;
    int jerr = 0;
    while ((n = http_read_body(req, chunk, sizeof(chunk))) > 0) {
        jerr = jstream_feed(&js, chunk, (size_t)n);
        if (jerr)
            break;
    }
    bool content_streamed = false;
    if (!jerr && n >= 0)
        jerr = jstream_finish(&js, &content_streamed);

    if (jerr || n < 0) {
        upload_cleanup();
        const char *msg = (jerr == JSTREAM_EDOC) ? "request too large"
                                                 : "parse error";
        send_rpc_error(req->fd, "null", -32700, msg);
        return;
    }

    if (json_parse(&doc, doc_buf, js.doc_len) != 0 || doc.ntok < 1 ||
        doc.tok[0].type != JSMN_OBJECT) {
        upload_cleanup();
        send_rpc_error(req->fd, "null", -32700, "parse error");
        return;
    }

    // id: echoed verbatim (number or string); absent => notification.
    char id[80] = "null";
    bool has_id = false;
    int id_tok = json_obj_get(&doc, 0, "id");
    if (id_tok >= 0 && json_raw(&doc, id_tok, id, sizeof(id)))
        has_id = true;

    char method[48] = "";
    int mtok = json_obj_get(&doc, 0, "method");
    if (mtok < 0 || !json_get_string(&doc, mtok, method, sizeof(method))) {
        upload_cleanup();
        send_rpc_error(req->fd, id, -32600, "missing method");
        return;
    }

    int params = json_obj_get(&doc, 0, "params"); // may be -1

    LOGF("mcp: %s\n", method);

    if (!has_id || strncmp(method, "notifications/", 14) == 0) {
        // Notification: accept and discard.
        upload_cleanup();
        http_send_header(req->fd, 202, "text/plain", 0);
        return;
    }

    if (strcmp(method, "initialize") == 0) {
        handle_initialize(req, id, &doc, params);
    } else if (strcmp(method, "ping") == 0) {
        send_rpc_value(req->fd, id, "result", "{}", 2);
    } else if (strcmp(method, "tools/list") == 0) {
        send_rpc_value(req->fd, id, "result", kMcpToolsJson, strlen(kMcpToolsJson));
    } else if (strcmp(method, "tools/call") == 0) {
        if (params < 0)
            send_rpc_error(req->fd, id, -32602, "missing params");
        else
            handle_tools_call(req, id, &doc, params, content_streamed);
    } else {
        send_rpc_error(req->fd, id, -32601, "method not found");
    }

    // Whatever happened, no upload temp state may survive the request.
    upload_cleanup();
}
