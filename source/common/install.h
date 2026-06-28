#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Streamed title installer. Reads a PFS0/NSP or HFS0/XCI from a byte stream
// (the HTTP request body) and installs it into NCM content storage + registers
// it so the title appears on the HOME menu. Uncompressed containers only
// (no NSZ/NCZ; decompress those on the host first).

// Storage target.
typedef enum {
    INSTALL_STORAGE_SD = 0,    // NcmStorageId_SdCard
    INSTALL_STORAGE_NAND = 1,  // NcmStorageId_BuiltInUser
} InstallStorage;

// Pull callback: read up to `len` bytes of the NSP stream into `buf`. Returns
// the number of bytes read (>0), 0 on clean end-of-stream, or -1 on error.
// The installer calls this strictly sequentially (forward-only).
typedef long (*InstallReadFn)(void *ctx, void *buf, size_t len);

typedef struct {
    bool ok;
    char message[256];   // human-readable result or error detail
    uint64_t title_id;   // installed application/base id (0 if unknown)
    uint32_t version;    // content-meta version
    int http_status;     // suggested HTTP status (200 / 4xx / 5xx)
} InstallResult;

// --- PFS0 parsing (pure, host-testable) --------------------------------------

#define PFS0_MAGIC 0x30534650u // "PFS0"

typedef struct {
    char     name[256];
    uint64_t offset; // offset of file data from the start of the data section
    uint64_t size;
} Pfs0Entry;

// Parses a PFS0 header blob (header + file table + string table). `buf` must
// contain at least the full header region. On success fills entries[] (up to
// max_entries) and sets *out_count and *out_data_start (byte offset where file
// data begins, relative to the start of the PFS0). Returns false on malformed
// input. Pure function — no Switch dependencies.
bool pfs0_parse_header(const uint8_t *buf, size_t buf_len,
                       Pfs0Entry *entries, int max_entries, int *out_count,
                       uint64_t *out_data_start, const char **err);

// Returns the number of header bytes needed to parse the file table+string
// table given the first 16 bytes (the fixed PFS0 header). Returns 0 if `buf16`
// isn't a valid PFS0 header.
size_t pfs0_header_size(const uint8_t *buf16);

// --- HFS0 parsing (pure, host-testable) --------------------------------------
// HFS0 ("Hashed FileSystem") is the partition format inside an XCI gamecard
// image. Same header layout as PFS0 (magic/count/strtab/pad) but file entries
// are 0x40 bytes (offset/size/name_offset/hash_size/pad + 0x20 hash) instead of
// PFS0's 0x18.

#define HFS0_MAGIC 0x30534648u // "HFS0"

typedef struct {
    char     name[256];
    uint64_t offset; // offset of file data from the start of the data section
    uint64_t size;
} Hfs0Entry;

// Returns the number of header bytes needed to parse the file table+string
// table given the first 16 bytes (the fixed HFS0 header). Returns 0 if `buf16`
// isn't a valid HFS0 header.
size_t hfs0_header_size(const uint8_t *buf16);

// Parses an HFS0 header blob (header + file table + string table). `buf` must
// contain at least the full header region. On success fills entries[] (up to
// max_entries) and sets *out_count and *out_data_start (byte offset where file
// data begins, relative to the start of the HFS0). Returns false on malformed
// input. Pure function — no Switch dependencies.
bool hfs0_parse_header(const uint8_t *buf, size_t buf_len,
                       Hfs0Entry *entries, int max_entries, int *out_count,
                       uint64_t *out_data_start, const char **err);

// --- XCI layout detection (pure) ---------------------------------------------

#define XCI_HEAD_MAGIC 0x48454144u // "HEAD" read big-endian (bytes H,E,A,D)

// Given the first bytes of a stream, detects the container. Sets *out_xci_root
// to the absolute byte offset of the root HFS0 for XCI (0xF000 trimmed /
// 0x10000 full). Returns one of CONTAINER_*.
typedef enum {
    CONTAINER_UNKNOWN = 0,
    CONTAINER_NSP = 1,   // PFS0 at offset 0
    CONTAINER_XCI = 2,   // gamecard image (HEAD magic at 0x100 or 0x1100)
} ContainerKind;

// Needs at least 0x1104 bytes to distinguish trimmed vs full XCI; with fewer
// bytes it can still detect NSP (PFS0@0) and trimmed XCI (HEAD@0x100).
ContainerKind container_detect(const uint8_t *buf, size_t len,
                               uint64_t *out_xci_root);

// --- installer (Switch only) -------------------------------------------------

#ifdef __SWITCH__
// Initializes ncm/ns/es services for installation. Call from __appInit while
// the sm session is open. Returns true if installation is available.
bool install_init(void);
void install_exit(void);

// Installs a streamed title (NSP or XCI; auto-detected from the stream's magic)
// via read_fn(ctx,...). total_size is the full stream length (from
// Content-Length) and must be accurate. Fills *out. Returns out->ok. Performs
// rollback of partially-written content on failure.
bool install_stream(InstallReadFn read_fn, void *ctx, uint64_t total_size,
                    InstallStorage storage, InstallResult *out);
#endif
