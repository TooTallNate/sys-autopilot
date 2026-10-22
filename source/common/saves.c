#include "saves.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// --- pure helpers (host-testable) ---------------------------------------------

const char *saves_type_name(uint8_t type) {
    switch (type) {
    case SAVE_TYPE_SYSTEM:      return "system";
    case SAVE_TYPE_ACCOUNT:     return "account";
    case SAVE_TYPE_BCAT:        return "bcat";
    case SAVE_TYPE_DEVICE:      return "device";
    case SAVE_TYPE_TEMPORARY:   return "temporary";
    case SAVE_TYPE_CACHE:       return "cache";
    case SAVE_TYPE_SYSTEM_BCAT: return "systembcat";
    default:                    return "unknown";
    }
}

static int hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static bool parse_hex_n(const char *s, size_t len, uint64_t *out) {
    uint64_t v = 0;
    for (size_t i = 0; i < len; i++) {
        int h = hexval(s[i]);
        if (h < 0)
            return false;
        v = (v << 4) | (uint64_t)h;
    }
    *out = v;
    return true;
}

bool saves_parse_hex64(const char *s, uint64_t *out) {
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))
        s += 2;
    size_t len = strlen(s);
    return len >= 1 && len <= 16 && parse_hex_n(s, len, out);
}

bool saves_parse_uid(const char *s, uint64_t out[2]) {
    uint64_t hi, lo;
    if (strlen(s) != 32 || !parse_hex_n(s, 16, &hi) || !parse_hex_n(s + 16, 16, &lo))
        return false;
    out[0] = hi;
    out[1] = lo;
    return true;
}

void saves_format_uid(const uint64_t uid[2], char out[33]) {
    snprintf(out, 33, "%016llx%016llx", (unsigned long long)uid[0],
             (unsigned long long)uid[1]);
}

#ifdef __SWITCH__

#include <switch.h>

#include "json.h"
#include "log.h"
#include "ustar.h"

// Save data lives in the NAND user partition as journaled save containers. FS
// presents a mounted save as a plain filesystem, so export/import work on the
// files the game wrote. Everything is streamed through one fixed buffer: the
// server is single-threaded and the sysmodule heap is small.

#define SAVES_IO_BUF 0x8000
static char g_buf[SAVES_IO_BUF];

// FS "TargetLocked" (module 2, description 7 = 0xE02): the save is open
// elsewhere -- by the running application, or held by FS's save data cache,
// which keeps the most recently mounted save open after it is closed.
static bool rc_is_locked(Result rc) {
    return R_MODULE(rc) == 2 && R_DESCRIPTION(rc) == 7;
}

static void send_fs_error(HttpRequest *req, int code, const char *what, Result rc) {
    if (rc_is_locked(rc)) {
        http_send_json(req->fd, 409,
                       "{\"error\":\"save is open elsewhere (the running game, or the save "
                       "most recently mounted); close the game or access another save, then "
                       "retry\",\"rc\":\"0x%x\"}", rc);
        return;
    }
    http_send_json(req->fd, code, "{\"error\":\"%s\",\"rc\":\"0x%x\"}", what, rc);
}

static bool is_game_save(u8 type) {
    return type != FsSaveDataType_System && type != FsSaveDataType_SystemBcat;
}

// Calls fn for each game save in the user save space until it returns false.
typedef bool (*SaveVisitor)(const FsSaveDataInfo *info, void *ctx);

static Result for_each_save(SaveVisitor fn, void *ctx) {
    FsSaveDataInfoReader reader;
    Result rc = fsOpenSaveDataInfoReader(&reader, FsSaveDataSpaceId_User);
    if (R_FAILED(rc))
        return rc;
    static FsSaveDataInfo batch[16];
    bool go = true;
    while (go) {
        s64 got = 0;
        rc = fsSaveDataInfoReaderRead(&reader, batch, 16, &got);
        if (R_FAILED(rc) || got <= 0)
            break;
        for (s64 i = 0; i < got && go; i++)
            if (is_game_save(batch[i].save_data_type))
                go = fn(&batch[i], ctx);
    }
    fsSaveDataInfoReaderClose(&reader);
    return R_FAILED(rc) ? rc : 0;
}

// --- GET /saves / list_saves ----------------------------------------------------

typedef struct {
    SaveEntry *v;
    int n, cap;
    bool oom;
} ListCtx;

