#include "mdns.h"
#include "config.h"
#include "device_info.h"
#include "netif.h"

#include <stdio.h>
#include <string.h>

// Injected by the Makefile from package.json; matches routes.c fallback.
#ifndef APP_VERSION
#define APP_VERSION "0.0.0-dev"
#endif

// Build "<instance>.<service>" (the DNS-SD instance FQDN).
static int fqdn(char *buf, size_t cap, const MdnsConfig *cfg) {
    return snprintf(buf, cap, "%s.%s", cfg->instance, cfg->service);
}

// "auth" TXT value reflecting the configured authentication scheme.
static const char *auth_kind(const Config *app_cfg) {
    if (app_cfg->username[0] != '\0' && app_cfg->password[0] != '\0')
        return "oauth"; // basic + OAuth browser flow available
    if (app_cfg->token[0] != '\0')
        return "token";
    return "none";
}

// Fills the name/port/TXT fields shared by every platform. The caller is
// responsible for setting cfg->ipv4_be.
static void mdns_fill_common(MdnsConfig *cfg, const Config *app_cfg) {
    const DeviceInfo *di = device_info_get();
    char name[64];
    config_hostname(app_cfg, di->serial, name, sizeof(name));

    memset(cfg, 0, sizeof(*cfg));
    snprintf(cfg->instance, sizeof(cfg->instance), "%s", name);
    snprintf(cfg->host, sizeof(cfg->host), "%s.local", name);
    snprintf(cfg->service, sizeof(cfg->service), "%s", MDNS_SERVICE_TYPE);
    cfg->port = (uint16_t)app_cfg->port;

    // DNS-SD TXT key=value pairs. Each must be <=255 bytes.
    char version[48], firmware[48], model[48], ams[48];
    snprintf(version,  sizeof(version),  "version=%s", APP_VERSION);
    snprintf(model,    sizeof(model),    "model=%s", di->model);
    snprintf(firmware, sizeof(firmware), "firmware=%s", di->firmware);
    snprintf(ams,      sizeof(ams),      "atmosphere=%s", di->atmosphere);

    const char *pairs[10];
    int n = 0;
    pairs[n++] = version;
    pairs[n++] = "path=/mcp";
    char auth[32];
    snprintf(auth, sizeof(auth), "auth=%s", auth_kind(app_cfg));
    pairs[n++] = auth;
    pairs[n++] = model;
    if (di->firmware[0] != '\0')
        pairs[n++] = firmware;
    if (di->atmosphere[0] != '\0')
        pairs[n++] = ams;
    pairs[n] = NULL;

    mdns_build_txt(cfg, pairs);
}

#ifndef __SWITCH__
// Host/test build: netif returns a fixed placeholder so the wire-format
// helpers can be exercised deterministically.
bool mdns_config_init(MdnsConfig *cfg, const Config *app_cfg) {
    mdns_fill_common(cfg, app_cfg);
    uint32_t s_addr = 0;
    if (!netif_current_ipv4(&s_addr))
        return false;
    const uint8_t *o = (const uint8_t *)&s_addr;
    cfg->ipv4 = ((uint32_t)o[0] << 24) | ((uint32_t)o[1] << 16) |
                ((uint32_t)o[2] << 8) | (uint32_t)o[3];
    return true;
}
#endif

// DNS record type / class constants.
#define DNS_TYPE_A    1
#define DNS_TYPE_PTR  12
#define DNS_TYPE_TXT  16
#define DNS_TYPE_SRV  33
#define DNS_TYPE_ANY  255

#define DNS_CLASS_IN       1
#define DNS_CLASS_FLUSH    0x8000  // cache-flush bit (top bit of the class)

// TTLs recommended by RFC 6762: 120s for host records, 75min for PTR.
#define TTL_HOST  120
#define TTL_PTR   4500

// --- low-level writers --------------------------------------------------------

typedef struct {
    uint8_t *buf;
    size_t cap;
    size_t len;
    bool ok;
} Writer;

static void w_u8(Writer *w, uint8_t v) {
    if (!w->ok) return;
    if (w->len + 1 > w->cap) { w->ok = false; return; }
    w->buf[w->len++] = v;
}

