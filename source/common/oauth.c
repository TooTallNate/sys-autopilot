#include "oauth.h"
#include "base64.h"
#include "json.h"
#include "log.h"
#include "sha256.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>

#ifdef __SWITCH__
#include <switch.h>
#else
#include <unistd.h>
#include <sys/random.h>
#endif

#define OAUTH_CLIENT_ID "sys-autopilot"
#define CODE_LIFETIME_SECS 300
#define MAX_AUTH_CODES 8
#define MAX_TOKENS 32
#define TOKEN_HEX_LEN 64 // 32 random bytes as hex

static const Config *g_cfg;

// --- small utilities -----------------------------------------------------------

static void fill_random(uint8_t *buf, size_t len) {
#ifdef __SWITCH__
    randomGet(buf, len);
#else
    // getentropy() is limited to 256 bytes per call; our needs are smaller.
    if (getentropy(buf, len) != 0)
        abort();
#endif
}

static uint64_t now_secs(void) {
#ifdef __SWITCH__
    return armTicksToNs(armGetSystemTick()) / 1000000000ULL;
#else
    return (uint64_t)time(NULL);
#endif
}

static void to_hex(const uint8_t *in, size_t n, char *out) {
    static const char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) {
        out[i * 2] = hex[in[i] >> 4];
        out[i * 2 + 1] = hex[in[i] & 0xF];
    }
    out[n * 2] = '\0';
}

// Appends src with HTML escaping (for embedding untrusted values in the
// login form). Returns false on overflow.
static bool html_escape_append(char *buf, size_t cap, size_t *len, const char *src) {
    for (const char *p = src; *p; p++) {
        const char *rep;
        char single[2] = { *p, 0 };
        switch (*p) {
            case '&':  rep = "&amp;";  break;
            case '<':  rep = "&lt;";   break;
            case '>':  rep = "&gt;";   break;
            case '"':  rep = "&quot;"; break;
            case '\'': rep = "&#39;";  break;
            default:   rep = single;   break;
        }
        size_t rlen = strlen(rep);
        if (*len + rlen + 1 > cap)
            return false;
        memcpy(buf + *len, rep, rlen);
        *len += rlen;
    }
    buf[*len] = '\0';
    return true;
}

// URL-encodes src (for redirect query params). Returns false on overflow.
static bool url_encode_append(char *buf, size_t cap, size_t *len, const char *src) {
    static const char hex[] = "0123456789ABCDEF";
    for (const char *p = src; *p; p++) {
        unsigned char c = (unsigned char)*p;
        bool safe = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                    (c >= '0' && c <= '9') || c == '-' || c == '.' ||
                    c == '_' || c == '~';
        if (*len + 4 > cap)
            return false;
        if (safe) {
            buf[(*len)++] = (char)c;
        } else {
            buf[(*len)++] = '%';
            buf[(*len)++] = hex[c >> 4];
            buf[(*len)++] = hex[c & 0xF];
        }
    }
    buf[*len] = '\0';
    return true;
}

// Reads a full (small) request body into buf. Returns length or -1 (after
// sending an error response).
static int read_small_body(HttpRequest *req, char *buf, size_t cap) {
    if (!req->has_content_length) {
        http_send_error(req->fd, 411, "Content-Length required");
        return -1;
    }
    if (req->content_length >= cap) {
        http_send_error(req->fd, 413, "request body too large");
        return -1;
    }
    size_t total = 0;
    ssize_t n;
    while (total < req->content_length &&
           (n = http_read_body(req, buf + total, cap - 1 - total)) > 0)
        total += (size_t)n;
    buf[total] = '\0';
    if (total != req->content_length) {
        http_send_error(req->fd, 400, "incomplete body");
        return -1;
    }
    return (int)total;
}

// --- persisted token store -------------------------------------------------------

static char g_tokens[MAX_TOKENS][TOKEN_HEX_LEN + 1];
static int g_token_count;
static time_t g_tokens_mtime;
static bool g_tokens_loaded;

