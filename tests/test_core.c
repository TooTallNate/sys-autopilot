// Host unit tests for the pure-logic modules: base64, json, jstream, buttons.
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "base64.h"
#include "buttons.h"
#include "json.h"
#include "jstream.h"
#include "sha256.h"

// --- base64 -------------------------------------------------------------------

static void test_base64(void) {
    // Round-trip various lengths (exercises all padding cases).
    const char *cases[] = { "", "f", "fo", "foo", "foob", "fooba", "foobar" };
    const char *expect[] = { "", "Zg==", "Zm8=", "Zm9v", "Zm9vYg==", "Zm9vYmE=", "Zm9vYmFy" };
    for (int i = 0; i < 7; i++) {
        char enc[32];
        size_t n = b64_encode((const uint8_t *)cases[i], strlen(cases[i]), enc);
        enc[n] = '\0';
        assert(strcmp(enc, expect[i]) == 0);
        assert(n == b64_encoded_len(strlen(cases[i])));

        char dec[32];
        size_t dn = b64_decode(enc, dec, sizeof(dec));
        assert(dn == strlen(cases[i]));
        assert(memcmp(dec, cases[i], dn) == 0);
    }

    // Incremental decode split mid-quad.
    B64Decoder d;
    b64dec_init(&d);
    uint8_t out[32];
    size_t total = 0;
    const char *b64 = "Zm9vYmFy"; // "foobar"
    for (size_t i = 0; i < strlen(b64); i++) {
        ssize_t n = b64dec_update(&d, b64 + i, 1, out + total);
        assert(n >= 0);
        total += (size_t)n;
    }
    assert(b64dec_finish(&d));
    assert(total == 6 && memcmp(out, "foobar", 6) == 0);

    // Invalid character rejected.
    b64dec_init(&d);
    assert(b64dec_update(&d, "Zm!v", 4, out) < 0);

    // Chunked encode (multiple of 3) equals whole-buffer encode.
    uint8_t data[300];
    for (int i = 0; i < 300; i++) data[i] = (uint8_t)(i * 7);
    char whole[512], chunked[512];
    size_t wn = b64_encode(data, 300, whole);
    size_t cn = 0;
    for (size_t off = 0; off < 300; off += 99)
        cn += b64_encode(data + off, off + 99 <= 300 ? 99 : 300 - off, chunked + cn);
    assert(wn == cn && memcmp(whole, chunked, wn) == 0);

    printf("base64 ok\n");
}

// --- json ----------------------------------------------------------------------

static void test_json(void) {
    static JsonDoc doc;
    const char *src = "{\"jsonrpc\":\"2.0\",\"id\":7,\"params\":{\"name\":\"x\\n\\\"y\","
                      "\"arr\":[1,{\"k\":true},\"s\"],\"pi\":3.5,\"u\":\"\\u00e9\"}}";
    assert(json_parse(&doc, src, strlen(src)) == 0);

    int id = json_obj_get(&doc, 0, "id");
    long long v;
    assert(json_get_int(&doc, id, &v) && v == 7);

    char raw[16];
    assert(json_raw(&doc, id, raw, sizeof(raw)) && strcmp(raw, "7") == 0);

    int params = json_obj_get(&doc, 0, "params");
    assert(params > 0);
    int name = json_obj_get(&doc, params, "name");
    char s[32];
    assert(json_get_string(&doc, name, s, sizeof(s)));
    assert(strcmp(s, "x\n\"y") == 0);

    int arr = json_obj_get(&doc, params, "arr");
    assert(json_arr_len(&doc, arr) == 3);
    assert(json_get_int(&doc, json_arr_get(&doc, arr, 0), &v) && v == 1);
    int obj = json_arr_get(&doc, arr, 1);
    bool b;
    assert(json_get_bool(&doc, json_obj_get(&doc, obj, "k"), &b) && b);
    assert(json_get_string(&doc, json_arr_get(&doc, arr, 2), s, sizeof(s)));
    assert(strcmp(s, "s") == 0);

    double dv;
    assert(json_get_double(&doc, json_obj_get(&doc, params, "pi"), &dv) && dv == 3.5);

    assert(json_get_string(&doc, json_obj_get(&doc, params, "u"), s, sizeof(s)));
    assert(strcmp(s, "\xc3\xa9") == 0); // é in UTF-8

    // Escaping round-trip.
    const char *text = "a\"b\\c\nd\x01" "e";
    size_t elen = json_escaped_len(text, strlen(text));
    char esc[64];
    assert(json_escape(text, strlen(text), esc, sizeof(esc)) == elen);
    assert(strcmp(esc, "a\\\"b\\\\c\\nd\\u0001e") == 0);

    printf("json ok\n");
}