static void w_u16(Writer *w, uint16_t v) {
    w_u8(w, (uint8_t)(v >> 8));
    w_u8(w, (uint8_t)(v & 0xFF));
}

static void w_u32(Writer *w, uint32_t v) {
    w_u8(w, (uint8_t)(v >> 24));
    w_u8(w, (uint8_t)(v >> 16));
    w_u8(w, (uint8_t)(v >> 8));
    w_u8(w, (uint8_t)(v & 0xFF));
}

static void w_bytes(Writer *w, const void *p, size_t n) {
    if (!w->ok) return;
    if (w->len + n > w->cap) { w->ok = false; return; }
    memcpy(w->buf + w->len, p, n);
    w->len += n;
}

// Encodes a dotted name ("foo.bar.local") as DNS labels, no compression.
static void w_name(Writer *w, const char *name) {
    const char *seg = name;
    while (*seg) {
        const char *dot = strchr(seg, '.');
        size_t seglen = dot ? (size_t)(dot - seg) : strlen(seg);
        if (seglen == 0 || seglen > 63) { w->ok = false; return; }
        w_u8(w, (uint8_t)seglen);
        w_bytes(w, seg, seglen);
        if (!dot) break;
        seg = dot + 1;
    }
    w_u8(w, 0); // root label
}

// Writes a resource record header (name, type, class, ttl) and reserves a
// 2-byte rdata length placeholder, returning the offset of that placeholder.
static size_t w_rr_head(Writer *w, const char *name, uint16_t type,
                        uint16_t class_, uint32_t ttl) {
    w_name(w, name);
    w_u16(w, type);
    w_u16(w, class_);
    w_u32(w, ttl);
    size_t at = w->len;
    w_u16(w, 0); // rdlength placeholder
    return at;
}

// Backpatches the rdata length for the record whose placeholder is at `at`.
static void w_rr_patch(Writer *w, size_t at) {
    if (!w->ok || at + 2 > w->len) return;
    uint16_t rdlen = (uint16_t)(w->len - (at + 2));
    w->buf[at]     = (uint8_t)(rdlen >> 8);
    w->buf[at + 1] = (uint8_t)(rdlen & 0xFF);
}

// --- record emitters ----------------------------------------------------------

static void emit_a(Writer *w, const MdnsConfig *cfg) {
    size_t at = w_rr_head(w, cfg->host, DNS_TYPE_A,
                          DNS_CLASS_IN | DNS_CLASS_FLUSH, TTL_HOST);
    // ipv4 is stored host-order with octet 1 in the high byte; emit the four
    // octets big-endian (network order) regardless of machine endianness.
    uint32_t v = cfg->ipv4;
    w_u8(w, (uint8_t)(v >> 24));
    w_u8(w, (uint8_t)(v >> 16));
    w_u8(w, (uint8_t)(v >> 8));
    w_u8(w, (uint8_t)(v & 0xFF));
    w_rr_patch(w, at);
}

static void emit_ptr(Writer *w, const MdnsConfig *cfg) {
    char instance_fqdn[128];
    // "<instance>._sys-autopilot._tcp.local"
    fqdn(instance_fqdn, sizeof(instance_fqdn), cfg);
    size_t at = w_rr_head(w, cfg->service, DNS_TYPE_PTR, DNS_CLASS_IN, TTL_PTR);
    w_name(w, instance_fqdn);
    w_rr_patch(w, at);
}

static void emit_srv(Writer *w, const MdnsConfig *cfg) {
    char instance_fqdn[128];
    fqdn(instance_fqdn, sizeof(instance_fqdn), cfg);
    size_t at = w_rr_head(w, instance_fqdn, DNS_TYPE_SRV,
                          DNS_CLASS_IN | DNS_CLASS_FLUSH, TTL_HOST);
    w_u16(w, 0);            // priority
    w_u16(w, 0);            // weight
    w_u16(w, cfg->port);    // port
    w_name(w, cfg->host);   // target
    w_rr_patch(w, at);
}