static void tokens_load(void) {
    g_token_count = 0;
    g_tokens_loaded = true;

    struct stat st;
    if (stat(OAUTH_TOKENS_PATH, &st) == 0)
        g_tokens_mtime = st.st_mtime;

    FILE *f = fopen(OAUTH_TOKENS_PATH, "rb");
    if (!f)
        return;
    char line[256];
    while (g_token_count < MAX_TOKENS && fgets(line, sizeof(line), f)) {
        // First whitespace-delimited field is the token; rest is comment.
        char *tok = line;
        while (*tok == ' ' || *tok == '\t') tok++;
        size_t len = strcspn(tok, " \t\r\n#");
        if (len < 16 || len > TOKEN_HEX_LEN)
            continue;
        memcpy(g_tokens[g_token_count], tok, len);
        g_tokens[g_token_count][len] = '\0';
        g_token_count++;
    }
    fclose(f);
    LOGF("oauth: loaded %d token(s)\n", g_token_count);
}

// Reload when the file changed (lets users revoke by editing tokens.txt).
static void tokens_refresh(void) {
    struct stat st;
    time_t mtime = (stat(OAUTH_TOKENS_PATH, &st) == 0) ? st.st_mtime : 0;
    if (!g_tokens_loaded || mtime != g_tokens_mtime)
        tokens_load();
}

static bool tokens_append(const char *token, const char *note) {
    FILE *f = fopen(OAUTH_TOKENS_PATH, "ab");
    if (!f)
        return false;
    char stamp[64] = "";
    time_t t = time(NULL);
    if (t > 1600000000) {
        struct tm tmv;
        gmtime_r(&t, &tmv);
        strftime(stamp, sizeof(stamp), " issued %Y-%m-%dT%H:%M:%SZ", &tmv);
    }
    fprintf(f, "%s #%s %s\n", token, stamp, note ? note : "");
    fclose(f);

    struct stat st;
    if (stat(OAUTH_TOKENS_PATH, &st) == 0)
        g_tokens_mtime = st.st_mtime;
    if (g_token_count < MAX_TOKENS)
        snprintf(g_tokens[g_token_count++], TOKEN_HEX_LEN + 1, "%s", token);
    return true;
}

bool oauth_token_valid(const char *token) {
    tokens_refresh();
    bool ok = false;
    for (int i = 0; i < g_token_count; i++) {
        // Check every entry (no early exit) to keep timing uniform.
        if (http_secure_streq(token, g_tokens[i]))
            ok = true;
    }
    return ok;
}

void oauth_init(const Config *cfg) {
    g_cfg = cfg;
    tokens_load();
}

bool oauth_mint_token(char *out, size_t outsz, const char *note) {
    if (outsz < TOKEN_HEX_LEN + 1)
        return false;
    uint8_t rnd[32];
    fill_random(rnd, sizeof(rnd));
    to_hex(rnd, sizeof(rnd), out);
    return tokens_append(out, note);
}

// --- pending authorization codes ---------------------------------------------

typedef struct {
    char code[40];          // 16 random bytes as hex
    char challenge[96];     // PKCE S256 code_challenge (43 base64url chars)
    char redirect_uri[512];
    uint64_t expires_at;    // now_secs() deadline; 0 = slot free
} AuthCode;

static AuthCode g_codes[MAX_AUTH_CODES];

static AuthCode *code_create(const char *challenge, const char *redirect_uri) {
    // Pick a free or the oldest slot.
    AuthCode *slot = &g_codes[0];
    uint64_t now = now_secs();
    for (int i = 0; i < MAX_AUTH_CODES; i++) {
        if (g_codes[i].expires_at == 0 || g_codes[i].expires_at < now) {
            slot = &g_codes[i];
            break;
        }
        if (g_codes[i].expires_at < slot->expires_at)
            slot = &g_codes[i];
    }
    uint8_t rnd[16];
    fill_random(rnd, sizeof(rnd));
    to_hex(rnd, sizeof(rnd), slot->code);
    snprintf(slot->challenge, sizeof(slot->challenge), "%s", challenge);
    snprintf(slot->redirect_uri, sizeof(slot->redirect_uri), "%s", redirect_uri);
    slot->expires_at = now + CODE_LIFETIME_SECS;
    return slot;
}

static AuthCode *code_take(const char *code) {
    uint64_t now = now_secs();
    for (int i = 0; i < MAX_AUTH_CODES; i++) {
        if (g_codes[i].expires_at != 0 && g_codes[i].expires_at >= now &&
            http_secure_streq(code, g_codes[i].code)) {
            g_codes[i].expires_at = 0; // single use
            return &g_codes[i];
        }
    }
    return NULL;
}

