// Host unit tests for the pure PFS0 header parser (install.c).
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include "install.h"

static void w32(uint8_t *p, uint32_t v) {
    p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24;
}
static void w64(uint8_t *p, uint64_t v) {
    w32(p, (uint32_t)v); w32(p + 4, (uint32_t)(v >> 32));
}

// Builds a minimal PFS0 header (no file data) into buf, returns its size.
static size_t build_pfs0(uint8_t *buf, const char **names, const uint64_t *offs,
                         const uint64_t *sizes, int count) {
    // Build string table first.
    char strtab[256];
    uint32_t name_offs[16];
    size_t st = 0;
    for (int i = 0; i < count; i++) {
        name_offs[i] = (uint32_t)st;
        size_t l = strlen(names[i]);
        memcpy(strtab + st, names[i], l + 1);
        st += l + 1;
    }
    // Pad string table to 0x10 alignment (real NSPs align, not required here).
    uint32_t strtab_size = (uint32_t)st;

    w32(buf + 0, 0x30534650);   // PFS0
    w32(buf + 4, (uint32_t)count);
    w32(buf + 8, strtab_size);
    w32(buf + 12, 0);
    size_t pos = 0x10;
    for (int i = 0; i < count; i++) {
        w64(buf + pos, offs[i]);
        w64(buf + pos + 8, sizes[i]);
        w32(buf + pos + 16, name_offs[i]);
        w32(buf + pos + 20, 0);
        pos += 0x18;
    }
    memcpy(buf + pos, strtab, strtab_size);
    pos += strtab_size;
    return pos;
}

static void test_basic(void) {
    uint8_t buf[512];
    const char *names[] = {
        "abc0123456789abcdef0123456789abc.nca",
        "def0123456789abcdef0123456789abc.cnmt.nca",
        "00112233445566778899aabbccddeeff.tik",
    };
    uint64_t offs[]  = { 0, 0x1000, 0x1800 };
    uint64_t sizes[] = { 0x1000, 0x800, 0x2c0 };
    size_t hsize = build_pfs0(buf, names, offs, sizes, 3);

    assert(pfs0_header_size(buf) == hsize);

    Pfs0Entry e[8];
    int count = 0;
    uint64_t data_start = 0;
    const char *err = NULL;
    bool ok = pfs0_parse_header(buf, hsize, e, 8, &count, &data_start, &err);
    assert(ok);
    assert(count == 3);
    assert(data_start == hsize);
    assert(strcmp(e[0].name, names[0]) == 0 && e[0].offset == 0 && e[0].size == 0x1000);
    assert(strcmp(e[1].name, names[1]) == 0 && e[1].offset == 0x1000 && e[1].size == 0x800);
    assert(strcmp(e[2].name, names[2]) == 0 && e[2].offset == 0x1800 && e[2].size == 0x2c0);
    printf("  basic parse: ok\n");
}

static void test_bad_magic(void) {
    uint8_t buf[64] = {0};
    memcpy(buf, "XXXX", 4);
    assert(pfs0_header_size(buf) == 0);
    Pfs0Entry e[4]; int c; uint64_t ds; const char *err = NULL;
    assert(!pfs0_parse_header(buf, sizeof(buf), e, 4, &c, &ds, &err));
    printf("  bad magic rejected: ok\n");
}

static void test_too_many(void) {
    uint8_t buf[512];
    const char *names[] = { "a.nca", "b.nca", "c.nca" };
    uint64_t offs[] = {0, 1, 2}, sizes[] = {1, 1, 1};
    size_t hsize = build_pfs0(buf, names, offs, sizes, 3);
    Pfs0Entry e[2]; int c; uint64_t ds; const char *err = NULL;
    // max_entries=2 < 3 files -> reject.
    assert(!pfs0_parse_header(buf, hsize, e, 2, &c, &ds, &err));
    printf("  too-many-files rejected: ok\n");
}

static void test_short(void) {
    uint8_t buf[8] = {0};
    Pfs0Entry e[4]; int c; uint64_t ds; const char *err = NULL;
    assert(!pfs0_parse_header(buf, 8, e, 4, &c, &ds, &err));
    printf("  short header rejected: ok\n");
}

int main(void) {
    printf("== test_install ==\n");
    test_basic();
    test_bad_magic();
    test_too_many();
    test_short();
    printf("test_install: all passed\n");
    return 0;
}