static bool list_visit(const FsSaveDataInfo *info, void *p) {
    ListCtx *c = p;
    if (c->n == c->cap) {
        int ncap = c->cap ? c->cap * 2 : 64;
        SaveEntry *nv = realloc(c->v, (size_t)ncap * sizeof(*nv));
        if (!nv) {
            c->oom = true;
            return false;
        }
        c->v = nv;
        c->cap = ncap;
    }
    SaveEntry *e = &c->v[c->n++];
    e->save_id = info->save_data_id;
    e->title_id = info->application_id;
    e->uid[0] = info->uid.uid[0];
    e->uid[1] = info->uid.uid[1];
    e->size = info->size;
    e->index = info->save_data_index;
    e->type = info->save_data_type;
    return true;
}

bool saves_list(SaveEntry **out, int *count, char *err, size_t errsz) {
    ListCtx c = {0};
    Result rc = for_each_save(list_visit, &c);
    if (R_FAILED(rc) || c.oom) {
        free(c.v);
        if (c.oom)
            snprintf(err, errsz, "out of memory listing saves");
        else
            snprintf(err, errsz, "fsOpenSaveDataInfoReader failed (rc=0x%x)", rc);
        return false;
    }
    *out = c.v;
    *count = c.n;
    return true;
}

// Appends to a heap-grown buffer. Returns false on OOM.
static bool buf_append(char **buf, size_t *len, size_t *cap, const char *data, size_t n) {
    if (*len + n + 1 > *cap) {
        size_t ncap = *cap ? *cap : 2048;
        while (*len + n + 1 > ncap)
            ncap *= 2;
        char *nb = realloc(*buf, ncap);
        if (!nb)
            return false;
        *buf = nb;
        *cap = ncap;
    }
    memcpy(*buf + *len, data, n);
    *len += n;
    (*buf)[*len] = '\0';
    return true;
}

void saves_handle_list(HttpRequest *req) {
    SaveEntry *v = NULL;
    int n = 0;
    char err[96];
    if (!saves_list(&v, &n, err, sizeof(err))) {
        http_send_error(req->fd, 500, err);
        return;
    }
    char *json = NULL;
    size_t len = 0, cap = 0;
    bool ok = buf_append(&json, &len, &cap, "{\"saves\":[", 10);
    for (int i = 0; i < n && ok; i++) {
        char uid[33], entry[256];
        saves_format_uid(v[i].uid, uid);
        int m = snprintf(entry, sizeof(entry),
                         "%s{\"id\":\"%016llx\",\"type\":\"%s\",\"titleId\":\"%016llx\","
                         "\"uid\":\"%s\",\"index\":%u,\"size\":%llu}",
                         i ? "," : "", (unsigned long long)v[i].save_id,
                         saves_type_name(v[i].type), (unsigned long long)v[i].title_id,
                         uid, v[i].index, (unsigned long long)v[i].size);
        ok = buf_append(&json, &len, &cap, entry, (size_t)m);
    }
    free(v);
    if (ok)
        ok = buf_append(&json, &len, &cap, "]}", 2);
    if (!ok) {
        free(json);
        http_send_error(req->fd, 500, "out of memory building save list");
        return;
    }
    http_send_response(req->fd, 200, "application/json", json, len);
    free(json);
}

// --- target resolution + mounting ---------------------------------------------

typedef struct {
    u64 id;            // match by save id (when by_id)
    bool by_id;
    u64 title_id;
    u64 uid[2];
    bool have_uid;
    int matches;
    FsSaveDataInfo first;
} FindCtx;

static bool find_visit(const FsSaveDataInfo *info, void *p) {
    FindCtx *c = p;
    if (c->by_id) {
        if (info->save_data_id != c->id)
            return true;
    } else {
        if (info->application_id != c->title_id)
            return true;
        if (c->have_uid) {
            if (info->save_data_type != FsSaveDataType_Account ||
                info->uid.uid[0] != c->uid[0] || info->uid.uid[1] != c->uid[1])
                return true;
        } else if (info->save_data_type != FsSaveDataType_Account &&
                   info->save_data_type != FsSaveDataType_Device) {
            return true; // titleId alone selects the game's account/device save
        }
    }
    if (c->matches++ == 0)
        c->first = *info;
    return !c->by_id;
}

