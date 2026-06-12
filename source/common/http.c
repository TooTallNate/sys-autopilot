#include "http.h"
#include "base64.h"
#include "log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <ctype.h>
#include <errno.h>
#include <unistd.h>
#include <poll.h>
#include <sys/socket.h>

// Abort a connection after this much I/O inactivity.
#define HTTP_IO_TIMEOUT_MS 10000
// Poll in short slices so the idle callback keeps running during transfers.
#define HTTP_POLL_SLICE_MS 100

static HttpIdleCb g_idle_cb;

void http_set_idle_callback(HttpIdleCb cb) {
    g_idle_cb = cb;
}

// Waits until fd is ready for `events`. Returns false on inactivity timeout,
// poll error, or when the idle callback requests shutdown.
static bool io_wait(int fd, short events) {
    int waited_ms = 0;
    while (waited_ms < HTTP_IO_TIMEOUT_MS) {
        if (g_idle_cb && !g_idle_cb())
            return false;
        struct pollfd pfd = { .fd = fd, .events = events };
        int pr = poll(&pfd, 1, HTTP_POLL_SLICE_MS);
        if (pr < 0) {
            if (errno == EINTR)
                continue;
            return false;
        }
        // On POLLERR/POLLHUP, return true so recv/send surfaces the error.
        if (pr > 0 && (pfd.revents & (events | POLLERR | POLLHUP)))
            return true;
        waited_ms += HTTP_POLL_SLICE_MS;
    }
    return false;
}

// recv() that tolerates non-blocking sockets: waits for readability on
// EAGAIN/EWOULDBLOCK instead of failing.
static ssize_t io_recv(int fd, void *buf, size_t len) {
    for (;;) {
        ssize_t n = recv(fd, buf, len, 0);
        if (n >= 0)
            return n;
        if (errno == EINTR)
            continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            if (!io_wait(fd, POLLIN))
                return -1;
            continue;
        }
        return -1;
    }
}

static const char *status_reason(int code) {
    switch (code) {
        case 100: return "Continue";
        case 200: return "OK";
        case 201: return "Created";
        case 204: return "No Content";
        case 400: return "Bad Request";
        case 401: return "Unauthorized";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 411: return "Length Required";
        case 413: return "Payload Too Large";
        case 500: return "Internal Server Error";
        case 507: return "Insufficient Storage";
        default:  return "Unknown";
    }
}

bool http_write_all(int fd, const void *buf, size_t len) {
    const char *p = buf;
    while (len > 0) {
        ssize_t n = send(fd, p, len, 0);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                if (!io_wait(fd, POLLOUT))
                    return false;
                continue;
            }
            return false;
        }
        if (n == 0)
            return false;
        p += n;
        len -= (size_t)n;
    }
    return true;
}

// Decodes %XX escapes and '+' (as space) in-place-safe copy from src to out.
static void url_decode(const char *src, size_t srclen, char *out, size_t outsz) {
    size_t o = 0;
    for (size_t i = 0; i < srclen && o + 1 < outsz; i++) {
        char c = src[i];
        if (c == '+') {
            out[o++] = ' ';
        } else if (c == '%' && i + 2 < srclen && isxdigit((unsigned char)src[i+1]) &&
                   isxdigit((unsigned char)src[i+2])) {
            char hex[3] = { src[i+1], src[i+2], 0 };
            out[o++] = (char)strtol(hex, NULL, 16);
            i += 2;
        } else {
            out[o++] = c;
        }
    }
    out[o] = '\0';
}

static bool header_is(const char *line, const char *name, const char **out_value) {
    size_t n = strlen(name);
    if (strncasecmp(line, name, n) != 0 || line[n] != ':')
        return false;
    const char *v = line + n + 1;
    while (*v == ' ' || *v == '\t') v++;
    *out_value = v;
    return true;
}

