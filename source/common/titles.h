#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Enumerate installed applications (titles) from NCM content-meta databases,
// resolving display names from each title's control data (NACP) via ns.
// Read-only — no NPDM changes beyond the ncm/ns access the installer already
// declares.

#define TITLES_MAX 256

typedef struct {
    uint64_t title_id;     // application (base) id
    uint32_t version;      // content-meta version
    uint8_t  storage_id;   // NcmStorageId (2=GameCard, 4=NAND, 5=SD)
    char     name[80];     // display name (NACP), may be empty if unavailable
} TitleInfo;

// Lists installed applications across the SD card, internal (NAND user), and
// gamecard storages. Fills titles[] up to max, sets *out_count. Returns true on
// success (an empty list is still success). On failure writes a short message
// to err. (Host build: a stub that returns an empty list.)
bool titles_list(TitleInfo *titles, int max, int *out_count,
                 char *err, size_t errsz);