// Resolves ?id= or ?titleId=[&uid=] to one save. Sends an error response and
// returns false on failure.
static bool resolve_target(HttpRequest *req, FsSaveDataInfo *out) {
    char val[48];
    FindCtx c = {0};
    if (http_query_get(req, "id", val, sizeof(val)) && val[0]) {
        if (!saves_parse_hex64(val, &c.id)) {
            http_send_error(req->fd, 400, "id must be a hex save id (from GET /saves)");
            return false;
        }
        c.by_id = true;
    } else if (http_query_get(req, "titleId", val, sizeof(val)) && val[0]) {
        if (!saves_parse_hex64(val, &c.title_id)) {
            http_send_error(req->fd, 400, "titleId must be a hex title id");
            return false;
        }
        if (http_query_get(req, "uid", val, sizeof(val)) && val[0]) {
            if (!saves_parse_uid(val, c.uid)) {
                http_send_error(req->fd, 400, "uid must be 32 hex chars (as GET /saves prints it)");
                return false;
            }
            c.have_uid = true;
        }
    } else {
        http_send_error(req->fd, 400, "pass ?id=<saveId> or ?titleId=<hex>[&uid=<32 hex>]");
        return false;
    }

    Result rc = for_each_save(find_visit, &c);
    if (R_FAILED(rc)) {
        send_fs_error(req, 500, "fsOpenSaveDataInfoReader failed", rc);
        return false;
    }
    if (c.matches == 0) {
        http_send_error(req->fd, 404, c.by_id
            ? "no game save with that id"
            : "no save for that title/user (launch the game once on that user to create it)");
        return false;
    }
    if (c.matches > 1) {
        http_send_error(req->fd, 409, "several saves match that title; pass uid (or id)");
        return false;
    }
    *out = c.first;
    return true;
}

static void attr_from_info(const FsSaveDataInfo *info, FsSaveDataAttribute *attr) {
    memset(attr, 0, sizeof(*attr));
    attr->application_id = info->application_id;
    attr->uid = info->uid;
    attr->system_save_data_id = info->system_save_data_id;
    attr->save_data_type = info->save_data_type;
    attr->save_data_rank = info->save_data_rank;
    attr->save_data_index = info->save_data_index;
}

// Mounts the save read-only (falls back to a normal mount for save types that
// reject the read-only open). Sends an error response on failure.
static bool mount_readonly(HttpRequest *req, const FsSaveDataInfo *info, FsFileSystem *fs) {
    FsSaveDataAttribute attr;
    attr_from_info(info, &attr);
    FsSaveDataSpaceId space = (FsSaveDataSpaceId)info->save_data_space_id;
    Result rc = fsOpenReadOnlySaveDataFileSystem(fs, space, &attr);
    if (R_SUCCEEDED(rc))
        return true;
    if (!rc_is_locked(rc)) {
        Result rc2 = fsOpenSaveDataFileSystem(fs, space, &attr);
        if (R_SUCCEEDED(rc2))
            return true;
        LOGF("saves: mount %016llx failed ro=0x%x rw=0x%x\n",
             (unsigned long long)info->save_data_id, rc, rc2);
        rc = rc2;
    }
    send_fs_error(req, 500, "could not mount save", rc);
    return false;
}

// Streams size bytes of an open file to the client. Returns false on a read
// or write error (the response is then short of its Content-Length).
static bool stream_file(int fd, FsFile *f, s64 size) {
    s64 off = 0;
    while (off < size) {
        u64 n = 0;
        u64 want = (u64)(size - off) > SAVES_IO_BUF ? SAVES_IO_BUF : (u64)(size - off);
        if (R_FAILED(fsFileRead(f, off, g_buf, want, FsReadOption_None, &n)) || n == 0)
            return false;
        if (!http_write_all(fd, g_buf, n))
            return false;
        off += (s64)n;
    }
    return true;
}

// After a failure mid-body the response can't be corrected; make sure the
// connection closes so the client sees a truncated transfer.
static void abort_response(HttpRequest *req, const char *what) {
    LOGF("saves: %s; aborting response\n", what);
    req->keep_alive = false;
}

// --- GET /saves/files ----------------------------------------------------------

static char g_esc[FS_MAX_PATH * 6 + 1];

