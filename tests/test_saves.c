// Host unit tests for the save-data helpers: ustar header build/parse, archive
// entry path sanitizing, and save id / uid parsing.
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "saves.h"
#include "ustar.h"

static void fix_checksum(char hdr[USTAR_BLOCK]) {
    memset(hdr + 148, ' ', 8);
    unsigned sum = 0;
    for (int i = 0; i < USTAR_BLOCK; i++)
        sum += (unsigned char)hdr[i];
    snprintf(hdr + 148, 8, "%06o", sum);
    hdr[155] = ' ';
}

static void test_build_parse_roundtrip(void) {
    char hdr[USTAR_BLOCK];
    UstarEntry e;
    const char *err = NULL;

    assert(ustar_build_header(hdr, "save_data/slot1.bin", 12345, false));
    assert(memcmp(hdr + 257, "ustar", 6) == 0);
    assert(hdr[156] == '0');
    assert(strcmp(hdr + 124, "00000030071") == 0); // 12345 in octal
    assert(ustar_parse_header(hdr, &e, &err));
    assert(e.type == USTAR_ENTRY_FILE);
    assert(e.size == 12345);
    assert(strcmp(e.name, "save_data/slot1.bin") == 0);

    assert(ustar_build_header(hdr, "save_data/", 999, true));
    assert(ustar_parse_header(hdr, &e, &err));
    assert(e.type == USTAR_ENTRY_DIR && e.size == 0);
    assert(strcmp(e.name, "save_data/") == 0);

    // Empty names and >= 8 GiB files are refused.
    assert(!ustar_build_header(hdr, "", 0, false));
    assert(!ustar_build_header(hdr, "big", 8ULL << 30, false));
    assert(ustar_build_header(hdr, "big", (8ULL << 30) - 1, false));
    printf("ustar roundtrip ok\n");
}

static void test_long_names(void) {
    char hdr[USTAR_BLOCK], name[300];
    UstarEntry e;
    const char *err = NULL;

    // 150-char directory + "/" + 80-char file: needs the prefix split.
    memset(name, 'd', 150);
    name[150] = '/';
    memset(name + 151, 'f', 80);
    name[231] = '\0';
    assert(ustar_build_header(hdr, name, 1, false));
    assert(hdr[345] == 'd' && hdr[0] == 'f');
    assert(ustar_parse_header(hdr, &e, &err));
    assert(strcmp(e.name, name) == 0);

    // A 101-char final component can't be split.
    memset(name, 'x', 101);
    name[101] = '\0';
    assert(!ustar_build_header(hdr, name, 1, false));

    // Exactly 100 chars fits in the name field without a NUL.
    memset(name, 'y', 100);
    name[100] = '\0';
    assert(ustar_build_header(hdr, name, 1, false));
    assert(ustar_parse_header(hdr, &e, &err));
    assert(strcmp(e.name, name) == 0);

    // A long directory name: the trailing '/' must not become the split point.
    memset(name, 'a', 60);
    name[60] = '/';
    memset(name + 61, 'b', 60);
    name[121] = '/';
    name[122] = '\0';
    assert(ustar_build_header(hdr, name, 0, true));
    assert(ustar_parse_header(hdr, &e, &err));
    assert(strcmp(e.name, name) == 0);
    printf("ustar long names ok\n");
}

static void test_parse_rejects(void) {
    char hdr[USTAR_BLOCK];
    UstarEntry e;
    const char *err = NULL;

    // Corrupted byte -> checksum mismatch.
    assert(ustar_build_header(hdr, "file", 10, false));
    hdr[0] = 'g';
    assert(!ustar_parse_header(hdr, &e, &err) && err);

    // Random non-tar data.
    memset(hdr, 'A', sizeof(hdr));
    assert(!ustar_parse_header(hdr, &e, &err));

    // GNU long name entries are refused with a clear error.
    assert(ustar_build_header(hdr, "././@LongLink", 300, false));
    hdr[156] = 'L';
    fix_checksum(hdr);
    err = NULL;
    assert(!ustar_parse_header(hdr, &e, &err) && strstr(err, "long-name"));

    // Base-256 size (GNU large-file extension) is refused.
    assert(ustar_build_header(hdr, "file", 10, false));
    memset(hdr + 124, 0, 12);
    hdr[124] = (char)0x80;
    hdr[135] = 1;
    fix_checksum(hdr);
    assert(!ustar_parse_header(hdr, &e, &err));

    // Garbage inside an octal field.
    assert(ustar_build_header(hdr, "file", 10, false));
    memcpy(hdr + 124, "0000001x012\0", 12);
    fix_checksum(hdr);
    assert(!ustar_parse_header(hdr, &e, &err));

    // Space-padded octal (as some tars write it) is accepted.
    assert(ustar_build_header(hdr, "file", 10, false));
    memcpy(hdr + 124, "         12 ", 12);
    fix_checksum(hdr);
    assert(ustar_parse_header(hdr, &e, &err) && e.size == 10);
    printf("ustar rejects ok\n");
}

