#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Enumerate installed applications (titles) from NCM content-meta databases,
// resolving display names from each title's control data (NACP) via ns.
// Read-only — no NPDM changes beyond the ncm/ns access the installer already
// declares.

typedef struct {
    uint64_t title_id;     // application (base) id
    uint32_t version;      // content-meta version
    uint8_t  storage_id;   // NcmStorageId (2=GameCard, 4=NAND, 5=SD)
    char     name[80];     // display name (NACP, valid UTF-8), may be empty
} TitleInfo;

// Called once per listed title, in order. Return false to stop early (e.g.
// the caller ran out of memory); titles_foreach then returns false with err
// left empty.
typedef bool (*TitlesVisitFn)(const TitleInfo *t, void *ctx);

// Lists installed applications across the SD card, internal (NAND user), and
// gamecard storages, with no upper bound on the count. Visits the entries in
// [offset, offset + limit) (limit < 0 = through the end); display names are
// resolved only for visited entries. *out_total (optional) receives the total
// number of installed applications regardless of the window. Returns true on
// success (an empty list is still success). On failure writes a short message
// to err. (Host build: a stub listing `titles_stub_count` synthetic titles.)
bool titles_foreach(int offset, int limit, TitlesVisitFn fn, void *ctx,
                    int *out_total, char *err, size_t errsz);

#ifndef __SWITCH__
extern int titles_stub_count; // host tests: how many fake titles to list
#endif