static void send_dir(HttpRequest *req, FsFileSystem *fs, const char *path) {
    FsDir dir;
    Result rc = fsFsOpenDirectory(fs, path, FsDirOpenMode_ReadDirs | FsDirOpenMode_ReadFiles, &dir);
    if (R_FAILED(rc)) {
        send_fs_error(req, 500, "open directory failed", rc);
        return;
    }
    static FsDirectoryEntry ent;
    char *json = NULL;
    size_t len = 0, cap = 0;
    json_escape(path, strlen(path), g_esc, sizeof(g_esc));
    bool ok = buf_append(&json, &len, &cap, "{\"path\":\"", 9) &&
              buf_append(&json, &len, &cap, g_esc, strlen(g_esc)) &&
              buf_append(&json, &len, &cap, "\",\"entries\":[", 13);
    s64 got = 0;
    bool first = true;
    while (ok && R_SUCCEEDED(fsDirRead(&dir, &got, 1, &ent)) && got > 0) {
        ent.name[sizeof(ent.name) - 1] = '\0';
        if (json_escape(ent.name, strlen(ent.name), g_esc, sizeof(g_esc)) == (size_t)-1)
            continue;
        char tail[64];
        if (ent.type == FsDirEntryType_Dir)
            snprintf(tail, sizeof(tail), "\",\"type\":\"dir\"}");
        else
            snprintf(tail, sizeof(tail), "\",\"type\":\"file\",\"size\":%lld}",
                     (long long)ent.file_size);
        ok = buf_append(&json, &len, &cap, first ? "{\"name\":\"" : ",{\"name\":\"", first ? 9 : 10) &&
             buf_append(&json, &len, &cap, g_esc, strlen(g_esc)) &&
             buf_append(&json, &len, &cap, tail, strlen(tail));
        first = false;
    }
    fsDirClose(&dir);
    if (ok)
        ok = buf_append(&json, &len, &cap, "]}", 2);
    if (!ok) {
        free(json);
        http_send_error(req->fd, 500, "out of memory building listing");
        return;
    }
    http_send_response(req->fd, 200, "application/json", json, len);
    free(json);
}

static void send_save_file(HttpRequest *req, FsFileSystem *fs, const char *path) {
    FsFile f;
    Result rc = fsFsOpenFile(fs, path, FsOpenMode_Read, &f);
    if (R_FAILED(rc)) {
        send_fs_error(req, 500, "open file failed", rc);
        return;
    }
    s64 size = 0;
    rc = fsFileGetSize(&f, &size);
    if (R_FAILED(rc)) {
        fsFileClose(&f);
        send_fs_error(req, 500, "get file size failed", rc);
        return;
    }
    http_send_header(req->fd, 200, "application/octet-stream", (size_t)size);
    if (!stream_file(req->fd, &f, size))
        abort_response(req, "save file read/write failed");
    fsFileClose(&f);
}

void saves_handle_files(HttpRequest *req) {
    char raw[FS_MAX_PATH], path[FS_MAX_PATH];
    if (!http_query_get(req, "path", raw, sizeof(raw)) || !raw[0])
        snprintf(raw, sizeof(raw), "/");
    if (raw[0] != '/') {
        http_send_error(req->fd, 400, "path must be absolute (start with /)");
        return;
    }
    const char *err = NULL;
    if (!ustar_sanitize_path(raw, path, sizeof(path), &err)) {
        http_send_error(req->fd, 400, err);
        return;
    }
    FsSaveDataInfo info;
    FsFileSystem fs;
    if (!resolve_target(req, &info) || !mount_readonly(req, &info, &fs))
        return;
    FsDirEntryType t;
    Result rc = fsFsGetEntryType(&fs, path, &t);
    if (R_FAILED(rc))
        http_send_json(req->fd, 404, "{\"error\":\"no such path in save\",\"rc\":\"0x%x\"}", rc);
    else if (t == FsDirEntryType_Dir)
        send_dir(req, &fs, path);
    else
        send_save_file(req, &fs, path);
    fsFsClose(&fs);
}

// --- GET /saves/export (ustar archive of the whole save) ----------------------
//
// The save is walked twice: a sizing pass computes the exact archive length
// (so the response has a Content-Length and nothing is buffered), then the
// write pass streams it. Anything ustar can't represent fails the request in
// the sizing pass, before any byte is sent, rather than producing a silently
// incomplete archive.

#define EXPORT_MAX_DEPTH 32

