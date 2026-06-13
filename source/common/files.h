#pragma once

#include "http.h"

// Filesystem root prefix; overridable for host-side tests.
#ifndef FILES_ROOT
#define FILES_ROOT "sdmc:"
#endif

// Handlers for the /files endpoint. Each one sends a complete HTTP response.
void files_handle_get(HttpRequest *req);    // file download or directory listing
void files_handle_put(HttpRequest *req);    // upload (streamed write)
void files_handle_delete(HttpRequest *req); // remove file or empty directory
void files_handle_hash(HttpRequest *req);   // SHA-256 of a file (JSON)

// --- Shared helpers (also used by the MCP tools) -----------------------------

// Resolves a user path ("/switch/foo") into FILES_ROOT-prefixed fspath.
// Rejects relative paths and ".." traversal; *err receives a message.
bool files_resolve(const char *userpath, char *out, size_t outsz, const char **err);

// Creates all parent directories of fspath.
void files_mkdirs_for(const char *fspath);

// Builds a JSON directory listing (malloc'd, caller frees). NULL on error,
// with *err set.
char *files_build_listing(const char *fspath, const char *userpath,
                          size_t *out_len, const char **err);

// Deletes a file or empty directory. Returns true on success, *err on failure.
bool files_delete_path(const char *fspath, const char **err);

// Computes the SHA-256 of a regular file, streamed in fixed-size chunks (so
// memory use is constant regardless of file size). On success writes a 64-char
// lowercase hex string (NUL-terminated) into out_hex and the byte count into
// *out_size, and returns true. On failure returns false with *err set.
bool files_hash_sha256(const char *fspath, char out_hex[65], long long *out_size,
                       const char **err);
