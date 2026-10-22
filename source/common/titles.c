#include "titles.h"

#ifdef __SWITCH__
#include <switch.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "json.h"
#include "log.h"

// Storages that can hold user applications. (BuiltInSystem holds system titles,
// which we intentionally skip.)
static const NcmStorageId kStorages[] = {
    NcmStorageId_SdCard,
    NcmStorageId_BuiltInUser,
    NcmStorageId_GameCard,
};

// Initial key-buffer size per storage; grown on demand (the list is unbounded).
#define KEYS_INITIAL 256
// Sanity cap on keys per storage, so a misbehaving service can't make us grow
// forever.
#define KEYS_MAX     65536

typedef struct {
    u64 app_id;
    u32 version;
    u8  storage_id;
} TitleKey;

// Resolve a title's display name from its control data (NACP). Best-effort:
// leaves name empty on any failure. Uses a single large static buffer
// (control data is ~0x24000 bytes: NACP + icon).
static void resolve_name(u64 app_id, char *name, size_t namesz) {
    static NsApplicationControlData ctrl; // ~0x24000, in BSS
    name[0] = '\0';
    u64 actual = 0;
    Result rc = nsGetApplicationControlData(NsApplicationControlSource_Storage,
                                            app_id, &ctrl, sizeof(ctrl), &actual);
    if (R_FAILED(rc) || actual < sizeof(ctrl.nacp))
        return;
    NacpLanguageEntry *le = NULL;
    // The NACP name field is fixed-size (0x200) and not guaranteed NUL-
    // terminated within our smaller buffer, so bound the copy explicitly.
    if (R_SUCCEEDED(nsGetApplicationDesiredLanguage(&ctrl.nacp, &le)) && le)
        snprintf(name, namesz, "%.*s", (int)namesz - 1, le->name);
    else if (R_SUCCEEDED(nacpGetLanguageEntry(&ctrl.nacp, &le)) && le)
        snprintf(name, namesz, "%.*s", (int)namesz - 1, le->name);
    // The byte-bounded copy can split a multi-byte character (e.g. long
    // Japanese names); drop the partial tail so the name stays valid UTF-8.
    utf8_trim_partial(name);
}

// Appends every Application content-meta key on one storage to *keys/*n.
// Absent storages (e.g. no gamecard) are skipped quietly. Returns false only
// on out-of-memory.
static bool collect_storage(NcmStorageId sid, TitleKey **keys, int *n, int *cap) {
    NcmContentMetaDatabase db;
    if (R_FAILED(ncmOpenContentMetaDatabase(&db, sid)))
        return true;

    // ListApplication has no offset: it writes up to `count` keys and reports
    // the total, so retry with a buffer big enough for everything. Also grow
    // when the buffer comes back exactly full, in case total isn't reported.
    NcmApplicationContentMetaKey *buf = NULL;
    s32 bufcap = KEYS_INITIAL, total = 0, written = 0;
    bool ok = true;
    for (;;) {
        NcmApplicationContentMetaKey *nb = realloc(buf, (size_t)bufcap * sizeof(*buf));
        if (!nb) {
            ok = false;
            break;
        }
        buf = nb;
        total = written = 0;
        Result rc = ncmContentMetaDatabaseListApplication(
            &db, &total, &written, buf, bufcap, NcmContentMetaType_Application);
        if (R_FAILED(rc)) {
            written = 0;
            break;
        }
        if (written < bufcap || bufcap >= KEYS_MAX)
            break;
        s32 want = total > bufcap ? total : bufcap * 2;
        bufcap = want > KEYS_MAX ? KEYS_MAX : want;
    }

    if (ok && written > 0) {
        if (*n + written > *cap) {
            int newcap = *n + written;
            TitleKey *nk = realloc(*keys, (size_t)newcap * sizeof(**keys));
            if (nk) {
                *keys = nk;
                *cap = newcap;
            } else {
                ok = false;
            }
        }
        for (s32 i = 0; ok && i < written; i++) {
            TitleKey *k = &(*keys)[(*n)++];
            k->app_id = buf[i].application_id;
            k->version = buf[i].key.version;
            k->storage_id = (u8)sid;
        }
    }

    free(buf);
    ncmContentMetaDatabaseClose(&db);
    return ok;
}

bool titles_foreach(int offset, int limit, TitlesVisitFn fn, void *ctx,
                    int *out_total, char *err, size_t errsz) {
    if (errsz)
        err[0] = '\0';
    TitleKey *keys = NULL;
    int n = 0, cap = 0;
    for (size_t s = 0; s < sizeof(kStorages) / sizeof(kStorages[0]); s++) {
        if (!collect_storage(kStorages[s], &keys, &n, &cap)) {
            free(keys);
            snprintf(err, errsz, "out of memory listing titles");
            return false;
        }
    }
    if (out_total)
        *out_total = n;
    LOGF("titles: %d installed application(s)\n", n);

    if (offset < 0)
        offset = 0;
    int end = (limit < 0 || offset >= n || limit > n - offset) ? n : offset + limit;
    bool ok = true;
    for (int i = offset; i < end; i++) {
        TitleInfo t;
        memset(&t, 0, sizeof(t));
        t.title_id = keys[i].app_id;
        t.version = keys[i].version;
        t.storage_id = keys[i].storage_id;
        resolve_name(t.title_id, t.name, sizeof(t.name));
        if (!fn(&t, ctx)) {
            ok = false;
            break;
        }
    }
    free(keys);
    return ok;
}

#else // !__SWITCH__ : host stub so the REST/MCP layer links in tests.

#include <string.h>
#include <stdio.h>

int titles_stub_count;

bool titles_foreach(int offset, int limit, TitlesVisitFn fn, void *ctx,
                    int *out_total, char *err, size_t errsz) {
    if (errsz)
        err[0] = '\0';
    int n = titles_stub_count;
    if (out_total)
        *out_total = n;
    if (offset < 0)
        offset = 0;
    int end = (limit < 0 || offset >= n || limit > n - offset) ? n : offset + limit;
    for (int i = offset; i < end; i++) {
        TitleInfo t;
        memset(&t, 0, sizeof(t));
        t.title_id = 0x0100000000010000ULL + ((uint64_t)i << 16);
        t.storage_id = 5;
        snprintf(t.name, sizeof(t.name), "Title %d", i);
        if (!fn(&t, ctx))
            return false;
    }
    return true;
}

#endif // __SWITCH__