typedef struct {
    FsFileSystem *fs;
    int fd;
    bool emit;            // false = sizing pass, true = write pass
    s64 total;            // bytes (sized or sent)
    const char *err;      // first failure
    Result rc;
    char bad[FS_MAX_PATH]; // offending entry
} TarCtx;

static void tar_fail(TarCtx *c, const char *err, Result rc, const char *name) {
    if (c->err)
        return;
    c->err = err;
    c->rc = rc;
    snprintf(c->bad, sizeof(c->bad), "%s", name ? name : "");
}

static void tar_put(TarCtx *c, const void *buf, size_t n) {
    if (c->err)
        return;
    if (c->emit && !http_write_all(c->fd, buf, n)) {
        tar_fail(c, "client write failed", 0, NULL);
        return;
    }
    c->total += (s64)n;
}

static void tar_file(TarCtx *c, const char *fspath, const char *name, s64 size) {
    char hdr[USTAR_BLOCK];
    if (!ustar_build_header(hdr, name, (u64)size, false)) {
        tar_fail(c, "entry name too long (or file too large) for ustar", 0, name);
        return;
    }
    tar_put(c, hdr, sizeof(hdr));
    u64 pad = ustar_padding((u64)size);
    if (!c->emit) {
        c->total += size + (s64)pad;
        return;
    }
    FsFile f;
    Result rc = fsFsOpenFile(c->fs, fspath, FsOpenMode_Read, &f);
    if (R_FAILED(rc)) {
        tar_fail(c, "open file failed", rc, name);
        return;
    }
    s64 off = 0;
    while (off < size && !c->err) {
        u64 n = 0;
        u64 want = (u64)(size - off) > SAVES_IO_BUF ? SAVES_IO_BUF : (u64)(size - off);
        rc = fsFileRead(&f, off, g_buf, want, FsReadOption_None, &n);
        if (R_FAILED(rc) || n == 0) {
            tar_fail(c, "file read failed (size changed?)", rc, name);
            break;
        }
        tar_put(c, g_buf, n);
        off += (s64)n;
    }
    fsFileClose(&f);
    if (pad) {
        memset(g_buf, 0, (size_t)pad);
        tar_put(c, g_buf, (size_t)pad);
    }
}

static void tar_walk(TarCtx *c, const char *dirpath, const char *prefix, int depth) {
    if (depth > EXPORT_MAX_DEPTH) {
        tar_fail(c, "directory tree too deep", 0, prefix);
        return;
    }
    FsDir dir;
    Result rc = fsFsOpenDirectory(c->fs, dirpath, FsDirOpenMode_ReadDirs | FsDirOpenMode_ReadFiles, &dir);
    if (R_FAILED(rc)) {
        tar_fail(c, "open directory failed", rc, prefix);
        return;
    }
    // Per-level heap buffers (FS paths are up to 0x301 bytes; keep them off
    // the small stack).
    FsDirectoryEntry *ent = malloc(sizeof(*ent));
    char *fspath = malloc(FS_MAX_PATH);
    char *name = malloc(FS_MAX_PATH);
    if (!ent || !fspath || !name) {
        tar_fail(c, "out of memory", 0, NULL);
    } else {
        s64 got = 0;
        size_t dl = strlen(dirpath);
        while (!c->err && R_SUCCEEDED(rc = fsDirRead(&dir, &got, 1, ent)) && got > 0) {
            ent->name[sizeof(ent->name) - 1] = '\0';
            bool is_dir = ent->type == FsDirEntryType_Dir;
            if ((size_t)snprintf(fspath, FS_MAX_PATH, "%s%s%s", dirpath,
                                 dirpath[dl - 1] == '/' ? "" : "/", ent->name) >= FS_MAX_PATH ||
                (size_t)snprintf(name, FS_MAX_PATH, "%s%s%s", prefix, ent->name,
                                 is_dir ? "/" : "") >= FS_MAX_PATH) {
                tar_fail(c, "path too long", 0, ent->name);
                break;
            }
            if (is_dir) {
                char hdr[USTAR_BLOCK];
                if (!ustar_build_header(hdr, name, 0, true)) {
                    tar_fail(c, "entry name too long for ustar", 0, name);
                    break;
                }
                tar_put(c, hdr, sizeof(hdr));
                tar_walk(c, fspath, name, depth + 1);
            } else {
                tar_file(c, fspath, name, ent->file_size);
            }
        }
        if (R_FAILED(rc))
            tar_fail(c, "read directory failed", rc, prefix);
    }
    free(ent);
    free(fspath);
    free(name);
    fsDirClose(&dir);
}