// --- discovery metadata --------------------------------------------------------

// Resolves the externally-visible base URL from the Host header.
static bool base_url(const HttpRequest *req, char *out, size_t outsz) {
    if (req->host[0] == '\0')
        return false;
    snprintf(out, outsz, "http://%s", req->host);
    return true;
}

void oauth_handle_protected_resource(HttpRequest *req) {
    char base[160];
    if (!base_url(req, base, sizeof(base))) {
        http_send_error(req->fd, 400, "missing Host header");
        return;
    }
    http_send_json(req->fd, 200,
                   "{\"resource\":\"%s/mcp\","
                   "\"authorization_servers\":[\"%s\"],"
                   "\"bearer_methods_supported\":[\"header\"]}",
                   base, base);
}

void oauth_handle_as_metadata(HttpRequest *req) {
    char base[160];
    if (!base_url(req, base, sizeof(base))) {
        http_send_error(req->fd, 400, "missing Host header");
        return;
    }
    http_send_json(req->fd, 200,
                   "{\"issuer\":\"%s\","
                   "\"authorization_endpoint\":\"%s/oauth/authorize\","
                   "\"token_endpoint\":\"%s/oauth/token\","
                   "\"registration_endpoint\":\"%s/oauth/register\","
                   "\"response_types_supported\":[\"code\"],"
                   "\"grant_types_supported\":[\"authorization_code\"],"
                   "\"token_endpoint_auth_methods_supported\":[\"none\"],"
                   "\"code_challenge_methods_supported\":[\"S256\"]}",
                   base, base, base, base);
}

// --- dynamic client registration (RFC 7591) -----------------------------------

void oauth_handle_register(HttpRequest *req) {
    static char body[8192];
    if (read_small_body(req, body, sizeof(body)) < 0)
        return;

    // Echo redirect_uris back when provided (clients expect it).
    static JsonDoc doc;
    char redirect_uris[1024] = "[]";
    if (json_parse(&doc, body, strlen(body)) == 0 && doc.ntok > 0 &&
        doc.tok[0].type == JSMN_OBJECT) {
        int uris = json_obj_get(&doc, 0, "redirect_uris");
        if (uris >= 0)
            json_raw(&doc, uris, redirect_uris, sizeof(redirect_uris));
    }

    http_send_json(req->fd, 201,
                   "{\"client_id\":\"" OAUTH_CLIENT_ID "\","
                   "\"redirect_uris\":%s,"
                   "\"token_endpoint_auth_method\":\"none\","
                   "\"grant_types\":[\"authorization_code\"],"
                   "\"response_types\":[\"code\"]}",
                   redirect_uris);
}

// --- authorization endpoint ----------------------------------------------------

static const char kPageHead[] =
    "<!DOCTYPE html><html><head><meta charset=\"utf-8\">"
    "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">"
    "<title>sys-autopilot</title><style>"
    "body{font-family:-apple-system,system-ui,sans-serif;background:#1a1b1e;"
    "color:#e8e8e8;display:flex;justify-content:center;align-items:center;"
    "min-height:100vh;margin:0}"
    "form{background:#26272b;padding:2rem;border-radius:12px;width:20rem;"
    "display:flex;flex-direction:column;gap:.75rem}"
    "h1{font-size:1.2rem;margin:0}"
    "p{margin:0;color:#9a9aa3;font-size:.9rem}"
    ".err{color:#ff6b6b;font-size:.9rem;margin:0}"
    "input{background:#1a1b1e;border:1px solid #3a3b40;border-radius:8px;"
    "padding:.6rem .8rem;color:#e8e8e8;font-size:1rem}"
    "button{background:#e60012;border:none;border-radius:8px;padding:.7rem;"
    "color:#fff;font-size:1rem;font-weight:600;cursor:pointer}"
    "</style></head><body>";
static const char kPageFoot[] = "</body></html>";


// Bounded string builder for the login page.
typedef struct { char *buf; size_t cap; size_t len; bool ok; } Sb;

static void sb_puts(Sb *sb, const char *s) {
    size_t n = strlen(s);
    if (!sb->ok || sb->len + n + 1 > sb->cap) { sb->ok = false; return; }
    memcpy(sb->buf + sb->len, s, n);
    sb->len += n;
    sb->buf[sb->len] = '\0';
}