bool http_read_request(int fd, HttpRequest *req) {
    memset(req, 0, sizeof(*req));
    req->fd = fd;

    // Read until end of headers (or buffer full).
    size_t total = 0;
    char *hdr_end = NULL;
    while (total < sizeof(req->buf) - 1) {
        ssize_t n = io_recv(fd, req->buf + total, sizeof(req->buf) - 1 - total);
        if (n <= 0)
            return false;
        total += (size_t)n;
        req->buf[total] = '\0';
        hdr_end = strstr(req->buf, "\r\n\r\n");
        if (hdr_end)
            break;
    }
    if (!hdr_end)
        return false; // headers too large or malformed

    req->body_leftover = hdr_end + 4;
    req->body_leftover_len = total - (size_t)(req->body_leftover - req->buf);

    // Terminate the header block so string ops below stay inside it.
    *hdr_end = '\0';

    // --- Request line: METHOD SP target SP version ---
    char *line = req->buf;
    char *line_end = strstr(line, "\r\n");
    if (line_end)
        *line_end = '\0';

    char *sp1 = strchr(line, ' ');
    if (!sp1)
        return false;
    *sp1 = '\0';
    snprintf(req->method, sizeof(req->method), "%.7s", line);

    char *target = sp1 + 1;
    char *sp2 = strchr(target, ' ');
    if (!sp2)
        return false;
    *sp2 = '\0';

    char *qmark = strchr(target, '?');
    if (qmark) {
        *qmark = '\0';
        snprintf(req->query, sizeof(req->query), "%s", qmark + 1);
    }
    url_decode(target, strlen(target), req->path, sizeof(req->path));

    // --- Headers ---
    char *cursor = line_end ? line_end + 2 : hdr_end;
    while (cursor && cursor < hdr_end) {
        char *next = strstr(cursor, "\r\n");
        if (next)
            *next = '\0';

        const char *value;
        if (header_is(cursor, "Content-Length", &value)) {
            req->content_length = (size_t)strtoull(value, NULL, 10);
            req->has_content_length = true;
        } else if (header_is(cursor, "Host", &value)) {
            snprintf(req->host, sizeof(req->host), "%s", value);
        } else if (header_is(cursor, "Authorization", &value)) {
            snprintf(req->auth, sizeof(req->auth), "%s", value);
        } else if (header_is(cursor, "Expect", &value)) {
            if (strncasecmp(value, "100-continue", 12) == 0)
                req->expect_100 = true;
        }

        cursor = next ? next + 2 : NULL;
    }

    LOGF("http: %s %s%s%s\n", req->method, req->path,
         req->query[0] ? "?" : "", req->query);
    return true;
}
bool http_param_get(const char *params, const char *key, char *out, size_t outsz) {
    size_t klen = strlen(key);
    const char *p = params;
    while (*p) {
        const char *amp = strchr(p, '&');
        size_t pair_len = amp ? (size_t)(amp - p) : strlen(p);
        if (pair_len > klen && strncmp(p, key, klen) == 0 && p[klen] == '=') {
            url_decode(p + klen + 1, pair_len - klen - 1, out, outsz);
            return true;
        }
        if (pair_len == klen && strncmp(p, key, klen) == 0) {
            if (outsz) out[0] = '\0'; // key present with empty value
            return true;
        }
        if (!amp)
            break;
        p = amp + 1;
    }
    return false;
}

bool http_query_get(const HttpRequest *req, const char *key, char *out, size_t outsz) {
    return http_param_get(req->query, key, out, outsz);
}

ssize_t http_read_body(HttpRequest *req, void *buf, size_t len) {
    if (req->has_content_length) {
        size_t remaining = req->content_length - req->body_consumed;
        if (remaining == 0)
            return 0;
        if (len > remaining)
            len = remaining;
    }

    // Serve bytes that arrived with the headers first.
    if (req->body_leftover_len > 0) {
        size_t n = req->body_leftover_len < len ? req->body_leftover_len : len;
        memcpy(buf, req->body_leftover, n);
        req->body_leftover += n;
        req->body_leftover_len -= n;
        req->body_consumed += n;
        return (ssize_t)n;
    }

    if (req->expect_100 && !req->sent_100) {
        const char *cont = "HTTP/1.1 100 Continue\r\n\r\n";
        if (!http_write_all(req->fd, cont, strlen(cont)))
            return -1;
        req->sent_100 = true;
    }

    ssize_t n = io_recv(req->fd, buf, len);
    if (n > 0)
        req->body_consumed += (size_t)n;
    return n;
}

// --- Authentication -----------------------------------------------------------

// Constant-time string comparison (length difference folds into diff).
bool http_secure_streq(const char *a, const char *b) {
    size_t alen = strlen(a), blen = strlen(b);
    unsigned char diff = (unsigned char)(alen ^ blen);
    size_t max = alen > blen ? alen : blen;
    for (size_t i = 0; i < max; i++) {
        unsigned char ca = i < alen ? (unsigned char)a[i] : 0;
        unsigned char cb = i < blen ? (unsigned char)b[i] : 0;
        diff |= (unsigned char)(ca ^ cb);
    }
    return diff == 0;
}