void saves_handle_export(HttpRequest *req) {
    FsSaveDataInfo info;
    FsFileSystem fs;
    if (!resolve_target(req, &info) || !mount_readonly(req, &info, &fs))
        return;

    static TarCtx c;
    memset(&c, 0, sizeof(c));
    c.fs = &fs;
    c.fd = req->fd;
    tar_walk(&c, "/", "", 0);
    if (c.err) {
        json_escape(c.bad, strlen(c.bad), g_esc, sizeof(g_esc));
        http_send_json(req->fd, R_FAILED(c.rc) ? 500 : 422,
                       "{\"error\":\"%s\",\"path\":\"%s\",\"rc\":\"0x%x\"}", c.err, g_esc, c.rc);
        fsFsClose(&fs);
        return;
    }
    s64 archive = c.total + 2 * USTAR_BLOCK; // end-of-archive: two zero blocks

    http_send_header(req->fd, 200, "application/x-tar", (size_t)archive);
    c.emit = true;
    c.total = 0;
    tar_walk(&c, "/", "", 0);
    memset(g_buf, 0, 2 * USTAR_BLOCK);
    tar_put(&c, g_buf, 2 * USTAR_BLOCK);
    if (c.err || c.total != archive) {
        LOGF("saves: export %016llx: %s (%s) rc=0x%x sent=%lld of %lld\n",
             (unsigned long long)info.save_data_id, c.err ? c.err : "size changed",
             c.bad, c.rc, (long long)c.total, (long long)archive);
        abort_response(req, "export incomplete");
    }
    fsFsClose(&fs);
}

// --- GET /saves/raw (the save container straight from NAND USER) -------------
//
// Reads /save/<id> through the BIS USER filesystem, which strips the BIS
// encryption: the result is the save container itself (DISF header, journal,
// IVFC hash trees), which hactool/hactoolnet `-t save` can extract. FS keeps
// the file open while the save is mounted (including the cached, most recently
// mounted save), so an open save returns 409 here too.

void saves_handle_raw(HttpRequest *req) {
    FsSaveDataInfo info;
    if (!resolve_target(req, &info))
        return;
    FsFileSystem bis;
    Result rc = fsOpenBisFileSystem(&bis, FsBisPartitionId_User, "");
    if (R_FAILED(rc)) {
        send_fs_error(req, 500, "fsOpenBisFileSystem(User) failed", rc);
        return;
    }
    char path[32];
    snprintf(path, sizeof(path), "/save/%016llx", (unsigned long long)info.save_data_id);
    FsFile f;
    rc = fsFsOpenFile(&bis, path, FsOpenMode_Read, &f);
    if (R_FAILED(rc)) {
        send_fs_error(req, 500, "open raw save failed", rc);
        fsFsClose(&bis);
        return;
    }
    s64 size = 0;
    rc = fsFileGetSize(&f, &size);
    if (R_FAILED(rc)) {
        send_fs_error(req, 500, "get raw save size failed", rc);
    } else {
        http_send_header(req->fd, 200, "application/octet-stream", (size_t)size);
        if (!stream_file(req->fd, &f, size))
            abort_response(req, "raw save read/write failed");
    }
    fsFileClose(&f);
    fsFsClose(&bis);
}

// --- PUT /saves/import (restore a ustar archive into an existing save) -------
//
// The save's contents are replaced by the archive. The save filesystem is
// journaled: nothing is persisted until fsFsCommit. When the whole restore
// fits the save's journal it is committed once at the end, so a failure at any
// point (bad archive, dropped connection, full save) leaves the save exactly
// as it was. A restore larger than the journal must commit along the way
// (writes past the journal capacity fail), so on failure that save may be left
// partially restored; the response says which case applied.

#define SAVE_BLOCK 0x4000 // save FS block size: the journal's unit of cost

static u64 round_block(u64 n) {
    return (n + SAVE_BLOCK - 1) & ~(u64)(SAVE_BLOCK - 1);
}

// Reads exactly n body bytes into buf, or discards them when buf is NULL.
static bool body_exact(HttpRequest *req, void *buf, u64 n) {
    char *b = buf;
    while (n) {
        size_t want = n > SAVES_IO_BUF ? SAVES_IO_BUF : (size_t)n;
        ssize_t r = http_read_body(req, b ? (void *)b : (void *)g_buf, want);
        if (r <= 0)
            return false;
        n -= (u64)r;
        if (b)
            b += r;
    }
    return true;
}

