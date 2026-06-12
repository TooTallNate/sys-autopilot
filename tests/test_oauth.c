// Host tests for the OAuth flow: crypto vectors, discovery, login, PKCE
// token exchange, token persistence/revocation.
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>

#include "base64.h"
#include "config.h"
#include "http.h"
#include "oauth.h"
#include "sha256.h"

static Config g_test_cfg;

// Sends one HTTP request through a socketpair to `handler`; returns the raw
// response in a static buffer.
typedef void (*Handler)(HttpRequest *req);

static const char *do_req(Handler handler, const char *method, const char *target,
                          const char *host, const char *body) {
    static char resp[64 * 1024];
    int sv[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    // The handler writes the entire response before the test reads it back;
    // grow the buffers so large payloads (tools/list, screenshots) don't
    // deadlock the single-threaded harness.
    int bufsz = 512 * 1024;
    setsockopt(sv[0], SOL_SOCKET, SO_SNDBUF, &bufsz, sizeof(bufsz));
    setsockopt(sv[1], SOL_SOCKET, SO_RCVBUF, &bufsz, sizeof(bufsz));

    char req[8192];
    int rn;
    if (body) {
        rn = snprintf(req, sizeof(req),
                      "%s %s HTTP/1.1\r\nHost: %s\r\n"
                      "Content-Type: application/x-www-form-urlencoded\r\n"
                      "Content-Length: %zu\r\n\r\n%s",
                      method, target, host, strlen(body), body);
    } else {
        rn = snprintf(req, sizeof(req), "%s %s HTTP/1.1\r\nHost: %s\r\n\r\n",
                      method, target, host);
    }
    assert(rn > 0 && write(sv[1], req, (size_t)rn) == rn);
    shutdown(sv[1], SHUT_WR);

    static HttpRequest hreq;
    assert(http_read_request(sv[0], &hreq));
    handler(&hreq);
    close(sv[0]);

    size_t total = 0;
    ssize_t n;
    while ((n = read(sv[1], resp + total, sizeof(resp) - 1 - total)) > 0)
        total += (size_t)n;
    resp[total] = '\0';
    close(sv[1]);
    return resp;
}

static void test_sha256_vectors(void) {
    uint8_t d[32];
    char hex[65];
    static const char *hexc = "0123456789abcdef";

    sha256_hash(d, "", 0);
    for (int i = 0; i < 32; i++) { hex[i*2] = hexc[d[i]>>4]; hex[i*2+1] = hexc[d[i]&0xF]; }
    hex[64] = '\0';
    assert(strcmp(hex, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855") == 0);

    sha256_hash(d, "abc", 3);
    for (int i = 0; i < 32; i++) { hex[i*2] = hexc[d[i]>>4]; hex[i*2+1] = hexc[d[i]&0xF]; }
    hex[64] = '\0';
    assert(strcmp(hex, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad") == 0);

    // Multi-block input (exercises padding across block boundary).
    char longmsg[200];
    memset(longmsg, 'a', sizeof(longmsg));
    sha256_hash(d, longmsg, 120);

    // RFC 7636 appendix B PKCE vector.
    const char *verifier = "dBjftJeZ4CVP-mB92K27uhbUJU1p1r_wW1gFWFOEjXk";
    sha256_hash(d, verifier, strlen(verifier));
    char challenge[48];
    b64url_encode(d, 32, challenge);
    assert(strcmp(challenge, "E9Melhoa2OwvFrEMTJguCHaoeK1t8URWbuGJSstw-cM") == 0);

    printf("sha256/pkce vectors ok\n");
}

static void test_discovery(void) {
    const char *r = do_req(oauth_handle_protected_resource, "GET",
                           "/.well-known/oauth-protected-resource",
                           "10.0.0.5:4150", NULL);
    assert(strstr(r, "HTTP/1.1 200"));
    assert(strstr(r, "\"resource\":\"http://10.0.0.5:4150/mcp\""));
    assert(strstr(r, "\"authorization_servers\":[\"http://10.0.0.5:4150\"]"));

    r = do_req(oauth_handle_as_metadata, "GET",
               "/.well-known/oauth-authorization-server", "10.0.0.5:4150", NULL);
    assert(strstr(r, "\"authorization_endpoint\":\"http://10.0.0.5:4150/oauth/authorize\""));
    assert(strstr(r, "\"token_endpoint\":\"http://10.0.0.5:4150/oauth/token\""));
    assert(strstr(r, "\"registration_endpoint\":\"http://10.0.0.5:4150/oauth/register\""));
    assert(strstr(r, "\"code_challenge_methods_supported\":[\"S256\"]"));
    printf("discovery ok\n");
}

static void test_register(void) {
    const char *r = do_req(oauth_handle_register, "POST", "/oauth/register",
                           "10.0.0.5:4150",
                           "{\"client_name\":\"test\",\"redirect_uris\":[\"http://localhost:33418/cb\"]}");
    assert(strstr(r, "HTTP/1.1 201"));
    assert(strstr(r, "\"client_id\":\"sys-autopilot\""));
    assert(strstr(r, "\"redirect_uris\":[\"http://localhost:33418/cb\"]"));
    assert(strstr(r, "\"token_endpoint_auth_method\":\"none\""));
    printf("register ok\n");
}

static void test_authorize_form(void) {
    // XSS hygiene: params must come back HTML-escaped in hidden fields.
    const char *r = do_req(oauth_handle_authorize_get, "GET",
                           "/oauth/authorize?redirect_uri=http%3A%2F%2Flocalhost%3A33418%2Fcb"
                           "&state=st%22%3E%3Cscript%3E&code_challenge=abc123"
                           "&code_challenge_method=S256&client_id=sys-autopilot",
                           "10.0.0.5:4150", NULL);
    assert(strstr(r, "HTTP/1.1 200"));
    assert(strstr(r, "text/html"));
    assert(strstr(r, "name=\"username\""));
    assert(strstr(r, "value=\"st&quot;&gt;&lt;script&gt;"));
    assert(!strstr(r, "st\"><script>"));

    // Missing PKCE challenge -> rejected.
    r = do_req(oauth_handle_authorize_get, "GET",
               "/oauth/authorize?redirect_uri=http%3A%2F%2Flocalhost%2Fcb",
               "10.0.0.5:4150", NULL);
    assert(strstr(r, "HTTP/1.1 400"));

    // Missing redirect_uri -> rejected.
    r = do_req(oauth_handle_authorize_get, "GET",
               "/oauth/authorize?code_challenge=abc", "10.0.0.5:4150", NULL);
    assert(strstr(r, "HTTP/1.1 400"));
    printf("authorize form ok\n");
}

static void test_no_creds_configured(void) {
    Config empty = { .port = 4150 };
    oauth_init(&empty);
    const char *r = do_req(oauth_handle_authorize_get, "GET",
                           "/oauth/authorize?redirect_uri=http%3A%2F%2Fl%2Fcb&code_challenge=x",
                           "h:1", NULL);
    assert(strstr(r, "HTTP/1.1 403"));
    assert(strstr(r, "No credentials configured"));
    oauth_init(&g_test_cfg); // restore
    printf("no-creds page ok\n");
}

// Extracts the value of the Location header from a raw response.
static bool get_location(const char *resp, char *out, size_t outsz) {
    const char *loc = strstr(resp, "Location: ");
    if (!loc)
        return false;
    loc += 10;
    size_t n = strcspn(loc, "\r\n");
    if (n + 1 > outsz)
        return false;
    memcpy(out, loc, n);
    out[n] = '\0';
    return true;
}

static void test_full_flow(void) {
    // PKCE setup (client side).
    const char *verifier = "test-verifier-0123456789-0123456789-0123456789";
    uint8_t digest[32];
    sha256_hash(digest, verifier, strlen(verifier));
    char challenge[48];
    b64url_encode(digest, 32, challenge);

    // 1. Wrong password re-renders the form, issues no code.
    char body[1024];
    snprintf(body, sizeof(body),
             "redirect_uri=http%%3A%%2F%%2Flocalhost%%3A33418%%2Fcb&state=xyz"
             "&code_challenge=%s&username=admin&password=wrong", challenge);
    const char *r = do_req(oauth_handle_authorize_post, "POST", "/oauth/authorize",
                           "10.0.0.5:4150", body);
    assert(strstr(r, "HTTP/1.1 200"));
    assert(strstr(r, "Incorrect username or password"));
    assert(!strstr(r, "Location:"));

    // 2. Correct credentials -> 302 with code + state.
    snprintf(body, sizeof(body),
             "redirect_uri=http%%3A%%2F%%2Flocalhost%%3A33418%%2Fcb&state=xyz"
             "&code_challenge=%s&username=admin&password=hunter2", challenge);
    r = do_req(oauth_handle_authorize_post, "POST", "/oauth/authorize",
               "10.0.0.5:4150", body);
    assert(strstr(r, "HTTP/1.1 302"));
    char location[1024];
    assert(get_location(r, location, sizeof(location)));
    assert(strncmp(location, "http://localhost:33418/cb?code=", 31) == 0);
    assert(strstr(location, "&state=xyz"));

    char code[64];
    snprintf(code, sizeof(code), "%.*s", (int)strcspn(location + 31, "&"), location + 31);
    assert(strlen(code) == 32);

    // 3. Token exchange with a WRONG verifier fails.
    snprintf(body, sizeof(body),
             "grant_type=authorization_code&code=%s&code_verifier=wrong-verifier"
             "&redirect_uri=http%%3A%%2F%%2Flocalhost%%3A33418%%2Fcb", code);
    r = do_req(oauth_handle_token, "POST", "/oauth/token", "10.0.0.5:4150", body);
    assert(strstr(r, "invalid_grant"));

    // The code is single-use: even the right verifier must now fail.
    snprintf(body, sizeof(body),
             "grant_type=authorization_code&code=%s&code_verifier=%s"
             "&redirect_uri=http%%3A%%2F%%2Flocalhost%%3A33418%%2Fcb", code, verifier);
    r = do_req(oauth_handle_token, "POST", "/oauth/token", "10.0.0.5:4150", body);
    assert(strstr(r, "invalid_grant"));

    // 4. Fresh login -> valid exchange -> bearer token.
    snprintf(body, sizeof(body),
             "redirect_uri=http%%3A%%2F%%2Flocalhost%%3A33418%%2Fcb"
             "&code_challenge=%s&username=admin&password=hunter2", challenge);
    r = do_req(oauth_handle_authorize_post, "POST", "/oauth/authorize",
               "10.0.0.5:4150", body);
    assert(get_location(r, location, sizeof(location)));
    snprintf(code, sizeof(code), "%.*s", (int)strcspn(location + 31, "&"), location + 31);

    snprintf(body, sizeof(body),
             "grant_type=authorization_code&code=%s&code_verifier=%s"
             "&redirect_uri=http%%3A%%2F%%2Flocalhost%%3A33418%%2Fcb", code, verifier);
    r = do_req(oauth_handle_token, "POST", "/oauth/token", "10.0.0.5:4150", body);
    assert(strstr(r, "HTTP/1.1 200"));
    assert(strstr(r, "\"token_type\":\"Bearer\""));

    const char *tok = strstr(r, "\"access_token\":\"");
    assert(tok);
    tok += 16;
    char token[80];
    snprintf(token, sizeof(token), "%.*s", (int)strcspn(tok, "\""), tok);
    assert(strlen(token) == 64);

    // 5. Token is valid, persisted, and survives a reload.
    assert(oauth_token_valid(token));
    assert(!oauth_token_valid("0000000000000000000000000000000000000000000000000000000000000000"));

    FILE *f = fopen(OAUTH_TOKENS_PATH, "rb");
    assert(f);
    char file[4096] = {0};
    fread(file, 1, sizeof(file) - 1, f);
    fclose(f);
    assert(strstr(file, token));

    oauth_init(&g_test_cfg); // simulates reboot: reload from file
    assert(oauth_token_valid(token));

    // 6. Revocation: remove the line -> token rejected (mtime-based reload).
    remove(OAUTH_TOKENS_PATH);
    struct timespec ts = { .tv_sec = 0, .tv_nsec = 50000000 };
    nanosleep(&ts, NULL);
    assert(!oauth_token_valid(token));

    printf("full flow ok\n");
}

int main(void) {
    remove(OAUTH_TOKENS_PATH);
    snprintf(g_test_cfg.username, sizeof(g_test_cfg.username), "admin");
    snprintf(g_test_cfg.password, sizeof(g_test_cfg.password), "hunter2");
    g_test_cfg.port = 4150;
    oauth_init(&g_test_cfg);

    test_sha256_vectors();
    test_discovery();
    test_register();
    test_authorize_form();
    test_no_creds_configured();
    test_full_flow();
    printf("all oauth tests passed\n");
    return 0;
}
