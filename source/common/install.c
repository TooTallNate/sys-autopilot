#include "install.h"

#include <string.h>
#include <stdio.h>

// --- PFS0 header parsing (pure) ----------------------------------------------

// On-disk PFS0 layout: Header(0x10) | FileEntry[count](0x18 each) |
// string table(string_table_size) | file data.
typedef struct {
    uint32_t magic;
    uint32_t file_count;
    uint32_t string_table_size;
    uint32_t reserved;
} Pfs0Header;

typedef struct {
    uint64_t data_offset;
    uint64_t data_size;
    uint32_t name_offset;
    uint32_t pad;
} Pfs0FileEntry;

static uint32_t rd_u32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint64_t rd_u64(const uint8_t *p) {
    return (uint64_t)rd_u32(p) | ((uint64_t)rd_u32(p + 4) << 32);
}

size_t pfs0_header_size(const uint8_t *buf16) {
    if (rd_u32(buf16) != PFS0_MAGIC)
        return 0;
    uint32_t count = rd_u32(buf16 + 4);
    uint32_t strtab = rd_u32(buf16 + 8);
    // Guard against absurd values that would overflow our buffers.
    if (count > 4096 || strtab > 0x10000)
        return 0;
    return 0x10 + (size_t)count * 0x18 + strtab;
}

bool pfs0_parse_header(const uint8_t *buf, size_t buf_len,
                       Pfs0Entry *entries, int max_entries, int *out_count,
                       uint64_t *out_data_start, const char **err) {
    if (buf_len < 0x10) { *err = "short PFS0 header"; return false; }
    if (rd_u32(buf) != PFS0_MAGIC) { *err = "bad PFS0 magic"; return false; }

    uint32_t count = rd_u32(buf + 4);
    uint32_t strtab_size = rd_u32(buf + 8);
    if (count == 0) { *err = "empty PFS0"; return false; }
    if (count > 4096 || strtab_size > 0x10000) { *err = "PFS0 too large"; return false; }
    if ((int)count > max_entries) { *err = "too many files in PFS0"; return false; }

    size_t table_off = 0x10;
    size_t strtab_off = table_off + (size_t)count * sizeof(Pfs0FileEntry);
    size_t data_start = strtab_off + strtab_size;
    if (buf_len < data_start) { *err = "incomplete PFS0 header"; return false; }

    for (uint32_t i = 0; i < count; i++) {
        const uint8_t *e = buf + table_off + (size_t)i * sizeof(Pfs0FileEntry);
        uint64_t doff = rd_u64(e);
        uint64_t dsize = rd_u64(e + 8);
        uint32_t noff = rd_u32(e + 16);
        if (noff >= strtab_size) { *err = "bad name offset"; return false; }
        const char *name = (const char *)(buf + strtab_off + noff);
        // Ensure NUL-terminated within the string table.
        size_t maxlen = strtab_size - noff;
        size_t nlen = strnlen(name, maxlen);
        if (nlen >= maxlen) { *err = "unterminated filename"; return false; }
        if (nlen >= sizeof(entries[i].name)) { *err = "filename too long"; return false; }
        memcpy(entries[i].name, name, nlen + 1);
        entries[i].offset = doff;
        entries[i].size = dsize;
    }

    *out_count = (int)count;
    *out_data_start = data_start;
    return true;
}

#ifdef __SWITCH__
#include <switch.h>
#include <stdarg.h>
#include <stdlib.h>
#include <sys/stat.h>
#include "log.h"
#include "nx_ext.h"
#include "sha256.h"

// --- helpers -----------------------------------------------------------------

#define INSTALL_CHUNK 0x100000            // 1 MiB streaming chunk
#define MAX_PFS0_FILES 64                 // NSPs have a handful of entries

// The on-disk packaged content-meta header (start of the .cnmt file).
typedef struct {
    u64 id;
    u32 version;
    u8  type;            // NcmContentMetaType
    u8  reserved;
    u16 extended_header_size;
    u16 content_count;
    u16 content_meta_count;
    u8  attributes;
    u8  storage_id;
    u32 reserved2;
} PackagedContentMetaHeader;

static u8 g_chunk[INSTALL_CHUNK];

static bool g_ncm_ok, g_ns_ok, g_es_ok;

