#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Enumerate installed applications (titles) from NCM content-meta databases,
// resolving display names from each title's control data (NACP) via ns.
// Also uninstalls applications via ns. No NPDM changes beyond the ncm/ns access
// the installer already declares.

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

// Parses a title id as printed by GET /titles: exactly 16 hex digits (an
// optional "0x" prefix is accepted). Returns false if malformed or zero.
bool titles_parse_id(const char *s, uint64_t *out);

// Permanently uninstalls an application: its base game, updates, and DLC on
// every storage, plus its application record (removed from the HOME menu).
// Save data is kept. On failure writes a short message to err and returns
// false. (Host build: a stub that succeeds without doing anything.)
bool titles_uninstall(uint64_t title_id, char *err, size_t errsz);
