#include "base64.h"

#include <string.h>

static const char kEnc[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

size_t b64_encode(const uint8_t *in, size_t n, char *out) {
    size_t o = 0;
    size_t i = 0;
    while (i + 3 <= n) {
        uint32_t v = ((uint32_t)in[i] << 16) | ((uint32_t)in[i+1] << 8) | in[i+2];
        out[o++] = kEnc[(v >> 18) & 0x3F];
        out[o++] = kEnc[(v >> 12) & 0x3F];
        out[o++] = kEnc[(v >> 6) & 0x3F];
        out[o++] = kEnc[v & 0x3F];
        i += 3;
    }
    size_t rem = n - i;
    if (rem == 1) {
        uint32_t v = (uint32_t)in[i] << 16;
        out[o++] = kEnc[(v >> 18) & 0x3F];
        out[o++] = kEnc[(v >> 12) & 0x3F];
        out[o++] = '=';
        out[o++] = '=';
    } else if (rem == 2) {
        uint32_t v = ((uint32_t)in[i] << 16) | ((uint32_t)in[i+1] << 8);
        out[o++] = kEnc[(v >> 18) & 0x3F];
        out[o++] = kEnc[(v >> 12) & 0x3F];
        out[o++] = kEnc[(v >> 6) & 0x3F];
        out[o++] = '=';
    }
    return o;
}

size_t b64url_encode(const uint8_t *in, size_t n, char *out) {
    size_t len = b64_encode(in, n, out);
    size_t o = 0;
    for (size_t i = 0; i < len; i++) {
        char c = out[i];
        if (c == '+') c = '-';
        else if (c == '/') c = '_';
        else if (c == '=') break; // strip padding
        out[o++] = c;
    }
    out[o] = '\0';
    return o;
}

static int b64_val(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

void b64dec_init(B64Decoder *d) {
    memset(d, 0, sizeof(*d));
}

ssize_t b64dec_update(B64Decoder *d, const char *in, size_t inlen, uint8_t *out) {
    if (d->err)
        return -1;
    size_t o = 0;
    for (size_t i = 0; i < inlen; i++) {
        char c = in[i];
        if (c == '=' || c == '\n' || c == '\r' || c == ' ' || c == '\t')
            continue; // padding/whitespace: ignored, validity checked in finish
        int v = b64_val(c);
        if (v < 0) {
            d->err = true;
            return -1;
        }
        d->acc = (d->acc << 6) | (uint32_t)v;
        d->bits += 6;
        if (d->bits >= 8) {
            d->bits -= 8;
            out[o++] = (uint8_t)((d->acc >> d->bits) & 0xFF);
        }
    }
    return (ssize_t)o;
}

bool b64dec_finish(const B64Decoder *d) {
    // Leftover bits must be zero padding from a valid 2/3-char tail.
    if (d->err)
        return false;
    if (d->bits != 0 && (d->acc & ((1u << d->bits) - 1)) != 0)
        return false;
    return true;
}

size_t b64_decode(const char *in, char *out, size_t outsz) {
    B64Decoder d;
    b64dec_init(&d);
    size_t inlen = strlen(in);
    if ((inlen / 4 + 1) * 3 + 1 > outsz)
        return 0;
    ssize_t n = b64dec_update(&d, in, inlen, (uint8_t *)out);
    if (n < 0 || !b64dec_finish(&d))
        return 0;
    out[n] = '\0';
    return (size_t)n;
}