static void sb_puts_html(Sb *sb, const char *s) {
    if (sb->ok && !html_escape_append(sb->buf, sb->cap, &sb->len, s))
        sb->ok = false;
}

// Renders the login form. error may be NULL. The OAuth params are carried
// through as hidden fields (HTML-escaped).
static void send_login_page(HttpRequest *req, const char *error,
                            const char *redirect_uri, const char *state,
                            const char *challenge) {
    static char page[8192];
    Sb sb = { .buf = page, .cap = sizeof(page), .ok = true };
    page[0] = '\0';

    sb_puts(&sb, kPageHead);
    sb_puts(&sb, "<form method=\"POST\" action=\"/oauth/authorize\">"
                 "<h1>sys-autopilot</h1>"
                 "<p>An application is requesting access to your Switch.</p>");
    if (error) {
        sb_puts(&sb, "<p class=\"err\">");
        sb_puts_html(&sb, error);
        sb_puts(&sb, "</p>");
    }

    const struct { const char *name; const char *value; } hidden[] = {
        { "redirect_uri", redirect_uri },
        { "state", state },
        { "code_challenge", challenge },
    };
    for (int i = 0; i < 3; i++) {
        sb_puts(&sb, "<input type=\"hidden\" name=\"");
        sb_puts(&sb, hidden[i].name);
        sb_puts(&sb, "\" value=\"");
        sb_puts_html(&sb, hidden[i].value);
        sb_puts(&sb, "\">");
    }

    sb_puts(&sb, "<input name=\"username\" placeholder=\"Username\" autofocus>"
                 "<input type=\"password\" name=\"password\" placeholder=\"Password\">"
                 "<button>Sign in</button></form>");
    sb_puts(&sb, kPageFoot);

    if (!sb.ok) {
        http_send_error(req->fd, 400, "request parameters too long");
        return;
    }
    http_send_response(req->fd, 200, "text/html", page, sb.len);
}

static bool creds_configured(void) {
    return g_cfg && g_cfg->username[0] != '\0' && g_cfg->password[0] != '\0';
}

// Basic validation: non-empty, no whitespace/control chars (also prevents
// header injection through the Location header).
static bool redirect_uri_ok(const char *uri) {
    if (uri[0] == '\0')
        return false;
    for (const char *p = uri; *p; p++) {
        if ((unsigned char)*p <= 0x20 || *p == '"' || *p == '<' || *p == '>')
            return false;
    }
    return strstr(uri, "://") != NULL;
}

void oauth_handle_authorize_get(HttpRequest *req) {
    if (!creds_configured()) {
        static char page[4096];
        Sb sb = { .buf = page, .cap = sizeof(page), .ok = true };
        page[0] = '\0';
        sb_puts(&sb, kPageHead);
        sb_puts(&sb, "<form><h1>sys-autopilot</h1>"
                     "<p class=\"err\">No credentials configured. Set username and "
                     "password in config/sys-autopilot/config.ini on the SD card "
                     "and reboot, then try again.</p></form>");
        sb_puts(&sb, kPageFoot);
        http_send_response(req->fd, 403, "text/html", page, sb.len);
        return;
    }

    char redirect_uri[512] = "", state[256] = "", challenge[96] = "", method[16] = "";
    http_query_get(req, "redirect_uri", redirect_uri, sizeof(redirect_uri));
    http_query_get(req, "state", state, sizeof(state));
    http_query_get(req, "code_challenge", challenge, sizeof(challenge));
    http_query_get(req, "code_challenge_method", method, sizeof(method));

    if (!redirect_uri_ok(redirect_uri)) {
        http_send_error(req->fd, 400, "missing or invalid redirect_uri");
        return;
    }
    if (challenge[0] == '\0' || (method[0] != '\0' && strcmp(method, "S256") != 0)) {
        http_send_error(req->fd, 400, "PKCE S256 code_challenge required");
        return;
    }

    send_login_page(req, NULL, redirect_uri, state, challenge);
}