static void emit_txt(Writer *w, const MdnsConfig *cfg) {
    char instance_fqdn[128];
    fqdn(instance_fqdn, sizeof(instance_fqdn), cfg);
    size_t at = w_rr_head(w, instance_fqdn, DNS_TYPE_TXT,
                          DNS_CLASS_IN | DNS_CLASS_FLUSH, TTL_PTR);
    if (cfg->txt_len > 0) {
        w_bytes(w, cfg->txt, cfg->txt_len);
    } else {
        w_u8(w, 0); // a single empty string => valid empty TXT
    }
    w_rr_patch(w, at);
}

// --- public: TXT builder ------------------------------------------------------

void mdns_build_txt(MdnsConfig *cfg, const char *const *pairs) {
    size_t len = 0;
    for (size_t i = 0; pairs && pairs[i]; i++) {
        size_t plen = strlen(pairs[i]);
        if (plen == 0 || plen > 255)
            continue;
        if (len + 1 + plen > sizeof(cfg->txt))
            break;
        cfg->txt[len++] = (uint8_t)plen;
        memcpy(cfg->txt + len, pairs[i], plen);
        len += plen;
    }
    cfg->txt_len = len;
}

// --- query parsing ------------------------------------------------------------

// Compares a DNS-encoded name starting at packet offset `off` against the
// dotted `name`, following compression pointers. Returns true on match.
static bool name_equals(const uint8_t *pkt, size_t pkt_len, size_t off,
                        const char *name) {
    const char *seg = name;
    int hops = 0;
    while (off < pkt_len) {
        uint8_t b = pkt[off];
        if (b == 0)
            return *seg == '\0';
        if ((b & 0xC0) == 0xC0) { // compression pointer
            if (off + 1 >= pkt_len || ++hops > 16)
                return false;
            off = ((b & 0x3F) << 8) | pkt[off + 1];
            continue;
        }
        if (b > 63 || off + 1 + b > pkt_len)
            return false;
        // Compare this label (case-insensitive) to the segment in `name`.
        for (uint8_t i = 0; i < b; i++) {
            char c = (char)pkt[off + 1 + i];
            char d = seg[i];
            if (d == '\0' || d == '.')
                return false;
            char lc = (c >= 'A' && c <= 'Z') ? c + 32 : c;
            char ld = (d >= 'A' && d <= 'Z') ? d + 32 : d;
            if (lc != ld)
                return false;
        }
        seg += b;
        if (*seg == '.')
            seg++;
        else if (*seg != '\0')
            return false;
        off += 1 + b;
    }
    return false;
}

// Skips a DNS-encoded name (handles compression) and returns the offset just
// past it, or 0 on malformed input.
static size_t skip_name(const uint8_t *pkt, size_t pkt_len, size_t off) {
    while (off < pkt_len) {
        uint8_t b = pkt[off];
        if (b == 0)
            return off + 1;
        if ((b & 0xC0) == 0xC0)
            return off + 2; // pointer terminates the name
        if (b > 63 || off + 1 + b > pkt_len)
            return 0;
        off += 1 + b;
    }
    return 0;
}

// What a question is asking about, relative to our advertised names.
typedef struct {
    bool want_a;       // A for cfg->host
    bool want_ptr;     // PTR for the service type (browse)
    bool want_srv;     // SRV for the instance
    bool want_txt;     // TXT for the instance
    bool unicast;      // a matched question set the QU bit (unicast response)
} WantSet;