bool http_check_basic_auth(const HttpRequest *req, const char *user, const char *pass) {
    if (strncasecmp(req->auth, "Basic ", 6) != 0)
        return false;

    char decoded[200];
    if (b64_decode(req->auth + 6, decoded, sizeof(decoded)) == 0)
        return false;

    char expected[200];
    int n = snprintf(expected, sizeof(expected), "%s:%s", user, pass);
    if (n < 0 || (size_t)n >= sizeof(expected))
        return false;

    return http_secure_streq(decoded, expected);
}

bool http_check_bearer_auth(const HttpRequest *req, const char *token) {
    if (strncasecmp(req->auth, "Bearer ", 7) != 0)
        return false;
    return http_secure_streq(req->auth + 7, token);
}

bool http_get_bearer(const HttpRequest *req, char *out, size_t outsz) {
    if (strncasecmp(req->auth, "Bearer ", 7) != 0)
        return false;
    const char *v = req->auth + 7;
    while (*v == ' ') v++;
    if (*v == '\0' || strlen(v) + 1 > outsz)
        return false;
    snprintf(out, outsz, "%s", v);
    return true;
}

// --- Responses ---------------------------------------------------------------

void http_send_header(int fd, int code, const char *content_type, size_t content_length) {
    char hdr[512];
    int n = snprintf(hdr, sizeof(hdr),
                     "HTTP/1.1 %d %s\r\n"
                     "Server: sys-autopilot\r\n"
                     "Access-Control-Allow-Origin: *\r\n"
                     "Content-Type: %s\r\n"
                     "Content-Length: %zu\r\n"
                     "Connection: close\r\n"
                     "\r\n",
                     code, status_reason(code), content_type, content_length);
    http_write_all(fd, hdr, (size_t)n);
}

void http_send_redirect(int fd, const char *location) {
    char hdr[1024];
    int n = snprintf(hdr, sizeof(hdr),
                     "HTTP/1.1 302 Found\r\n"
                     "Server: sys-autopilot\r\n"
                     "Location: %s\r\n"
                     "Content-Length: 0\r\n"
                     "Connection: close\r\n"
                     "\r\n",
                     location);
    if (n > 0 && (size_t)n < sizeof(hdr))
        http_write_all(fd, hdr, (size_t)n);
}

void http_send_response(int fd, int code, const char *content_type, const void *body, size_t len) {
    http_send_header(fd, code, content_type, len);
    if (len > 0)
        http_write_all(fd, body, len);
}

void http_send_json(int fd, int code, const char *fmt, ...) {
    char body[1024];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(body, sizeof(body), fmt, ap);
    va_end(ap);
    if (n < 0)
        n = 0;
    if ((size_t)n >= sizeof(body))
        n = sizeof(body) - 1;
    http_send_response(fd, code, "application/json", body, (size_t)n);
}

void http_send_error(int fd, int code, const char *msg) {
    http_send_json(fd, code, "{\"error\":\"%s\"}", msg);
}

void http_send_unauthorized(const HttpRequest *req, bool offer_basic, bool offer_bearer) {
    const char *body = "{\"error\":\"unauthorized\"}";
    char challenges[384];
    int cn = 0;
    if (offer_bearer) {
        if (req->host[0]) {
            // Point OAuth-capable MCP clients at the protected resource
            // metadata so they can run the browser auth flow automatically.
            cn += snprintf(challenges + cn, sizeof(challenges) - (size_t)cn,
                           "WWW-Authenticate: Bearer realm=\"sys-autopilot\", "
                           "resource_metadata=\"http://%s/.well-known/oauth-protected-resource\"\r\n",
                           req->host);
        } else {
            cn += snprintf(challenges + cn, sizeof(challenges) - (size_t)cn,
                           "WWW-Authenticate: Bearer realm=\"sys-autopilot\"\r\n");
        }
    }
    if (offer_basic)
        cn += snprintf(challenges + cn, sizeof(challenges) - (size_t)cn,
                       "WWW-Authenticate: Basic realm=\"sys-autopilot\"\r\n");
    challenges[cn] = '\0';
    char hdr[768];
    int n = snprintf(hdr, sizeof(hdr),
                     "HTTP/1.1 401 Unauthorized\r\n"
                     "Server: sys-autopilot\r\n"
                     "Access-Control-Allow-Origin: *\r\n"
                     "%s"
                     "Content-Type: application/json\r\n"
                     "Content-Length: %zu\r\n"
                     "Connection: close\r\n"
                     "\r\n",
                     challenges, strlen(body));
    http_write_all(req->fd, hdr, (size_t)n);
    http_write_all(req->fd, body, strlen(body));
}
