#include "titles.h"

bool titles_parse_id(const char *s, uint64_t *out) {
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))
        s += 2;
    uint64_t v = 0;
    int n = 0;
    for (; s[n]; n++) {
        char c = s[n];
        int d;
        if (c >= '0' && c <= '9')      d = c - '0';
        else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
        else return false;
        if (n >= 16) return false;
        v = (v << 4) | (uint64_t)d;
    }
    if (n != 16 || v == 0)
        return false;
    *out = v;
    return true;
}

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

bool titles_uninstall(uint64_t title_id, char *err, size_t errsz) {
    // Deletes all of the application's content (base, updates, DLC) across
    // storages and its application record. Save data is left untouched.
    Result rc = nsDeleteApplicationCompletely(title_id);
    if (R_FAILED(rc)) {
        snprintf(err, errsz, "uninstall failed (rc=0x%x)", rc);
        return false;
    }
    LOGF("titles: uninstalled %016llx\n", (unsigned long long)title_id);
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

bool titles_uninstall(uint64_t title_id, char *err, size_t errsz) {
    (void)title_id; (void)err; (void)errsz;
    return true;
}

#endif // __SWITCH__
