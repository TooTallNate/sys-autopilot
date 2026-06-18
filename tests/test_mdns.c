// Host unit tests for the mDNS / DNS-SD wire-format responder (mdns.c).
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "config.h"
#include "mdns.h"

// --- tiny DNS reader for assertions -------------------------------------------

// Reads a (possibly compressed) name at `off` into dotted form `out`.
// Returns the offset just past the name in the question/record stream.
static size_t read_name(const uint8_t *p, size_t len, size_t off,
                        char *out, size_t outcap) {
    size_t o = 0;
    size_t ret = 0;
    int hops = 0;
    while (off < len) {
        uint8_t b = p[off];
        if (b == 0) { if (!ret) ret = off + 1; break; }
        if ((b & 0xC0) == 0xC0) {
            if (!ret) ret = off + 2;
            off = ((b & 0x3F) << 8) | p[off + 1];
            assert(++hops <= 16);
            continue;
        }
        if (o && o < outcap) out[o++] = '.';
        for (uint8_t i = 0; i < b; i++)
            if (o < outcap - 1) out[o++] = (char)p[off + 1 + i];
        off += 1 + b;
    }
    out[o < outcap ? o : outcap - 1] = '\0';
    return ret;
}

typedef struct { uint16_t type; char name[128]; const uint8_t *rdata; uint16_t rdlen; } Rec;

// Parses a response packet's answers into recs[]; returns the answer count.
static int parse_answers(const uint8_t *p, size_t len, Rec *recs, int maxrec) {
    assert(len >= 12);
    uint16_t ancount = (p[6] << 8) | p[7];
    size_t off = 12;
    int n = 0;
    for (uint16_t i = 0; i < ancount && n < maxrec; i++) {
        Rec r;
        off = read_name(p, len, off, r.name, sizeof(r.name));
        r.type = (p[off] << 8) | p[off + 1];
        r.rdlen = (p[off + 8] << 8) | p[off + 9];
        r.rdata = p + off + 10;
        off += 10 + r.rdlen;
        recs[n++] = r;
    }
    return n;
}

static const Rec *find(Rec *recs, int n, uint16_t type) {
    for (int i = 0; i < n; i++)
        if (recs[i].type == type) return &recs[i];
    return NULL;
}

// Builds a single-question query packet for `name` of `qtype`.
static size_t make_query(uint8_t *buf, const char *name, uint16_t qtype) {
    size_t o = 0;
    memset(buf, 0, 12);
    buf[5] = 1; // qdcount = 1
    o = 12;
    const char *seg = name;
    while (*seg) {
        const char *dot = strchr(seg, '.');
        size_t sl = dot ? (size_t)(dot - seg) : strlen(seg);
        buf[o++] = (uint8_t)sl;
        memcpy(buf + o, seg, sl); o += sl;
        if (!dot) break;
        seg = dot + 1;
    }
    buf[o++] = 0;
    buf[o++] = (uint8_t)(qtype >> 8); buf[o++] = (uint8_t)(qtype & 0xFF);
    buf[o++] = 0; buf[o++] = 1; // class IN
    return o;
}

// Same, but sets the QU (unicast-response-requested) bit in qclass.
static size_t make_query_qu(uint8_t *buf, const char *name, uint16_t qtype) {
    size_t o = make_query(buf, name, qtype);
    buf[o - 2] = 0x80; // qclass high byte: QU bit set, class IN
    return o;
}

#define DNS_A 1
#define DNS_PTR 12
#define DNS_TXT 16
#define DNS_SRV 33

static MdnsConfig make_cfg(void) {
    Config app = {0};
    app.port = 4150;
    snprintf(app.hostname, sizeof(app.hostname), "test-switch");
    MdnsConfig cfg;
    assert(mdns_config_init(&cfg, &app));
    return cfg;
}

// --- tests --------------------------------------------------------------------

static void test_a_query(void) {
    MdnsConfig cfg = make_cfg();
    uint8_t q[256], out[1500];
    size_t qn = make_query(q, "test-switch.local", DNS_A);
    size_t rn = mdns_build_response(&cfg, q, qn, out, sizeof(out), NULL);
    assert(rn > 0);

    Rec recs[8];
    int n = parse_answers(out, rn, recs, 8);
    const Rec *a = find(recs, n, DNS_A);
    assert(a);
    assert(strcmp(a->name, "test-switch.local") == 0);
    assert(a->rdlen == 4);
    // 127.0.0.1 (host-build placeholder)
    assert(a->rdata[0] == 127 && a->rdata[3] == 1);
    printf("  A query: ok\n");
}

static void test_ptr_browse(void) {
    MdnsConfig cfg = make_cfg();
    uint8_t q[256], out[1500];
    size_t qn = make_query(q, "_sys-autopilot._tcp.local", DNS_PTR);
    size_t rn = mdns_build_response(&cfg, q, qn, out, sizeof(out), NULL);
    assert(rn > 0);

    Rec recs[8];
    int n = parse_answers(out, rn, recs, 8);
    // A browse should yield PTR + SRV + TXT + A bundled.
    const Rec *ptr = find(recs, n, DNS_PTR);
    const Rec *srv = find(recs, n, DNS_SRV);
    const Rec *txt = find(recs, n, DNS_TXT);
    const Rec *a   = find(recs, n, DNS_A);
    assert(ptr && srv && txt && a);

    // PTR target should be the instance FQDN.
    char target[128];
    read_name(out, rn, (size_t)(ptr->rdata - out), target, sizeof(target));
    assert(strcmp(target, "test-switch._sys-autopilot._tcp.local") == 0);
    printf("  PTR browse: ok (ptr+srv+txt+a)\n");
}