// --- jstream ---------------------------------------------------------------------

typedef struct {
    char data[4096];
    size_t len;
    int calls;
} CaptureSink;

static int capture_sink(const char *data, size_t len, void *ctx) {
    CaptureSink *cs = ctx;
    assert(cs->len + len < sizeof(cs->data));
    memcpy(cs->data + cs->len, data, len);
    cs->len += len;
    cs->calls++;
    return 0;
}

// Feeds src through jstream in chunks of `step` bytes.
static int run_jstream(const char *src, size_t step, char *doc, size_t doc_cap,
                       size_t *doc_len, CaptureSink *cs, bool *found) {
    Jstream js;
    memset(cs, 0, sizeof(*cs));
    jstream_init(&js, doc, doc_cap, capture_sink, cs);
    size_t len = strlen(src);
    for (size_t i = 0; i < len; i += step) {
        size_t n = i + step <= len ? step : len - i;
        int rc = jstream_feed(&js, src + i, n);
        if (rc)
            return rc;
    }
    int rc = jstream_finish(&js, found);
    *doc_len = js.doc_len;
    return rc;
}

static void test_jstream(void) {
    char doc[2048];
    size_t doc_len;
    CaptureSink cs;
    bool found;

    // Content extracted; reduced doc parseable; placeholder present.
    const char *src = "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/call\","
                      "\"params\":{\"name\":\"upload_file\",\"arguments\":"
                      "{\"content\":\"SGVsbG8=\",\"path\":\"/a.txt\"}}}";
    // Try several chunk sizes including 1 byte.
    size_t steps[] = { 1, 2, 3, 7, 4096 };
    for (int i = 0; i < 5; i++) {
        assert(run_jstream(src, steps[i], doc, sizeof(doc), &doc_len, &cs, &found) == 0);
        assert(found);
        assert(cs.len == 8 && memcmp(cs.data, "SGVsbG8=", 8) == 0);
        static JsonDoc jd;
        assert(json_parse(&jd, doc, doc_len) == 0);
        int params = json_obj_get(&jd, 0, "params");
        int args = json_obj_get(&jd, params, "arguments");
        char s[32];
        assert(json_get_string(&jd, json_obj_get(&jd, args, "path"), s, sizeof(s)));
        assert(strcmp(s, "/a.txt") == 0);
        assert(json_get_string(&jd, json_obj_get(&jd, args, "content"), s, sizeof(s)));
        assert(strcmp(s, "<streamed>") == 0);
    }

    // Content AFTER path (ordering shouldn't matter).
    const char *src2 = "{\"params\":{\"arguments\":{\"path\":\"/b\",\"content\":\"QUJD\"}}}";
    assert(run_jstream(src2, 1, doc, sizeof(doc), &doc_len, &cs, &found) == 0);
    assert(found && cs.len == 4 && memcmp(cs.data, "QUJD", 4) == 0);

    // Escapes in OTHER strings are preserved; content at wrong depth is NOT diverted.
    const char *src3 = "{\"content\":\"top\",\"params\":{\"arguments\":"
                       "{\"path\":\"a\\\"b\",\"note\":\"x\\\\y\"}},\"x\":[{\"content\":\"arr\"}]}";
    assert(run_jstream(src3, 1, doc, sizeof(doc), &doc_len, &cs, &found) == 0);
    assert(!found && cs.len == 0);
    static JsonDoc jd3;
    assert(json_parse(&jd3, doc, doc_len) == 0);
    char s3[16];
    assert(json_get_string(&jd3, json_obj_get(&jd3, 0, "content"), s3, sizeof(s3)));
    assert(strcmp(s3, "top") == 0);

    // Duplicate content fields rejected.
    const char *src4 = "{\"params\":{\"arguments\":{\"content\":\"QQ==\",\"z\":1,"
                       "\"content\":\"Qg==\"}}}";
    assert(run_jstream(src4, 4096, doc, sizeof(doc), &doc_len, &cs, &found) == JSTREAM_EDUP);

    // Escape inside content (invalid for base64) rejected.
    const char *src5 = "{\"params\":{\"arguments\":{\"content\":\"AB\\nCD\"}}}";
    assert(run_jstream(src5, 4096, doc, sizeof(doc), &doc_len, &cs, &found) == JSTREAM_ECONTENT);

    // Reduced-document overflow.
    char small[16];
    Jstream js;
    jstream_init(&js, small, sizeof(small), capture_sink, &cs);
    assert(jstream_feed(&js, src, strlen(src)) == JSTREAM_EDOC);

    // Truncated input.
    assert(run_jstream("{\"a\":\"unterminated", 4096, doc, sizeof(doc), &doc_len,
                       &cs, &found) == JSTREAM_EPARTIAL);

    printf("jstream ok\n");
}