static void scan_questions(const MdnsConfig *cfg,
                           const uint8_t *pkt, size_t pkt_len, WantSet *w) {
    if (pkt_len < 12)
        return;
    uint16_t qd = (uint16_t)((pkt[4] << 8) | pkt[5]);
    size_t off = 12;
    char instance_fqdn[128];
    fqdn(instance_fqdn, sizeof(instance_fqdn), cfg);

    for (uint16_t i = 0; i < qd && off < pkt_len; i++) {
        size_t name_off = off;
        size_t after = skip_name(pkt, pkt_len, off);
        if (after == 0 || after + 4 > pkt_len)
            return;
        uint16_t qtype  = (uint16_t)((pkt[after] << 8) | pkt[after + 1]);
        uint16_t qclass = (uint16_t)((pkt[after + 2] << 8) | pkt[after + 3]);
        off = after + 4; // skip qtype + qclass

        // Top bit of qclass is the "unicast response requested" (QU) bit.
        bool qu = (qclass & 0x8000) != 0;
        bool any = (qtype == DNS_TYPE_ANY);
        bool matched = false;
        if (name_equals(pkt, pkt_len, name_off, cfg->host)) {
            if (any || qtype == DNS_TYPE_A) { w->want_a = true; matched = true; }
        } else if (name_equals(pkt, pkt_len, name_off, cfg->service)) {
            if (any || qtype == DNS_TYPE_PTR) { w->want_ptr = true; matched = true; }
        } else if (name_equals(pkt, pkt_len, name_off, instance_fqdn)) {
            if (any || qtype == DNS_TYPE_SRV) { w->want_srv = true; matched = true; }
            if (any || qtype == DNS_TYPE_TXT) { w->want_txt = true; matched = true; }
        }
        if (matched && qu)
            w->unicast = true;
    }
}

// Writes the response header for `ancount` answers (QR=1, AA=1).
static void w_response_header(Writer *w, uint16_t ancount) {
    w_u16(w, 0);          // transaction id (0 for mDNS)
    w_u16(w, 0x8400);     // flags: QR + AA
    w_u16(w, 0);          // qdcount
    w_u16(w, ancount);    // ancount
    w_u16(w, 0);          // nscount
    w_u16(w, 0);          // arcount
}

size_t mdns_build_response(const MdnsConfig *cfg,
                           const uint8_t *query, size_t query_len,
                           uint8_t *out, size_t out_cap, bool *out_unicast) {
    if (out_unicast)
        *out_unicast = false;
    if (query_len < 12)
        return 0;
    // Ignore responses (QR bit set); only answer queries.
    if (query[2] & 0x80)
        return 0;

    WantSet want = {0};
    scan_questions(cfg, query, query_len, &want);
    if (out_unicast)
        *out_unicast = want.unicast;

    // When the service PTR is requested, include SRV/TXT/A too (known-answer
    // suppression aside, bundling speeds up resolution).
    if (want.want_ptr) {
        want.want_srv = true;
        want.want_txt = true;
        want.want_a = true;
    }
    if (want.want_srv)
        want.want_a = true;

    uint16_t ancount = (uint16_t)(want.want_a + want.want_ptr +
                                  want.want_srv + want.want_txt);
    if (ancount == 0)
        return 0;

    Writer w = { out, out_cap, 0, true };
    w_response_header(&w, ancount);
    if (want.want_ptr) emit_ptr(&w, cfg);
    if (want.want_srv) emit_srv(&w, cfg);
    if (want.want_txt) emit_txt(&w, cfg);
    if (want.want_a)   emit_a(&w, cfg);

    return w.ok ? w.len : 0;
}

size_t mdns_build_announcement(const MdnsConfig *cfg,
                               uint8_t *out, size_t out_cap) {
    Writer w = { out, out_cap, 0, true };
    w_response_header(&w, 4);
    emit_ptr(&w, cfg);
    emit_srv(&w, cfg);
    emit_txt(&w, cfg);
    emit_a(&w, cfg);
    return w.ok ? w.len : 0;
}

// --- Switch socket layer ------------------------------------------------------

#ifdef __SWITCH__
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <switch.h>

#include "log.h"

bool mdns_config_init(MdnsConfig *cfg, const Config *app_cfg) {
    mdns_fill_common(cfg, app_cfg);
    // Learn our LAN IPv4 from nifm (gethostid() returns loopback in a
    // sysmodule). The value is a struct in_addr.s_addr (network byte order);
    // decode octets via its byte layout, which is endianness-proof. Before
    // DHCP completes this returns false, and the server loop keeps retrying.
    uint32_t s_addr = 0;
    if (!netif_current_ipv4(&s_addr))
        return false; // not ready yet; the server loop retries

    const uint8_t *o = (const uint8_t *)&s_addr; // o[0]=octet1 ... o[3]=octet4
    cfg->ipv4 = ((uint32_t)o[0] << 24) | ((uint32_t)o[1] << 16) |
                ((uint32_t)o[2] << 8) | (uint32_t)o[3];
    LOGF("mdns: config ready ip=%u.%u.%u.%u host=%s\n",
         o[0], o[1], o[2], o[3], cfg->host);
    return true;
}

