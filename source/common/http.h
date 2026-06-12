#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <sys/types.h>

#define HTTP_MAX_HEADER 8192

typedef struct {
    int    fd;
    char   method[8];
    char   path[512];
    char   query[2048];
    char   host[128];           // Value of the Host header ("" if absent)
    char   auth[256];           // Value of the Authorization header ("" if absent)
    size_t content_length;
    bool   has_content_length;
    bool   expect_100;          // Client sent "Expect: 100-continue"
    bool   sent_100;
    // Body bytes that were read together with the headers.
    const char *body_leftover;
    size_t      body_leftover_len;
    size_t      body_consumed;  // Total body bytes consumed so far
    char   buf[HTTP_MAX_HEADER];
} HttpRequest;

// Optional callback invoked while waiting on socket I/O (~every 100ms).
// Return false to abort the transfer (used for clean shutdown).
typedef bool (*HttpIdleCb)(void);
void http_set_idle_callback(HttpIdleCb cb);

// Reads and parses a request's start-line + headers from fd.
// Sockets may be non-blocking: all I/O internally waits via poll() with a
// 10s inactivity timeout.
// Returns false on malformed input / connection error.
bool http_read_request(int fd, HttpRequest *req);

// Looks up a key in a URL-encoded parameter string ("a=1&b=2"), decoding the
// value. Works for both query strings and form-urlencoded bodies.
// Returns false if the key is not present.
bool http_param_get(const char *params, const char *key, char *out, size_t outsz);

// http_param_get over the request's query string.
bool http_query_get(const HttpRequest *req, const char *key, char *out, size_t outsz);

// Reads up to len body bytes (consuming the leftover from the header read
// first). Sends "100 Continue" when the client requested it. Returns the
// number of bytes read, 0 on clean end, -1 on error.
ssize_t http_read_body(HttpRequest *req, void *buf, size_t len);

// Constant-time string equality (for credentials/tokens).
bool http_secure_streq(const char *a, const char *b);

// Validate the Authorization header against expected credentials
// (constant-time comparisons).
bool http_check_basic_auth(const HttpRequest *req, const char *user, const char *pass);
bool http_check_bearer_auth(const HttpRequest *req, const char *token);

// Extracts the value of a "Bearer" Authorization header. Returns false when
// the header is absent or uses a different scheme.
bool http_get_bearer(const HttpRequest *req, char *out, size_t outsz);

// Response helpers. All use "Connection: close" semantics.
void http_send_header(int fd, int code, const char *content_type, size_t content_length);
void http_send_response(int fd, int code, const char *content_type, const void *body, size_t len);
void http_send_json(int fd, int code, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
void http_send_error(int fd, int code, const char *msg);
void http_send_unauthorized(const HttpRequest *req, bool offer_basic, bool offer_bearer);

// 302 redirect. location must not contain CR/LF.
void http_send_redirect(int fd, const char *location);

// Writes all of buf to fd, handling short writes. Returns false on error.
bool http_write_all(int fd, const void *buf, size_t len);