// --- buttons ---------------------------------------------------------------------

static void test_buttons(void) {
    uint64_t m;
    assert(button_from_name("A", &m) && m == BTN_A);
    assert(button_from_name("a", &m) && m == BTN_A);
    assert(button_from_name("home", &m) && m == BTN_HOME);
    assert(button_from_name("START", &m) && m == BTN_PLUS);
    assert(button_from_name("dright", &m) && m == BTN_RIGHT);
    assert(!button_from_name("Q", &m));
    assert(!button_from_name("", &m));
    printf("buttons ok\n");
}

// --- sha256 ------------------------------------------------------------------

static void to_hex(const uint8_t *in, size_t n, char *out) {
    static const char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) {
        out[i * 2]     = hex[in[i] >> 4];
        out[i * 2 + 1] = hex[in[i] & 0xF];
    }
    out[n * 2] = '\0';
}

static void test_sha256(void) {
    uint8_t digest[32];
    char hex[65];

    // Known-answer vectors.
    sha256_hash(digest, "", 0);
    to_hex(digest, 32, hex);
    assert(strcmp(hex, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855") == 0);

    sha256_hash(digest, "abc", 3);
    to_hex(digest, 32, hex);
    assert(strcmp(hex, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad") == 0);

    // Streaming API must agree with the one-shot version, including across
    // 64-byte block boundaries. 1000 'a's -> known digest.
    char buf[1000];
    memset(buf, 'a', sizeof(buf));

    sha256_hash(digest, buf, sizeof(buf));
    to_hex(digest, 32, hex);
    char oneshot[65];
    strcpy(oneshot, hex);

    // Feed in awkward chunk sizes to exercise buffering.
    Sha256Stream st;
    sha256_stream_init(&st);
    size_t off = 0;
    size_t chunks[] = { 1, 63, 64, 65, 200, 7 };
    for (size_t i = 0; i < sizeof(chunks) / sizeof(chunks[0]) && off < sizeof(buf); i++) {
        size_t n = chunks[i];
        if (off + n > sizeof(buf))
            n = sizeof(buf) - off;
        sha256_stream_update(&st, buf + off, n);
        off += n;
    }
    if (off < sizeof(buf))
        sha256_stream_update(&st, buf + off, sizeof(buf) - off);
    sha256_stream_final(&st, digest);
    to_hex(digest, 32, hex);
    assert(strcmp(hex, oneshot) == 0);

    // Empty streaming hash matches the empty one-shot vector.
    sha256_stream_init(&st);
    sha256_stream_final(&st, digest);
    to_hex(digest, 32, hex);
    assert(strcmp(hex, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855") == 0);

    printf("sha256 ok\n");
}

int main(void) {
    test_base64();
    test_json();
    test_jstream();
    test_buttons();
    test_sha256();
    printf("all core tests passed\n");
    return 0;
}
