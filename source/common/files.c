#include "files.h"
#include "log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <dirent.h>
#include <unistd.h>
#include <sys/stat.h>

#define IO_BUF_SIZE 0x8000 // 32 KB

static char g_io_buf[IO_BUF_SIZE];

// Resolves the "path" query param into an sdmc:/ path.
// Rejects missing params, relative paths and ".." traversal.
static bool resolve_path(HttpRequest *req, char *out, size_t outsz) {
    char path[512];
    if (!http_query_get(req, "path", path, sizeof(path)) || path[0] == '\0') {
        http_send_error(req->fd, 400, "missing 'path' query parameter");
        return false;
    }
    if (path[0] != '/') {
        http_send_error(req->fd, 400, "path must be absolute (start with /)");
        return false;
    }
    // Reject any ".." path segment.
    const char *p = path;
    while ((p = strstr(p, "..")) != NULL) {
        bool start_ok = (p == path) || p[-1] == '/';
        bool end_ok = p[2] == '\0' || p[2] == '/';
        if (start_ok && end_ok) {
            http_send_error(req->fd, 400, "path traversal not allowed");
            return false;
        }
        p += 2;
    }
    snprintf(out, outsz, "sdmc:%s", path);
    return true;
}

// Appends to a heap-grown buffer, doubling as needed. Returns false on OOM.
static bool buf_append(char **buf, size_t *len, size_t *cap, const char *data, size_t n) {
    if (*len + n + 1 > *cap) {
        size_t newcap = *cap ? *cap : 1024;
        while (*len + n + 1 > newcap)
            newcap *= 2;
        char *nb = realloc(*buf, newcap);
        if (!nb)
            return false;
        *buf = nb;
        *cap = newcap;
    }
    memcpy(*buf + *len, data, n);
    *len += n;
    (*buf)[*len] = '\0';
    return true;
}

// Minimal JSON string escaping for filenames.
static void json_escape(const char *in, char *out, size_t outsz) {
    size_t o = 0;
    for (const char *p = in; *p && o + 7 < outsz; p++) {
        unsigned char c = (unsigned char)*p;
        if (c == '"' || c == '\\') {
            out[o++] = '\\';
            out[o++] = (char)c;
        } else if (c < 0x20) {
            o += (size_t)snprintf(out + o, outsz - o, "\\u%04x", c);
        } else {
            out[o++] = (char)c;
        }
    }
    out[o] = '\0';
}

static const char *content_type_for(const char *path) {
    const char *ext = strrchr(path, '.');
    if (!ext)
        return "application/octet-stream";
    ext++;
    if (strcasecmp(ext, "txt") == 0 || strcasecmp(ext, "log") == 0 ||
        strcasecmp(ext, "ini") == 0 || strcasecmp(ext, "cfg") == 0 ||
        strcasecmp(ext, "md") == 0)
        return "text/plain";
    if (strcasecmp(ext, "json") == 0)
        return "application/json";
    if (strcasecmp(ext, "html") == 0 || strcasecmp(ext, "htm") == 0)
        return "text/html";
    if (strcasecmp(ext, "jpg") == 0 || strcasecmp(ext, "jpeg") == 0)
        return "image/jpeg";
    if (strcasecmp(ext, "png") == 0)
        return "image/png";
    return "application/octet-stream";
}