static void test_parse_types(void) {
    char hdr[USTAR_BLOCK];
    UstarEntry e;
    const char *err = NULL;

    // pax extended header: skip its data.
    assert(ustar_build_header(hdr, "PaxHeader/file", 42, false));
    hdr[156] = 'x';
    fix_checksum(hdr);
    assert(ustar_parse_header(hdr, &e, &err));
    assert(e.type == USTAR_ENTRY_SKIP && e.size == 42);

    // Symlink: skipped, carries no data even if size is set.
    hdr[156] = '2';
    fix_checksum(hdr);
    assert(ustar_parse_header(hdr, &e, &err));
    assert(e.type == USTAR_ENTRY_SKIP && e.size == 0);

    // Old-style regular file type '\0'.
    assert(ustar_build_header(hdr, "old", 5, false));
    hdr[156] = '\0';
    fix_checksum(hdr);
    assert(ustar_parse_header(hdr, &e, &err) && e.type == USTAR_ENTRY_FILE);

    // GNU magic ("ustar  "): the prefix area is not a name prefix.
    assert(ustar_build_header(hdr, "gnu", 5, false));
    memcpy(hdr + 257, "ustar  ", 8);
    memcpy(hdr + 345, "junk", 4);
    fix_checksum(hdr);
    assert(ustar_parse_header(hdr, &e, &err));
    assert(strcmp(e.name, "gnu") == 0);
    printf("ustar types ok\n");
}

static void test_padding_zero(void) {
    assert(ustar_padding(0) == 0);
    assert(ustar_padding(1) == 511);
    assert(ustar_padding(512) == 0);
    assert(ustar_padding(513) == 511);
    char blk[USTAR_BLOCK] = {0};
    assert(ustar_is_zero_block(blk));
    blk[511] = 1;
    assert(!ustar_is_zero_block(blk));
    printf("ustar padding ok\n");
}

static void test_sanitize(void) {
    char out[64];
    const char *err = NULL;
    struct { const char *in, *out; } ok[] = {
        { "a/b",        "/a/b" },
        { "./a/b",      "/a/b" },
        { "/a//b/",     "/a/b" },
        { "a/./b",      "/a/b" },
        { "./",         "/" },
        { "",           "/" },
        { "dir/",       "/dir" },
        { "a..b/c..",   "/a..b/c.." }, // ".." only rejected as a whole segment
        { "...",        "/..." },
    };
    for (size_t i = 0; i < sizeof(ok) / sizeof(ok[0]); i++) {
        assert(ustar_sanitize_path(ok[i].in, out, sizeof(out), &err));
        assert(strcmp(out, ok[i].out) == 0);
    }
    const char *bad[] = { "../x", "a/../../x", "a/..", "/..", "a\\b", "a\nb", "a\x7f" };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        err = NULL;
        assert(!ustar_sanitize_path(bad[i], out, sizeof(out), &err) && err);
    }
    // Overflow is an error, not a truncation.
    char small[6];
    assert(ustar_sanitize_path("abcd", small, sizeof(small), &err));
    assert(!ustar_sanitize_path("abcde", small, sizeof(small), &err));
    printf("path sanitize ok\n");
}

static void test_ids(void) {
    uint64_t v;
    assert(saves_parse_hex64("8000000000000010", &v) && v == 0x8000000000000010ULL);
    assert(saves_parse_hex64("0x01007EF00011E000", &v) && v == 0x01007EF00011E000ULL);
    assert(saves_parse_hex64("1f", &v) && v == 0x1f);
    assert(!saves_parse_hex64("", &v));
    assert(!saves_parse_hex64("0x", &v));
    assert(!saves_parse_hex64("12345678901234567", &v)); // 17 digits
    assert(!saves_parse_hex64("12g4", &v));
    assert(!saves_parse_hex64("-1", &v));

    uint64_t uid[2];
    const char *s = "1001b2c3d4e5f60718293a4b5c6d7e8f";
    assert(saves_parse_uid(s, uid));
    assert(uid[0] == 0x1001b2c3d4e5f607ULL && uid[1] == 0x18293a4b5c6d7e8fULL);
    char back[33];
    saves_format_uid(uid, back);
    assert(strcmp(back, s) == 0);
    assert(!saves_parse_uid("1001b2c3", uid));
    assert(!saves_parse_uid("1001b2c3d4e5f60718293a4b5c6d7e8z", uid));
    assert(!saves_parse_uid("1001b2c3d4e5f60718293a4b5c6d7e8f0", uid));

    assert(strcmp(saves_type_name(SAVE_TYPE_ACCOUNT), "account") == 0);
    assert(strcmp(saves_type_name(SAVE_TYPE_DEVICE), "device") == 0);
    assert(strcmp(saves_type_name(99), "unknown") == 0);
    printf("save ids ok\n");
}

int main(void) {
    test_build_parse_roundtrip();
    test_long_names();
    test_parse_rejects();
    test_parse_types();
    test_padding_zero();
    test_sanitize();
    test_ids();
    printf("all saves tests passed\n");
    return 0;
}
