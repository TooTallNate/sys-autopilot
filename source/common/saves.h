#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "http.h"

// Game save data in the NAND user save space: list, browse, export as a tar,
// read the raw save container, and restore an export into an existing save.
//
// Saves are identified by their 16-hex save data id (from GET /saves), or by
// titleId (+ uid when more than one user has a save for that title). A uid is
// written as 32 hex chars: AccountUid.uid[0] then uid[1], each as a 16-digit
// big-endian hex number (all zeros for saves that are not per-user).
//
// System saves (other system modules' state) are never listed or touched.

// FsSaveDataType values (mirrored so the host build doesn't need libnx).
enum {
    SAVE_TYPE_SYSTEM      = 0,
    SAVE_TYPE_ACCOUNT     = 1,
    SAVE_TYPE_BCAT        = 2,
    SAVE_TYPE_DEVICE      = 3,
    SAVE_TYPE_TEMPORARY   = 4,
    SAVE_TYPE_CACHE       = 5,
    SAVE_TYPE_SYSTEM_BCAT = 6,
};

typedef struct {
    uint64_t save_id;
    uint64_t title_id;   // owning application id
    uint64_t uid[2];     // AccountUid (zero for non-account saves)
    uint64_t size;       // size of the save image on NAND, bytes
    uint16_t index;      // save data index (cache saves)
    uint8_t  type;       // SAVE_TYPE_*
} SaveEntry;

// "account", "bcat", "device", "temporary", "cache", ... ("unknown" otherwise).
const char *saves_type_name(uint8_t type);

// Strict hex parsing for query parameters: 1-16 hex digits (an optional "0x"
// prefix is accepted) for ids, exactly 32 hex digits for a uid.
bool saves_parse_hex64(const char *s, uint64_t *out);
bool saves_parse_uid(const char *s, uint64_t out[2]);

// Formats a uid as 32 lowercase hex chars (inverse of saves_parse_uid).
void saves_format_uid(const uint64_t uid[2], char out[33]);

// Lists the game saves in the user save space (system saves excluded).
// *out is malloc'd (caller frees; may be NULL when *count is 0). Returns false
// with err set on failure. (Host build: a stub that returns an empty list.)
bool saves_list(SaveEntry **out, int *count, char *err, size_t errsz);

// REST handlers (Switch only). Each sends a complete HTTP response.
void saves_handle_list(HttpRequest *req);   // GET /saves
void saves_handle_files(HttpRequest *req);  // GET /saves/files?id=|titleId=[&uid=][&path=/]
void saves_handle_export(HttpRequest *req); // GET /saves/export?id=|titleId=[&uid=]  (ustar)
void saves_handle_raw(HttpRequest *req);    // GET /saves/raw?id=|titleId=[&uid=]     (save container)
void saves_handle_import(HttpRequest *req); // PUT /saves/import?id=|titleId=[&uid=]  (ustar body)