static void send_dir_listing(HttpRequest *req, const char *fspath, const char *userpath) {
    DIR *dir = opendir(fspath);
    if (!dir) {
        http_send_error(req->fd, 404, "directory not found");
        return;
    }

    char *json = NULL;
    size_t len = 0, cap = 0;
    bool ok = true;

    char head[600];
    char escaped[520];
    json_escape(userpath, escaped, sizeof(escaped));
    int n = snprintf(head, sizeof(head), "{\"path\":\"%s\",\"entries\":[", escaped);
    ok = buf_append(&json, &len, &cap, head, (size_t)n);

    struct dirent *ent;
    bool first = true;
    while (ok && (ent = readdir(dir)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
            continue;

        char full[1024];
        snprintf(full, sizeof(full), "%s%s%s", fspath,
                 fspath[strlen(fspath) - 1] == '/' ? "" : "/", ent->d_name);

        struct stat st;
        bool have_st = stat(full, &st) == 0;
        bool is_dir = have_st && S_ISDIR(st.st_mode);

        json_escape(ent->d_name, escaped, sizeof(escaped));

        char entry[640];
        if (is_dir) {
            n = snprintf(entry, sizeof(entry), "%s{\"name\":\"%s\",\"type\":\"dir\"}",
                         first ? "" : ",", escaped);
        } else {
            n = snprintf(entry, sizeof(entry),
                         "%s{\"name\":\"%s\",\"type\":\"file\",\"size\":%lld,\"mtime\":%lld}",
                         first ? "" : ",", escaped,
                         have_st ? (long long)st.st_size : 0LL,
                         have_st ? (long long)st.st_mtime : 0LL);
        }
        ok = buf_append(&json, &len, &cap, entry, (size_t)n);
        first = false;
    }
    closedir(dir);

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

static void send_file(HttpRequest *req, const char *fspath, const struct stat *st) {
    long long offset = 0;
    long long length = -1;
    char val[32];
    if (http_query_get(req, "offset", val, sizeof(val)))
        offset = atoll(val);
    if (http_query_get(req, "length", val, sizeof(val)))
        length = atoll(val);

    long long fsize = (long long)st->st_size;
    if (offset < 0) {
        // Negative offset: read the last N bytes (tail).
        offset = fsize + offset;
        if (offset < 0)
            offset = 0;
    }
    if (offset > fsize)
        offset = fsize;
    long long avail = fsize - offset;
    if (length < 0 || length > avail)
        length = avail;

    FILE *f = fopen(fspath, "rb");
    if (!f) {
        http_send_error(req->fd, 404, "file not found");
        return;
    }
    if (offset > 0 && fseek(f, (long)offset, SEEK_SET) != 0) {
        fclose(f);
        http_send_error(req->fd, 500, "seek failed");
        return;
    }

    http_send_header(req->fd, 200, content_type_for(fspath), (size_t)length);

    long long remaining = length;
    while (remaining > 0) {
        size_t chunk = remaining > IO_BUF_SIZE ? IO_BUF_SIZE : (size_t)remaining;
        size_t n = fread(g_io_buf, 1, chunk, f);
        if (n == 0)
            break;
        if (!http_write_all(req->fd, g_io_buf, n))
            break;
        remaining -= (long long)n;
    }
    fclose(f);
}

void files_handle_get(HttpRequest *req) {
    char fspath[768];
    if (!resolve_path(req, fspath, sizeof(fspath)))
        return;

    // Trailing slash forces a directory interpretation.
    size_t plen = strlen(fspath);
    bool want_dir = plen > 6 && fspath[plen - 1] == '/';
    if (want_dir && plen > 7)
        fspath[plen - 1] = '\0'; // stat without trailing slash

    struct stat st;
    if (stat(fspath, &st) != 0) {
        http_send_error(req->fd, 404, "no such file or directory");
        return;
    }

    if (S_ISDIR(st.st_mode)) {
        send_dir_listing(req, fspath, fspath + 5 /* skip "sdmc:" */);
    } else if (want_dir) {
        http_send_error(req->fd, 400, "not a directory");
    } else {
        send_file(req, fspath, &st);
    }
}

// Creates all parent directories of fspath ("sdmc:/a/b/c.txt" -> mkdir a, a/b).
static void mkdirs_for(const char *fspath) {
    char tmp[768];
    snprintf(tmp, sizeof(tmp), "%s", fspath);
    // Skip "sdmc:/"
    char *p = strchr(tmp, '/');
    if (!p)
        return;
    p++;
    for (; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            mkdir(tmp, 0777);
            *p = '/';
        }
    }
}

void files_handle_put(HttpRequest *req) {
    char fspath[768];
    if (!resolve_path(req, fspath, sizeof(fspath)))
        return;

    if (!req->has_content_length) {
        http_send_error(req->fd, 411, "Content-Length required");
        return;
    }

    mkdirs_for(fspath);

    FILE *f = fopen(fspath, "wb");
    if (!f) {
        http_send_error(req->fd, 500, "failed to open file for writing");
        return;
    }

    size_t total = 0;
    bool write_err = false;
    while (total < req->content_length) {
        ssize_t n = http_read_body(req, g_io_buf, IO_BUF_SIZE);
        if (n <= 0)
            break;
        if (fwrite(g_io_buf, 1, (size_t)n, f) != (size_t)n) {
            write_err = true;
            break;
        }
        total += (size_t)n;
    }
    fclose(f);

    if (write_err) {
        remove(fspath);
        http_send_error(req->fd, 507, "write failed (sd card full?)");
        return;
    }
    if (total != req->content_length) {
        remove(fspath);
        http_send_error(req->fd, 400, "incomplete upload");
        return;
    }

    LOGF("files: wrote %zu bytes to %s\n", total, fspath);
    http_send_json(req->fd, 201, "{\"written\":%zu,\"path\":\"%s\"}", total, fspath + 5);
}

void files_handle_delete(HttpRequest *req) {
    char fspath[768];
    if (!resolve_path(req, fspath, sizeof(fspath)))
        return;

    struct stat st;
    if (stat(fspath, &st) != 0) {
        http_send_error(req->fd, 404, "no such file or directory");
        return;
    }

    int rc;
    if (S_ISDIR(st.st_mode))
        rc = rmdir(fspath);
    else
        rc = remove(fspath);

    if (rc != 0) {
        http_send_error(req->fd, 500,
                        S_ISDIR(st.st_mode) ? "rmdir failed (directory not empty?)"
                                            : "delete failed");
        return;
    }
    http_send_json(req->fd, 200, "{\"deleted\":\"%s\"}", fspath + 5);
}
