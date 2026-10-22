#include "ustar.h"

#include <stdio.h>
#include <string.h>

// Header field offsets (POSIX ustar).
#define H_NAME     0    // 100
#define H_MODE     100  // 8
#define H_UID      108  // 8
#define H_GID      116  // 8
#define H_SIZE     124  // 12
#define H_MTIME    136  // 12
#define H_CHKSUM   148  // 8
#define H_TYPE     156  // 1
#define H_MAGIC    257  // 6 ("ustar\0"; GNU writes "ustar  \0" across magic+version)
#define H_VERSION  263  // 2
#define H_PREFIX   345  // 155

// Writes v as zero-padded octal filling width-1 digits plus a NUL. Returns
// false if v does not fit.
static bool put_octal(char *dst, size_t width, uint64_t v) {
    char tmp[24];
    int n = snprintf(tmp, sizeof(tmp), "%0*llo", (int)(width - 1), (unsigned long long)v);
    if (n < 0 || (size_t)n != width - 1)
        return false;
    memcpy(dst, tmp, width - 1);
    dst[width - 1] = '\0';
    return true;
}

// Strict octal field parse: optional leading spaces, octal digits, then
// NUL/space terminators only. Base-256 (high bit set) is rejected.
static bool get_octal(const char *p, size_t width, uint64_t *out) {
    size_t i = 0;
    if ((unsigned char)p[0] & 0x80)
        return false;
    while (i < width && p[i] == ' ')
        i++;
    uint64_t v = 0;
    size_t digits = 0;
    for (; i < width && p[i] >= '0' && p[i] <= '7'; i++, digits++) {
        if (v > (UINT64_MAX >> 3))
            return false;
        v = (v << 3) | (uint64_t)(p[i] - '0');
    }
    for (; i < width; i++)
        if (p[i] != '\0' && p[i] != ' ')
            return false;
    *out = v;
    return digits > 0 || p[0] == '\0';
}

static unsigned checksum_unsigned(const char hdr[USTAR_BLOCK]) {
    unsigned sum = 0;
    for (int i = 0; i < USTAR_BLOCK; i++)
        sum += (i >= H_CHKSUM && i < H_CHKSUM + 8) ? ' ' : (unsigned char)hdr[i];
    return sum;
}

// Some historic tars computed the checksum over signed chars.
static int checksum_signed(const char hdr[USTAR_BLOCK]) {
    int sum = 0;
    for (int i = 0; i < USTAR_BLOCK; i++)
        sum += (i >= H_CHKSUM && i < H_CHKSUM + 8) ? ' ' : (signed char)hdr[i];
    return sum;
}

bool ustar_build_header(char hdr[USTAR_BLOCK], const char *name, uint64_t size,
                        bool is_dir) {
    memset(hdr, 0, USTAR_BLOCK);
    size_t nl = strlen(name);
    if (nl == 0 || nl > USTAR_NAME_MAX)
        return false;

    const char *base = name;
    if (nl > 100) {
        // Split at the last '/' that leaves prefix <= 155 and the rest <= 100
        // (and non-empty, so a directory's trailing '/' is never the split).
        const char *split = NULL;
        for (const char *p = name; *p; p++)
            if (*p == '/' && (size_t)(p - name) <= 155 && p[1] != '\0' &&
                strlen(p + 1) <= 100)
                split = p;
        if (!split)
            return false;
        memcpy(hdr + H_PREFIX, name, (size_t)(split - name));
        base = split + 1;
    }
    memcpy(hdr + H_NAME, base, strlen(base));

    if (!put_octal(hdr + H_MODE, 8, is_dir ? 0755 : 0644) ||
        !put_octal(hdr + H_UID, 8, 0) ||
        !put_octal(hdr + H_GID, 8, 0) ||
        !put_octal(hdr + H_SIZE, 12, is_dir ? 0 : size) ||
        !put_octal(hdr + H_MTIME, 12, 0))
        return false;
    hdr[H_TYPE] = is_dir ? '5' : '0';
    memcpy(hdr + H_MAGIC, "ustar", 6); // includes the NUL
    memcpy(hdr + H_VERSION, "00", 2);

    char ck[8];
    snprintf(ck, sizeof(ck), "%06o", checksum_unsigned(hdr) & 0777777);
    memcpy(hdr + H_CHKSUM, ck, 6);
    hdr[H_CHKSUM + 6] = '\0';
    hdr[H_CHKSUM + 7] = ' ';
    return true;
}