bool install_init(void) {
    g_ncm_ok = R_SUCCEEDED(ncmInitialize());
    g_ns_ok  = R_SUCCEEDED(nsext_init());
    g_es_ok  = R_SUCCEEDED(esext_init());
    if (!g_ncm_ok) LOGF("install: ncm init failed\n");
    if (!g_ns_ok)  LOGF("install: ns init failed\n");
    if (!g_es_ok)  LOGF("install: es init failed\n");
    return g_ncm_ok && g_ns_ok;
}

void install_exit(void) {
    if (g_es_ok)  { esext_exit(); g_es_ok = false; }
    if (g_ns_ok)  { nsext_exit(); g_ns_ok = false; }
    if (g_ncm_ok) { ncmExit();    g_ncm_ok = false; }
}

// Convert an NCA filename (32 lowercase hex chars, optionally "<id>.nca" or
// "<id>.cnmt.nca") into an NcmContentId. Returns false if not 32 hex chars.
static bool content_id_from_name(const char *name, NcmContentId *out) {
    char hex[33];
    size_t n = 0;
    for (const char *p = name; *p && *p != '.' && n < 32; p++, n++)
        hex[n] = *p;
    if (n != 32)
        return false;
    hex[32] = '\0';
    for (int i = 0; i < 16; i++) {
        char b[3] = { hex[i * 2], hex[i * 2 + 1], 0 };
        char *end = NULL;
        long v = strtol(b, &end, 16);
        if (end != b + 2)
            return false;
        out->c[i] = (u8)v;
    }
    return true;
}

static void fail(InstallResult *r, int status, const char *fmt, ...) {
    r->ok = false;
    r->http_status = status;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(r->message, sizeof(r->message), fmt, ap);
    va_end(ap);
    LOGF("install: ERROR %s\n", r->message);
}

// A content we wrote to NCM (for rollback/registration).
typedef struct {
    NcmContentId id;
    bool registered;
} WrittenContent;

// Streams `size` bytes from the source into an NCM placeholder, registering it.
// Verifies SHA-256 against the content id (the NCA filename). `prebuf`/`prelen`
// are bytes already read from the stream that belong to this content.
static Result write_content(NcmContentStorage *cs, const NcmContentId *cid,
                            u64 size, InstallReadFn read_fn, void *ctx,
                            const u8 *prebuf, size_t prelen, bool verify) {
    NcmPlaceHolderId phid;
    Result rc = ncmContentStorageGeneratePlaceHolderId(cs, &phid);
    if (R_FAILED(rc)) return rc;
    ncmContentStorageDeletePlaceHolder(cs, &phid); // ignore
    rc = ncmContentStorageCreatePlaceHolder(cs, cid, &phid, (s64)size);
    if (R_FAILED(rc)) { LOGF("install: CreatePlaceHolder rc=0x%x\n", rc); return rc; }

    Sha256Stream sha;
    sha256_stream_init(&sha);

    u64 written = 0;
    // Consume any pre-read bytes first.
    while (prelen > 0) {
        size_t n = prelen > INSTALL_CHUNK ? INSTALL_CHUNK : prelen;
        rc = ncmContentStorageWritePlaceHolder(cs, &phid, written, prebuf, n);
        if (R_FAILED(rc)) goto done;
        if (verify) sha256_stream_update(&sha, prebuf, n);
        written += n; prebuf += n; prelen -= n;
    }
    while (written < size) {
        u64 want = size - written;
        size_t chunk = want > INSTALL_CHUNK ? INSTALL_CHUNK : (size_t)want;
        long got = read_fn(ctx, g_chunk, chunk);
        if (got <= 0) { rc = MAKERESULT(Module_Libnx, LibnxError_IoError); goto done; }
        rc = ncmContentStorageWritePlaceHolder(cs, &phid, written, g_chunk, (size_t)got);
        if (R_FAILED(rc)) goto done;
        if (verify) sha256_stream_update(&sha, g_chunk, (size_t)got);
        written += (u64)got;
    }

    if (verify) {
        u8 digest[32];
        sha256_stream_final(&sha, digest);
        if (memcmp(digest, cid->c, 16) != 0) {
            LOGF("install: hash mismatch for content\n");
            rc = MAKERESULT(Module_Libnx, LibnxError_BadInput);
            goto done;
        }
    }

    ncmContentStorageDelete(cs, cid); // replace any stale copy
    rc = ncmContentStorageRegister(cs, cid, &phid);
    if (R_FAILED(rc)) LOGF("install: Register rc=0x%x\n", rc);

done:
    ncmContentStorageDeletePlaceHolder(cs, &phid);
    return rc;
}