// Reads the next header block. Returns 1 when a block was read, 0 at a clean
// end of the body (Content-Length fully received on a block boundary), and -1
// when the body is truncated or the connection fails -- which must never be
// mistaken for the end of the archive.
static int read_block(HttpRequest *req, char blk[USTAR_BLOCK]) {
    if (req->body_consumed >= req->content_length)
        return 0;
    return body_exact(req, blk, USTAR_BLOCK) ? 1 : -1;
}

// Creates each missing parent directory of path ("/a/b/c" -> /a, /a/b).
static void mkparents(FsFileSystem *fs, const char *path) {
    char tmp[FS_MAX_PATH];
    snprintf(tmp, sizeof(tmp), "%s", path);
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            fsFsCreateDirectory(fs, tmp); // fails harmlessly if it exists
            *p = '/';
        }
    }
}

typedef struct {
    FsFileSystem *fs;
    s64 budget;      // journal bytes usable between commits (0 = unknown)
    s64 pending;     // estimated journal use since the last commit
    int commits;     // intermediate commits (0 = still atomic)
    Result rc;
} ImportCtx;

static bool import_commit(ImportCtx *c) {
    Result rc = fsFsCommit(c->fs);
    if (R_FAILED(rc)) {
        c->rc = rc;
        return false;
    }
    c->commits++;
    c->pending = 0;
    return true;
}

// Commits first if adding cost would overflow the journal budget.
static bool import_reserve(ImportCtx *c, s64 cost) {
    if (c->budget > 0 && c->pending > 0 && c->pending + cost > c->budget)
        if (!import_commit(c))
            return false;
    c->pending += cost;
    return true;
}

static const char *import_file(HttpRequest *req, ImportCtx *c, const char *path, u64 size) {
    mkparents(c->fs, path);
    fsFsDeleteFile(c->fs, path); // duplicate entry in the archive: last wins
    if (!import_reserve(c, SAVE_BLOCK))
        return "commit failed";
    if (R_FAILED(c->rc = fsFsCreateFile(c->fs, path, (s64)size, 0)))
        return "create file failed (save full?)";
    FsFile f;
    if (R_FAILED(c->rc = fsFsOpenFile(c->fs, path, FsOpenMode_Write, &f)))
        return "open file failed";
    const char *err = NULL;
    u64 off = 0;
    while (off < size) {
        size_t want = (size - off) > SAVES_IO_BUF ? SAVES_IO_BUF : (size_t)(size - off);
        if (!body_exact(req, g_buf, want)) {
            err = "request body ended inside a file";
            break;
        }
        // A file bigger than the journal: commit mid-file (with the file
        // closed, as FS requires for commit) and continue.
        if (c->budget > 0 && c->pending > 0 &&
            c->pending + (s64)round_block(want) > c->budget) {
            fsFileClose(&f);
            if (!import_commit(c))
                return "commit failed";
            if (R_FAILED(c->rc = fsFsOpenFile(c->fs, path, FsOpenMode_Write, &f)))
                return "reopen file failed";
        }
        if (R_FAILED(c->rc = fsFileWrite(&f, (s64)off, g_buf, want, FsWriteOption_None))) {
            err = "write failed (save full?)";
            break;
        }
        c->pending += (s64)round_block(want);
        off += want;
    }
    fsFileClose(&f);
    return err;
}