bool ustar_is_zero_block(const char hdr[USTAR_BLOCK]) {
    for (int i = 0; i < USTAR_BLOCK; i++)
        if (hdr[i])
            return false;
    return true;
}

bool ustar_parse_header(const char hdr[USTAR_BLOCK], UstarEntry *out,
                        const char **err) {
    uint64_t want = 0;
    if (!get_octal(hdr + H_CHKSUM, 8, &want) || hdr[H_CHKSUM] == '\0') {
        *err = "not a tar archive (bad header checksum field)";
        return false;
    }
    if (want != checksum_unsigned(hdr) && (int64_t)want != checksum_signed(hdr)) {
        *err = "not a tar archive (header checksum mismatch)";
        return false;
    }

    char type = hdr[H_TYPE];
    if (type == 'L' || type == 'K') {
        *err = "GNU long-name entries are not supported (use ustar format)";
        return false;
    }
    uint64_t size = 0;
    if (!get_octal(hdr + H_SIZE, 12, &size)) {
        *err = "unsupported tar entry size field";
        return false;
    }

    // The prefix field is only meaningful in POSIX ustar ("ustar\0"); GNU
    // ("ustar  ") and v7 headers use that area for other data.
    bool posix = memcmp(hdr + H_MAGIC, "ustar", 6) == 0;
    char prefix[156] = {0}, base[101] = {0};
    if (posix)
        memcpy(prefix, hdr + H_PREFIX, 155);
    memcpy(base, hdr + H_NAME, 100);
    snprintf(out->name, sizeof(out->name), "%s%s%s", prefix, prefix[0] ? "/" : "", base);
    out->size = size;

    if (type == '0' || type == '\0' || type == '7') {
        out->type = USTAR_ENTRY_FILE;
    } else if (type == '5') {
        out->type = USTAR_ENTRY_DIR;
        out->size = 0; // directories carry no data
    } else {
        // Hard/sym links, devices, FIFOs, pax extended headers ('x'/'g'):
        // not meaningful in a save; skip their data.
        out->type = USTAR_ENTRY_SKIP;
        if (type == '1' || type == '2' || type == '3' || type == '4' || type == '6')
            out->size = 0; // these never carry data
    }
    return true;
}

bool ustar_sanitize_path(const char *name, char *out, size_t outsz,
                         const char **err) {
    if (outsz < 2) {
        *err = "path too long";
        return false;
    }
    size_t len = 0;
    out[0] = '\0';
    const char *p = name;
    while (*p) {
        while (*p == '/')
            p++;
        if (!*p)
            break;
        const char *seg = p;
        while (*p && *p != '/')
            p++;
        size_t sl = (size_t)(p - seg);
        if (sl == 1 && seg[0] == '.')
            continue;
        if (sl == 2 && seg[0] == '.' && seg[1] == '.') {
            *err = "path traversal ('..') not allowed";
            return false;
        }
        for (size_t i = 0; i < sl; i++) {
            unsigned char c = (unsigned char)seg[i];
            if (c < 0x20 || c == 0x7f || c == '\\') {
                *err = "invalid character in path";
                return false;
            }
        }
        if (len + 1 + sl + 1 > outsz) {
            *err = "path too long";
            return false;
        }
        out[len++] = '/';
        memcpy(out + len, seg, sl);
        len += sl;
        out[len] = '\0';
    }
    if (len == 0) {
        out[0] = '/';
        out[1] = '\0';
    }
    return true;
}
