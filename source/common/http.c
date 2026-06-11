#include "http.h"
#include "log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <ctype.h>
#include <errno.h>
#include <unistd.h>
#include <sys/socket.h>

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
        if (n <= 0) {
            if (n < 0 && errno == EINTR)
                continue;
            return false;
        }
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
        ssize_t n = recv(fd, req->buf + total, sizeof(req->buf) - 1 - total, 0);
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
bool http_query_get(const HttpRequest *req, const char *key, char *out, size_t outsz) {
    size_t klen = strlen(key);
    const char *p = req->query;
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

    ssize_t n;
    do {
        n = recv(req->fd, buf, len, 0);
    } while (n < 0 && errno == EINTR);
    if (n > 0)
        req->body_consumed += (size_t)n;
    return n;
}

// --- Basic auth -------------------------------------------------------------

static int b64_val(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

static size_t b64_decode(const char *in, char *out, size_t outsz) {
    size_t o = 0;
    int acc = 0, bits = 0;
    for (; *in && *in != '='; in++) {
        int v = b64_val(*in);
        if (v < 0)
            return 0;
        acc = (acc << 6) | v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (o + 1 >= outsz)
                return 0;
            out[o++] = (char)((acc >> bits) & 0xFF);
        }
    }
    out[o] = '\0';
    return o;
}

bool http_check_basic_auth(const HttpRequest *req, const char *user, const char *pass) {
    if (strncasecmp(req->auth, "Basic ", 6) != 0)
        return false;

    char decoded[160];
    if (b64_decode(req->auth + 6, decoded, sizeof(decoded)) == 0)
        return false;

    char expected[160];
    int n = snprintf(expected, sizeof(expected), "%s:%s", user, pass);
    if (n < 0 || (size_t)n >= sizeof(expected))
        return false;

    // Constant-time comparison (length difference still folds into diff).
    size_t dlen = strlen(decoded), elen = strlen(expected);
    unsigned char diff = (unsigned char)(dlen ^ elen);
    size_t max = dlen > elen ? dlen : elen;
    for (size_t i = 0; i < max; i++) {
        unsigned char a = i < dlen ? (unsigned char)decoded[i] : 0;
        unsigned char b = i < elen ? (unsigned char)expected[i] : 0;
        diff |= (unsigned char)(a ^ b);
    }
    return diff == 0;
}

// --- Responses ---------------------------------------------------------------

void http_send_header(int fd, int code, const char *content_type, size_t content_length) {
    char hdr[512];
    int n = snprintf(hdr, sizeof(hdr),
                     "HTTP/1.1 %d %s\r\n"
                     "Server: sys-autopilot\r\n"
                     "Content-Type: %s\r\n"
                     "Content-Length: %zu\r\n"
                     "Connection: close\r\n"
                     "\r\n",
                     code, status_reason(code), content_type, content_length);
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

void http_send_unauthorized(int fd) {
    const char *body = "{\"error\":\"unauthorized\"}";
    char hdr[512];
    int n = snprintf(hdr, sizeof(hdr),
                     "HTTP/1.1 401 Unauthorized\r\n"
                     "Server: sys-autopilot\r\n"
                     "WWW-Authenticate: Basic realm=\"sys-autopilot\"\r\n"
                     "Content-Type: application/json\r\n"
                     "Content-Length: %zu\r\n"
                     "Connection: close\r\n"
                     "\r\n",
                     strlen(body));
    http_write_all(fd, hdr, (size_t)n);
    http_write_all(fd, body, strlen(body));
}