// Parses the meta NCA's CNMT into the packaged content-meta + content infos.
// The meta NCA has already been registered in NCM, so we ask NCM for its
// on-disk FS path and mount that as a ContentMeta filesystem (fs decrypts it
// for us — no keys needed), read the single .cnmt, and parse it.
static bool read_cnmt(NcmContentStorage *cs, const NcmContentId *meta_cid,
                      PackagedContentMetaHeader *out_hdr,
                      NcmContentInfo *out_infos, int max_infos, int *out_n,
                      u8 *out_ext_hdr, size_t ext_hdr_cap, u16 *out_ext_hdr_size,
                      const char **err) {
    char nca_path[FS_MAX_PATH] = {0};
    Result rc = ncmContentStorageGetPath(cs, nca_path, sizeof(nca_path), meta_cid);
    if (R_FAILED(rc)) {
        *err = "get meta nca path failed";
        LOGF("install: ncmContentStorageGetPath rc=0x%x\n", rc);
        return false;
    }

    FsFileSystem cfs;
    rc = fsOpenFileSystemWithId(&cfs, 0, FsFileSystemType_ContentMeta,
                                nca_path, FsContentAttributes_All);
    if (R_FAILED(rc)) {
        *err = "mount cnmt nca failed";
        LOGF("install: fsOpenFileSystemWithId(cnmt) rc=0x%x path=%s\n", rc, nca_path);
        return false;
    }

    bool ok = false;
    // Find the single *.cnmt file in the mounted FS.
    char cnmt_path[FS_MAX_PATH] = {0};
    FsDir dir;
    if (R_SUCCEEDED(fsFsOpenDirectory(&cfs, "/", FsDirOpenMode_ReadFiles, &dir))) {
        FsDirectoryEntry de;
        s64 n;
        while (R_SUCCEEDED(fsDirRead(&dir, &n, 1, &de)) && n == 1) {
            const char *dot = strrchr(de.name, '.');
            if (dot && strcasecmp(dot, ".cnmt") == 0) {
                cnmt_path[0] = '/';
                size_t nm = strnlen(de.name, sizeof(cnmt_path) - 2);
                memcpy(cnmt_path + 1, de.name, nm);
                cnmt_path[1 + nm] = '\0';
                break;
            }
        }
        fsDirClose(&dir);
    }

    if (cnmt_path[0] == '\0') { *err = "no .cnmt in meta nca"; goto out; }

    FsFile cf;
    if (R_FAILED(fsFsOpenFile(&cfs, cnmt_path, FsOpenMode_Read, &cf))) {
        *err = "open .cnmt failed"; goto out;
    }
    s64 csize = 0;
    fsFileGetSize(&cf, &csize);
    static u8 cnmt_buf[0x4000];
    if (csize <= 0 || csize > (s64)sizeof(cnmt_buf)) {
        fsFileClose(&cf); *err = ".cnmt size invalid"; goto out;
    }
    u64 br = 0;
    rc = fsFileRead(&cf, 0, cnmt_buf, csize, FsReadOption_None, &br);
    fsFileClose(&cf);
    if (R_FAILED(rc) || br != (u64)csize) { *err = "read .cnmt failed"; goto out; }

    if ((size_t)csize < sizeof(PackagedContentMetaHeader)) { *err = "cnmt too small"; goto out; }
    memcpy(out_hdr, cnmt_buf, sizeof(*out_hdr));

    // Capture the type-specific extended header that follows the header.
    if (out_hdr->extended_header_size > ext_hdr_cap ||
        sizeof(PackagedContentMetaHeader) + out_hdr->extended_header_size > (size_t)csize) {
        *err = "bad extended header size"; goto out;
    }
    memcpy(out_ext_hdr, cnmt_buf + sizeof(PackagedContentMetaHeader),
           out_hdr->extended_header_size);
    *out_ext_hdr_size = out_hdr->extended_header_size;

    int infos = 0;
    for (u32 i = 0; i < out_hdr->content_count && infos < max_infos; i++) {
        size_t off = sizeof(PackagedContentMetaHeader) + out_hdr->extended_header_size
                   + (size_t)i * sizeof(NcmPackagedContentInfo);
        if (off + sizeof(NcmPackagedContentInfo) > (size_t)csize) break;
        const NcmPackagedContentInfo *pci =
            (const NcmPackagedContentInfo *)(cnmt_buf + off);
        if (pci->info.content_type == NcmContentType_DeltaFragment)
            continue;
        out_infos[infos++] = pci->info;
    }
    *out_n = infos;
    ok = true;

out:
    fsFsClose(&cfs);
    return ok;
}

