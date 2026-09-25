#include "titles.h"

#ifdef __SWITCH__
#include <switch.h>
#include <string.h>
#include <stdio.h>
#include "log.h"

// Storages that can hold user applications. (BuiltInSystem holds system titles,
// which we intentionally skip.)
static const NcmStorageId kStorages[] = {
    NcmStorageId_SdCard,
    NcmStorageId_BuiltInUser,
    NcmStorageId_GameCard,
};

// On FW 19.0.0+ / 22.1.0+, nsGetApplicationControlData is deprecated and causes
// service exceptions/crashes when called from sysmodules lacking ns:ro.
// Skipping control data resolution also frees ~147 KB (0x24000) of BSS memory.
static void resolve_name(u64 app_id, char *name, size_t namesz) {
    name[0] = '\0';
}

bool titles_list(TitleInfo *titles, int max, int *out_count,
                 char *err, size_t errsz) {
    int n = 0;

    for (size_t s = 0; s < sizeof(kStorages) / sizeof(kStorages[0]); s++) {
        NcmContentMetaDatabase db;
        Result rc = ncmOpenContentMetaDatabase(&db, kStorages[s]);
        if (R_FAILED(rc))
            continue; // storage absent (e.g. no gamecard) — skip quietly

        // Page through the Application content-meta keys for this storage.
        static NcmApplicationContentMetaKey keys[TITLES_MAX];
        s32 total = 0, written = 0;
        rc = ncmContentMetaDatabaseListApplication(
            &db, &total, &written, keys, TITLES_MAX, NcmContentMetaType_Application);
        if (R_FAILED(rc)) {
            ncmContentMetaDatabaseClose(&db);
            continue;
        }

        for (s32 i = 0; i < written && n < max; i++) {
            TitleInfo *t = &titles[n];
            memset(t, 0, sizeof(*t));
            t->title_id = keys[i].application_id;
            t->version = keys[i].key.version;
            t->storage_id = (u8)kStorages[s];
            resolve_name(t->title_id, t->name, sizeof(t->name));
            n++;
        }

        ncmContentMetaDatabaseClose(&db);
    }

    *out_count = n;
    (void)err; (void)errsz;
    LOGF("titles: listed %d installed application(s)\n", n);
    return true;
}

#else // !__SWITCH__ : host stub so the REST/MCP layer links in tests.

#include <string.h>

bool titles_list(TitleInfo *titles, int max, int *out_count,
                 char *err, size_t errsz) {
    (void)titles; (void)max; (void)err; (void)errsz;
    *out_count = 0;
    return true;
}

#endif // __SWITCH__