static void test_srv_port(void) {
    MdnsConfig cfg = make_cfg();
    uint8_t q[256], out[1500];
    size_t qn = make_query(q, "test-switch._sys-autopilot._tcp.local", DNS_SRV);
    size_t rn = mdns_build_response(&cfg, q, qn, out, sizeof(out), NULL);
    assert(rn > 0);

    Rec recs[8];
    int n = parse_answers(out, rn, recs, 8);
    const Rec *srv = find(recs, n, DNS_SRV);
    assert(srv);
    // rdata: priority(2) weight(2) port(2) target(name)
    uint16_t port = (srv->rdata[4] << 8) | srv->rdata[5];
    assert(port == 4150);
    char tgt[128];
    read_name(out, rn, (size_t)(srv->rdata + 6 - out), tgt, sizeof(tgt));
    assert(strcmp(tgt, "test-switch.local") == 0);
    printf("  SRV port/target: ok\n");
}

static bool txt_has(const Rec *txt, const char *kv) {
    size_t klen = strlen(kv);
    size_t o = 0;
    while (o < txt->rdlen) {
        uint8_t l = txt->rdata[o++];
        if (l == klen && memcmp(txt->rdata + o, kv, klen) == 0)
            return true;
        o += l;
    }
    return false;
}

static void test_txt_fields(void) {
    MdnsConfig cfg = make_cfg();
    uint8_t q[256], out[1500];
    size_t qn = make_query(q, "test-switch._sys-autopilot._tcp.local", DNS_TXT);
    size_t rn = mdns_build_response(&cfg, q, qn, out, sizeof(out), NULL);
    assert(rn > 0);
    Rec recs[8];
    int n = parse_answers(out, rn, recs, 8);
    const Rec *txt = find(recs, n, DNS_TXT);
    assert(txt);
    assert(txt_has(txt, "path=/mcp"));
    assert(txt_has(txt, "auth=none"));
    assert(txt_has(txt, "model=unknown")); // host placeholder
    printf("  TXT fields: ok\n");
}

static void test_no_match(void) {
    MdnsConfig cfg = make_cfg();
    uint8_t q[256], out[1500];
    size_t qn = make_query(q, "other-host.local", DNS_A);
    size_t rn = mdns_build_response(&cfg, q, qn, out, sizeof(out), NULL);
    assert(rn == 0); // nothing to answer
    printf("  unrelated query ignored: ok\n");
}

static void test_default_hostname(void) {
    // Blank hostname -> auto "switch-<last 4 of serial>". The host stub serial
    // is "TEST0000000001", so the suffix is "0001".
    Config app = {0};
    app.port = 4150;
    MdnsConfig cfg;
    assert(mdns_config_init(&cfg, &app));
    assert(strcmp(cfg.host, "switch-0001.local") == 0);
    assert(strcmp(cfg.instance, "switch-0001") == 0);
    printf("  default hostname (serial suffix): ok\n");
}

static void test_config_hostname_helper(void) {
    char out[64];
    Config app = {0};
    // Explicit hostname wins.
    snprintf(app.hostname, sizeof(app.hostname), "my-switch");
    config_hostname(&app, "XAW12345", out, sizeof(out));
    assert(strcmp(out, "my-switch") == 0);

    // Auto with serial.
    app.hostname[0] = '\0';
    config_hostname(&app, "XAW10012345678", out, sizeof(out));
    assert(strcmp(out, "switch-5678") == 0);

    // No serial -> bare prefix.
    config_hostname(&app, NULL, out, sizeof(out));
    assert(strcmp(out, "switch") == 0);
    config_hostname(&app, "", out, sizeof(out));
    assert(strcmp(out, "switch") == 0);

    // Short serial used whole; sanitization lowercases.
    config_hostname(&app, "AB", out, sizeof(out));
    assert(strcmp(out, "switch-ab") == 0);
    printf("  config_hostname helper: ok\n");
}

static void test_unicast_bit(void) {
    MdnsConfig cfg = make_cfg();
    uint8_t q[256], out[1500];
    bool uni = false;

    // Normal (QM) query -> multicast reply requested.
    size_t qn = make_query(q, "test-switch.local", DNS_A);
    assert(mdns_build_response(&cfg, q, qn, out, sizeof(out), &uni) > 0);
    assert(uni == false);

    // QU query -> unicast reply requested.
    qn = make_query_qu(q, "test-switch.local", DNS_A);
    assert(mdns_build_response(&cfg, q, qn, out, sizeof(out), &uni) > 0);
    assert(uni == true);

    // QU bit on an unrelated name must not force unicast (no match).
    qn = make_query_qu(q, "other.local", DNS_A);
    assert(mdns_build_response(&cfg, q, qn, out, sizeof(out), &uni) == 0);
    assert(uni == false);
    printf("  unicast (QU) bit: ok\n");
}

static void test_announcement(void) {
    MdnsConfig cfg = make_cfg();
    uint8_t out[1500];
    size_t rn = mdns_build_announcement(&cfg, out, sizeof(out));
    assert(rn > 0);
    Rec recs[8];
    int n = parse_answers(out, rn, recs, 8);
    assert(n == 4);
    assert(find(recs, n, DNS_PTR) && find(recs, n, DNS_SRV) &&
           find(recs, n, DNS_TXT) && find(recs, n, DNS_A));
    printf("  announcement: ok\n");
}

int main(void) {
    printf("== test_mdns ==\n");
    test_a_query();
    test_ptr_browse();
    test_srv_port();
    test_txt_fields();
    test_no_match();
    test_default_hostname();
    test_config_hostname_helper();
    test_unicast_bit();
    test_announcement();
    printf("test_mdns: all passed\n");
    return 0;
}