bool install_nsp_stream(InstallReadFn read_fn, void *ctx, uint64_t total_size,
                        InstallStorage storage, InstallResult *out) {
    memset(out, 0, sizeof(*out));
    out->http_status = 200;

    if (!g_ncm_ok || !g_ns_ok) {
        fail(out, 500, "install services unavailable");
        return false;
    }

    NcmStorageId sid = (storage == INSTALL_STORAGE_NAND)
                     ? NcmStorageId_BuiltInUser : NcmStorageId_SdCard;

    // 1. Read the fixed 16-byte PFS0 header, then the full header region.
    static u8 hdrbuf[0x10 + MAX_PFS0_FILES * 0x18 + 0x4000];
    uint64_t consumed = 0;
    long n = read_fn(ctx, hdrbuf, 0x10);
    if (n != 0x10) { fail(out, 400, "stream too short"); return false; }
    consumed += 0x10;
    size_t hsize = pfs0_header_size(hdrbuf);
    if (hsize == 0 || hsize > sizeof(hdrbuf)) { fail(out, 400, "not a valid NSP (PFS0)"); return false; }
    // Read the rest of the header.
    size_t hgot = 0x10;
    while (hgot < hsize) {
        n = read_fn(ctx, hdrbuf + hgot, hsize - hgot);
        if (n <= 0) { fail(out, 400, "truncated PFS0 header"); return false; }
        hgot += (size_t)n;
    }
    consumed += hsize - 0x10;

    static Pfs0Entry entries[MAX_PFS0_FILES];
    int file_count = 0;
    uint64_t data_start = 0;
    const char *perr = NULL;
    if (!pfs0_parse_header(hdrbuf, hsize, entries, MAX_PFS0_FILES,
                           &file_count, &data_start, &perr)) {
        fail(out, 400, "PFS0 parse: %s", perr);
        return false;
    }

    NcmContentStorage cs;
    if (R_FAILED(ncmOpenContentStorage(&cs, sid))) {
        fail(out, 500, "cannot open content storage");
        return false;
    }

    // Buffers for the small metadata files (cnmt.nca / .tik / .cert).
    static u8 cnmt_nca[0x4000];
    size_t cnmt_nca_size = 0;
    NcmContentId meta_cid = {0};
    bool have_meta_cid = false;
    static u8 tik_buf[0x600]; size_t tik_size = 0;
    static u8 cert_buf[0x800]; size_t cert_size = 0;

    static WrittenContent written[MAX_PFS0_FILES];
    int written_n = 0;
    bool failed = false;

    // 2. Stream each entry in file order. data_start is relative to PFS0 start;
    //    `consumed` tracks how many bytes of the stream we've read.
    for (int i = 0; i < file_count && !failed; i++) {
        uint64_t file_abs = data_start + entries[i].offset;
        // Skip any gap before this file (forward-only).
        while (consumed < file_abs) {
            uint64_t gap = file_abs - consumed;
            size_t want = gap > INSTALL_CHUNK ? INSTALL_CHUNK : (size_t)gap;
            n = read_fn(ctx, g_chunk, want);
            if (n <= 0) { fail(out, 400, "unexpected end of stream"); failed = true; break; }
            consumed += (uint64_t)n;
        }
        if (failed) break;

        const char *name = entries[i].name;
        const char *dot = strrchr(name, '.');
        bool is_tik  = dot && strcasecmp(dot, ".tik") == 0;
        bool is_cert = dot && strcasecmp(dot, ".cert") == 0;
        size_t namelen = strlen(name);
        bool is_cnmt = namelen > 9 && strcasecmp(name + namelen - 9, ".cnmt.nca") == 0;
        bool is_nca  = dot && strcasecmp(dot, ".nca") == 0;

        if (is_cnmt) {
            // Buffer the meta NCA in RAM, then write it to NCM too.
            if (entries[i].size > sizeof(cnmt_nca)) { fail(out, 400, "cnmt nca too large"); failed = true; break; }
            size_t got = 0;
            while (got < entries[i].size) {
                n = read_fn(ctx, cnmt_nca + got, entries[i].size - got);
                if (n <= 0) { fail(out, 400, "truncated cnmt"); failed = true; break; }
                got += (size_t)n;
            }
            if (failed) break;
            cnmt_nca_size = entries[i].size;
            consumed += cnmt_nca_size;
            if (!content_id_from_name(name, &meta_cid)) {
                fail(out, 400, "bad cnmt nca filename: %s", name); failed = true; break;
            }
            have_meta_cid = true;
            Result rc = write_content(&cs, &meta_cid, cnmt_nca_size, read_fn, ctx,
                                      cnmt_nca, cnmt_nca_size, true);
            if (R_FAILED(rc)) { fail(out, 500, "write meta nca failed (0x%x)", rc); failed = true; break; }
            written[written_n].id = meta_cid; written[written_n].registered = true; written_n++;
        } else if (is_tik) {
            if (entries[i].size > sizeof(tik_buf)) { fail(out, 400, "ticket too large"); failed = true; break; }
            size_t got = 0;
            while (got < entries[i].size) {
                n = read_fn(ctx, tik_buf + got, entries[i].size - got);
                if (n <= 0) { fail(out, 400, "truncated ticket"); failed = true; break; }
                got += (size_t)n;
            }
            tik_size = entries[i].size; consumed += tik_size;
        } else if (is_cert) {
            if (entries[i].size > sizeof(cert_buf)) { fail(out, 400, "cert too large"); failed = true; break; }
            size_t got = 0;
            while (got < entries[i].size) {
                n = read_fn(ctx, cert_buf + got, entries[i].size - got);
                if (n <= 0) { fail(out, 400, "truncated cert"); failed = true; break; }
                got += (size_t)n;
            }
            cert_size = entries[i].size; consumed += cert_size;
        } else if (is_nca) {
            NcmContentId cid;
            if (!content_id_from_name(name, &cid)) {
                fail(out, 400, "bad NCA filename: %s", name); failed = true; break;
            }
            Result rc = write_content(&cs, &cid, entries[i].size, read_fn, ctx,
                                      NULL, 0, true);
            if (R_FAILED(rc)) { fail(out, 500, "write nca failed (0x%x)", rc); failed = true; break; }
            consumed += entries[i].size;
            written[written_n].id = cid; written[written_n].registered = true; written_n++;
        } else {
            // Unknown entry: skip its bytes.
            uint64_t skip = entries[i].size;
            while (skip > 0) {
                size_t want = skip > INSTALL_CHUNK ? INSTALL_CHUNK : (size_t)skip;
                n = read_fn(ctx, g_chunk, want);
                if (n <= 0) { fail(out, 400, "truncated entry"); failed = true; break; }
                skip -= (uint64_t)n; consumed += (uint64_t)n;
            }
        }
    }

    // 3. Parse the CNMT (from the now-registered meta NCA via its NCM path),
    //    then register the content-meta DB entry + ticket + record.
    PackagedContentMetaHeader pkg = {0};
    NcmContentInfo infos[MAX_PFS0_FILES];
    int infos_n = 0;
    static u8 ext_hdr[0x80];
    u16 ext_hdr_size = 0;

    if (!failed && (cnmt_nca_size == 0 || !have_meta_cid)) {
        fail(out, 400, "no cnmt.nca in NSP"); failed = true;
    }
    if (!failed) {
        const char *cerr = NULL;
        if (!read_cnmt(&cs, &meta_cid, &pkg, infos, MAX_PFS0_FILES,
                       &infos_n, ext_hdr, sizeof(ext_hdr), &ext_hdr_size, &cerr)) {
            fail(out, 400, "cnmt: %s", cerr); failed = true;
        }
    }

    ncmContentStorageClose(&cs);

    if (!failed) {
        // Import the ticket (best-effort permanence; es imports it as-is). A
        // missing ticket is fine for unprotected content.
        if (g_es_ok && tik_size > 0) {
            Result rc = esext_import_ticket(tik_buf, tik_size,
                                            cert_size ? cert_buf : NULL, cert_size);
            if (R_FAILED(rc)) LOGF("install: import ticket rc=0x%x (continuing)\n", rc);
        }

        // Build the install-time content-meta blob:
        //   NcmContentMetaHeader | extended header | NcmContentInfo[meta + contents]
        static u8 meta_blob[sizeof(NcmContentMetaHeader) + sizeof(ext_hdr)
                            + (MAX_PFS0_FILES + 1) * sizeof(NcmContentInfo)];
        size_t pos = 0;

        NcmContentMetaHeader mh = {0};
        mh.extended_header_size = ext_hdr_size;
        mh.content_count = (u16)(infos_n + 1); // +1 = the meta NCA itself
        mh.content_meta_count = pkg.content_meta_count;
        mh.attributes = pkg.attributes;
        mh.storage_id = 0;
        memcpy(meta_blob + pos, &mh, sizeof(mh)); pos += sizeof(mh);
        memcpy(meta_blob + pos, ext_hdr, ext_hdr_size); pos += ext_hdr_size;

        // Meta NCA's own NcmContentInfo first.
        NcmContentInfo meta_info = {0};
        meta_info.content_id = meta_cid;
        meta_info.content_type = NcmContentType_Meta;
        ncmU64ToContentInfoSize(cnmt_nca_size, &meta_info);
        memcpy(meta_blob + pos, &meta_info, sizeof(meta_info)); pos += sizeof(meta_info);
        // Then every packaged content's info.
        for (int i = 0; i < infos_n; i++) {
            memcpy(meta_blob + pos, &infos[i], sizeof(NcmContentInfo));
            pos += sizeof(NcmContentInfo);
        }

        NcmContentMetaKey meta_key = {0};
        meta_key.id = pkg.id;
        meta_key.version = pkg.version;
        meta_key.type = pkg.type;
        meta_key.install_type = NcmContentInstallType_Full;

        NcmContentMetaDatabase db;
        Result rc = ncmOpenContentMetaDatabase(&db, sid);
        if (R_SUCCEEDED(rc)) {
            rc = ncmContentMetaDatabaseSet(&db, &meta_key, meta_blob, pos);
            if (R_SUCCEEDED(rc)) rc = ncmContentMetaDatabaseCommit(&db);
            ncmContentMetaDatabaseClose(&db);
        }
        if (R_FAILED(rc)) { fail(out, 500, "content-meta set failed (0x%x)", rc); failed = true; }

        // Push the application record so the title appears on HOME.
        if (!failed) {
            // base id: patch ^0x800, AOC (^0x1000)&~0xfff, else app unchanged.
            u64 base_id = pkg.id;
            if (pkg.type == NcmContentMetaType_Patch) base_id = pkg.id ^ 0x800;
            else if (pkg.type == NcmContentMetaType_AddOnContent) base_id = (pkg.id ^ 0x1000) & ~(u64)0xFFF;

            // Merge with existing records for this base id.
            static NsExtContentStorageRecord recs[64];
            s32 existing = 0;
            nsext_list_application_record_content_meta(0, base_id, recs, 63, &existing);
            if (existing < 0 || existing > 63) existing = 0;
            recs[existing].key = meta_key;
            recs[existing].storage_id = (u8)sid;
            memset(recs[existing].padding, 0, sizeof(recs[existing].padding));

            nsext_delete_application_record(base_id); // ignore (may not exist)
            rc = nsext_push_application_record(base_id, recs, (u32)(existing + 1));
            if (R_FAILED(rc)) { fail(out, 500, "push application record failed (0x%x)", rc); failed = true; }
        }
    }

    // Rollback on failure: remove any content we registered.
    if (failed) {
        NcmContentStorage rcs;
        if (R_SUCCEEDED(ncmOpenContentStorage(&rcs, sid))) {
            for (int i = 0; i < written_n; i++)
                ncmContentStorageDelete(&rcs, &written[i].id);
            ncmContentStorageClose(&rcs);
        }
        return false;
    }

    out->ok = true;
    out->title_id = pkg.id;
    out->version = pkg.version;
    snprintf(out->message, sizeof(out->message),
             "installed %016llx v%u", (unsigned long long)pkg.id, pkg.version);
    LOGF("install: %s\n", out->message);
    return true;
}

#endif // __SWITCH__

