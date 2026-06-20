#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Streamed NSP title installer. Reads a PFS0/NSP from a byte stream (the HTTP
// request body) and installs it into NCM content storage + registers it so the
// title appears on the HOME menu. NSP only for now (no NSZ/NCZ).

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

// --- installer (Switch only) -------------------------------------------------

#ifdef __SWITCH__
// Initializes ncm/ns/es services for installation. Call from __appInit while
// the sm session is open. Returns true if installation is available.
bool install_init(void);
void install_exit(void);

// Installs an NSP streamed via read_fn(ctx,...). total_size is the full stream
// length (from Content-Length) and must be accurate. Fills *out. Returns
// out->ok. Performs rollback of partially-written content on failure.
bool install_nsp_stream(InstallReadFn read_fn, void *ctx, uint64_t total_size,
                        InstallStorage storage, InstallResult *out);
#endif