static struct sockaddr_in mdns_group(void) {
    struct sockaddr_in a = {0};
    a.sin_family = AF_INET;
    a.sin_port = htons(MDNS_PORT);
    a.sin_addr.s_addr = inet_addr(MDNS_MULTICAST_ADDR);
    return a;
}

int mdns_open(const MdnsConfig *cfg) {
    int fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (fd < 0) {
        LOGF("mdns: socket() failed (errno=%d)\n", errno);
        return -1;
    }

    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
#ifdef SO_REUSEPORT
    setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt));
#endif

    struct sockaddr_in bindaddr = {0};
    bindaddr.sin_family = AF_INET;
    bindaddr.sin_addr.s_addr = htonl(INADDR_ANY);
    bindaddr.sin_port = htons(MDNS_PORT);
    if (bind(fd, (struct sockaddr *)&bindaddr, sizeof(bindaddr)) != 0) {
        LOGF("mdns: bind :%d failed (errno=%d)\n", MDNS_PORT, errno);
        close(fd);
        return -1;
    }

    struct ip_mreq mreq = {0};
    mreq.imr_multiaddr.s_addr = inet_addr(MDNS_MULTICAST_ADDR);
    mreq.imr_interface.s_addr = htonl(INADDR_ANY);
    if (setsockopt(fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq)) != 0)
        LOGF("mdns: join group failed (errno=%d); continuing\n", errno);

    int flags = fcntl(fd, F_GETFL, 0);
    if (flags >= 0)
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);

    LOGF("mdns: advertising %s (service %s) on port %u\n",
         cfg->host, cfg->service, (unsigned)cfg->port);
    return fd;
}

void mdns_handle_readable(int fd, const MdnsConfig *cfg) {
    static uint8_t inbuf[1500];
    static uint8_t outbuf[1500];

    struct sockaddr_in from = {0};
    socklen_t fromlen = sizeof(from);
    ssize_t n = recvfrom(fd, inbuf, sizeof(inbuf), 0,
                         (struct sockaddr *)&from, &fromlen);
    if (n <= 0)
        return;

    bool unicast = false;
    size_t rlen = mdns_build_response(cfg, inbuf, (size_t)n,
                                      outbuf, sizeof(outbuf), &unicast);
    if (rlen == 0)
        return;

    // Reply to the multicast group by default, or directly to the sender when
    // it set the unicast-response (QU) bit (e.g. dig, one-shot resolvers).
    struct sockaddr_in dst = unicast ? from : mdns_group();
    if (sendto(fd, outbuf, rlen, 0, (struct sockaddr *)&dst, sizeof(dst)) < 0)
        LOGF("mdns: sendto failed (errno=%d)\n", errno);
    else
        LOGF("mdns: replied %zu bytes to %s:%u (%s)\n", rlen,
             inet_ntoa(dst.sin_addr), ntohs(dst.sin_port),
             unicast ? "unicast" : "multicast");
}

void mdns_announce(int fd, const MdnsConfig *cfg) {
    static uint8_t outbuf[1500];
    size_t len = mdns_build_announcement(cfg, outbuf, sizeof(outbuf));
    if (len == 0)
        return;
    struct sockaddr_in grp = mdns_group();
    ssize_t sent = sendto(fd, outbuf, len, 0,
                          (struct sockaddr *)&grp, sizeof(grp));
    if (sent < 0)
        LOGF("mdns: announce failed (errno=%d)\n", errno);
    else
        LOGF("mdns: announced %zd bytes as %s\n", sent, cfg->host);
}

void mdns_close(int fd) {
    if (fd < 0)
        return;
    struct ip_mreq mreq = {0};
    mreq.imr_multiaddr.s_addr = inet_addr(MDNS_MULTICAST_ADDR);
    mreq.imr_interface.s_addr = htonl(INADDR_ANY);
    setsockopt(fd, IPPROTO_IP, IP_DROP_MEMBERSHIP, &mreq, sizeof(mreq));
    close(fd);
}
#endif // __SWITCH__