void saves_handle_import(HttpRequest *req) {
    if (!req->has_content_length) {
        http_send_error(req->fd, 411, "Content-Length required (send the tar with curl -T)");
        return;
    }
    FsSaveDataInfo info;
    if (!resolve_target(req, &info))
        return;

    // Validate the first header before touching the save.
    static char hdr[USTAR_BLOCK];
    static UstarEntry ent;
    const char *err = NULL;
    if (read_block(req, hdr) != 1) {
        http_send_error(req->fd, 400, "request body is not a tar archive (too short)");
        return;
    }
    bool at_end = ustar_is_zero_block(hdr);
    if (!at_end && !ustar_parse_header(hdr, &ent, &err)) {
        http_send_error(req->fd, 400, err);
        return;
    }

    // Journal capacity decides whether the restore can be a single commit.
    FsSaveDataExtraData extra;
    s64 journal = 0;
    if (R_SUCCEEDED(fsReadSaveDataFileSystemExtraDataBySaveDataSpaceId(
            &extra, sizeof(extra), (FsSaveDataSpaceId)info.save_data_space_id,
            info.save_data_id)))
        journal = extra.journal_size;

    FsSaveDataAttribute attr;
    attr_from_info(&info, &attr);
    FsFileSystem fs;
    Result rc = fsOpenSaveDataFileSystem(&fs, (FsSaveDataSpaceId)info.save_data_space_id, &attr);
    if (R_FAILED(rc)) {
        send_fs_error(req, 500, "could not open save for writing", rc);
        return;
    }

    // Keep a quarter of the journal as headroom for FS metadata.
    ImportCtx c = { .fs = &fs, .budget = journal - journal / 4 };
    int files = 0;
    s64 bytes = 0;
    char path[FS_MAX_PATH];

    if (R_FAILED(c.rc = fsFsCleanDirectoryRecursively(&fs, "/")))
        err = "could not clear the save";

    while (!err && !at_end) {
        if (!ustar_sanitize_path(ent.name, path, sizeof(path), &err))
            break;
        u64 pad = ustar_padding(ent.size);
        if (ent.type == USTAR_ENTRY_DIR) {
            if (strcmp(path, "/") != 0) {
                mkparents(&fs, path);
                fsFsCreateDirectory(&fs, path);
            }
        } else if (ent.type == USTAR_ENTRY_SKIP || strcmp(path, "/") == 0) {
            if (!body_exact(req, NULL, ent.size + pad))
                err = "request body ended inside an entry";
        } else {
            err = import_file(req, &c, path, ent.size);
            if (!err && pad && !body_exact(req, NULL, pad))
                err = "request body ended in padding";
            if (!err) {
                files++;
                bytes += (s64)ent.size;
            }
        }
        if (err)
            break;

        // Next header. A body that ends cleanly on a block boundary without
        // the end-of-archive blocks is accepted (Content-Length was met).
        int r = read_block(req, hdr);
        if (r < 0)
            err = "request body truncated";
        else if (r == 0 || ustar_is_zero_block(hdr))
            at_end = true;
        else if (!ustar_parse_header(hdr, &ent, &err))
            break;
    }
    if (!err && R_FAILED(c.rc = fsFsCommit(&fs)))
        err = "commit failed";

    fsFsClose(&fs);
    char uid[33];
    saves_format_uid(info.uid.uid, uid);
    if (err) {
        // Without an intermediate commit, closing discards everything.
        bool unchanged = c.commits == 0;
        LOGF("saves: import %016llx failed: %s rc=0x%x after %d files (%s)\n",
             (unsigned long long)info.save_data_id, err, c.rc, files,
             unchanged ? "save unchanged" : "save partially restored");
        json_escape(err, strlen(err), g_esc, sizeof(g_esc));
        // Don't drain a possibly huge remaining body: close after replying.
        req->keep_alive = false;
        http_set_keep_alive(false);
        http_send_json(req->fd, rc_is_locked(c.rc) ? 409 : (R_FAILED(c.rc) ? 500 : 400),
                       "{\"ok\":false,\"error\":\"%s\",\"rc\":\"0x%x\",\"filesWritten\":%d,"
                       "\"saveUnchanged\":%s}",
                       g_esc, c.rc, files, unchanged ? "true" : "false");
        return;
    }
    LOGF("saves: imported %d files (%lld bytes) into %016llx, %d intermediate commits\n",
         files, (long long)bytes, (unsigned long long)info.save_data_id, c.commits);
    http_send_json(req->fd, 200,
                   "{\"ok\":true,\"saveId\":\"%016llx\",\"titleId\":\"%016llx\",\"uid\":\"%s\","
                   "\"files\":%d,\"bytes\":%lld,\"atomic\":%s}",
                   (unsigned long long)info.save_data_id, (unsigned long long)info.application_id,
                   uid, files, (long long)bytes, c.commits == 0 ? "true" : "false");
}

#else // !__SWITCH__ : host stub so the MCP layer links in tests.

bool saves_list(SaveEntry **out, int *count, char *err, size_t errsz) {
    (void)err; (void)errsz;
    *out = NULL;
    *count = 0;
    return true;
}

#endif // __SWITCH__
