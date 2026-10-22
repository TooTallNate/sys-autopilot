#pragma once

// Minimal POSIX ustar (tar) header helpers used by the save-data export and
// import endpoints. Pure logic (no libnx), so it is unit-tested on the host.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define USTAR_BLOCK 512

// Longest entry name a ustar header can carry (155-byte prefix + '/' + 100).
#define USTAR_NAME_MAX 256

// Bytes of zero padding that follow an entry's data to reach a block boundary.
static inline uint64_t ustar_padding(uint64_t size) {
    return (USTAR_BLOCK - (size % USTAR_BLOCK)) % USTAR_BLOCK;
}

// Builds a ustar header for a regular file (is_dir=false) or directory
// (is_dir=true; name should end in '/'). name is relative ("dir/file").
// mode 0644/0755, uid/gid/mtime 0. Returns false when name is empty or does
// not fit ustar's 100-byte name + 155-byte prefix split, or when size does not
// fit the 11-digit octal size field (>= 8 GiB).
bool ustar_build_header(char hdr[USTAR_BLOCK], const char *name, uint64_t size,
                        bool is_dir);

// True when the block is all zeros (end-of-archive marker).
bool ustar_is_zero_block(const char hdr[USTAR_BLOCK]);

typedef enum {
    USTAR_ENTRY_FILE,     // regular file: `size` data bytes follow
    USTAR_ENTRY_DIR,      // directory
    USTAR_ENTRY_SKIP,     // link/device/fifo/pax header: skip `size` bytes
} UstarEntryType;

typedef struct {
    UstarEntryType type;
    char     name[USTAR_NAME_MAX + 1]; // raw name (prefix "/" name), NUL-terminated
    uint64_t size;                     // data bytes that follow the header
} UstarEntry;

// Parses and validates one (non-zero) header block: checksum, octal fields,
// and entry type. GNU long-name/long-link entries ('L'/'K') and base-256
// sizes are rejected. Returns false with *err set on an invalid header.
bool ustar_parse_header(const char hdr[USTAR_BLOCK], UstarEntry *out,
                        const char **err);

// Turns an archive entry name into an absolute path for the target
// filesystem: "a/b", "./a/b", "/a/b/" -> "/a/b". Empty and "." segments are
// dropped; a ".." segment, a backslash, or a control character is rejected
// (path traversal / ambiguous names). A name that reduces to the root (e.g.
// "./") yields "/". Returns false with *err set on rejection or overflow.
bool ustar_sanitize_path(const char *name, char *out, size_t outsz,
                         const char **err);