void oauth_handle_authorize_post(HttpRequest *req) {
    if (!creds_configured()) {
        http_send_error(req->fd, 403, "no credentials configured");
        return;
    }

    static char body[4096];
    if (read_small_body(req, body, sizeof(body)) < 0)
        return;

    char redirect_uri[512] = "", state[256] = "", challenge[96] = "";
    char username[64] = "", password[64] = "";
    http_param_get(body, "redirect_uri", redirect_uri, sizeof(redirect_uri));
    http_param_get(body, "state", state, sizeof(state));
    http_param_get(body, "code_challenge", challenge, sizeof(challenge));
    http_param_get(body, "username", username, sizeof(username));
    http_param_get(body, "password", password, sizeof(password));

    if (!redirect_uri_ok(redirect_uri) || challenge[0] == '\0') {
        http_send_error(req->fd, 400, "missing or invalid OAuth parameters");
        return;
    }

    bool user_ok = http_secure_streq(username, g_cfg->username);
    bool pass_ok = http_secure_streq(password, g_cfg->password);
    if (!user_ok || !pass_ok) {
        LOGF("oauth: failed login attempt\n");
        send_login_page(req, "Incorrect username or password.",
                        redirect_uri, state, challenge);
        return;
    }

    AuthCode *code = code_create(challenge, redirect_uri);

    char location[1024];
    location[0] = '\0';
    Sb sb = { .buf = location, .cap = sizeof(location), .ok = true };
    sb_puts(&sb, redirect_uri);
    sb_puts(&sb, strchr(redirect_uri, '?') ? "&code=" : "?code=");
    sb_puts(&sb, code->code);
    if (state[0] != '\0') {
        sb_puts(&sb, "&state=");
        if (!url_encode_append(location, sizeof(location), &sb.len, state))
            sb.ok = false;
    }
    if (!sb.ok) {
        http_send_error(req->fd, 400, "redirect_uri too long");
        return;
    }

    LOGF("oauth: issued auth code\n");
    http_send_redirect(req->fd, location);
}

// --- token endpoint --------------------------------------------------------------

static void send_oauth_error(int fd, int status, const char *code, const char *desc) {
    http_send_json(fd, status, "{\"error\":\"%s\",\"error_description\":\"%s\"}",
                   code, desc);
}

void oauth_handle_token(HttpRequest *req) {
    static char body[4096];
    if (read_small_body(req, body, sizeof(body)) < 0)
        return;

    char grant_type[40] = "", code[64] = "", verifier[160] = "", redirect_uri[512] = "";
    http_param_get(body, "grant_type", grant_type, sizeof(grant_type));
    http_param_get(body, "code", code, sizeof(code));
    http_param_get(body, "code_verifier", verifier, sizeof(verifier));
    http_param_get(body, "redirect_uri", redirect_uri, sizeof(redirect_uri));

    if (strcmp(grant_type, "authorization_code") != 0) {
        send_oauth_error(req->fd, 400, "unsupported_grant_type",
                         "only authorization_code is supported");
        return;
    }
    if (code[0] == '\0' || verifier[0] == '\0') {
        send_oauth_error(req->fd, 400, "invalid_request",
                         "code and code_verifier are required");
        return;
    }

    AuthCode *entry = code_take(code);
    if (!entry) {
        send_oauth_error(req->fd, 400, "invalid_grant",
                         "unknown, expired, or already-used code");
        return;
    }

    // redirect_uri must match the authorization request when provided.
    if (redirect_uri[0] != '\0' && strcmp(redirect_uri, entry->redirect_uri) != 0) {
        send_oauth_error(req->fd, 400, "invalid_grant", "redirect_uri mismatch");
        return;
    }

    // PKCE S256: base64url(SHA256(verifier)) must equal the stored challenge.
    uint8_t digest[32];
    sha256_hash(digest, verifier, strlen(verifier));
    char computed[48];
    b64url_encode(digest, sizeof(digest), computed);
    if (!http_secure_streq(computed, entry->challenge)) {
        send_oauth_error(req->fd, 400, "invalid_grant", "PKCE verification failed");
        return;
    }

    // Mint and persist the token.
    char token[TOKEN_HEX_LEN + 1];
    if (!oauth_mint_token(token, sizeof(token), "via oauth login")) {
        send_oauth_error(req->fd, 500, "server_error", "failed to persist token");
        return;
    }

    LOGF("oauth: issued access token\n");
    http_send_json(req->fd, 200,
                   "{\"access_token\":\"%s\",\"token_type\":\"Bearer\"}", token);
}
