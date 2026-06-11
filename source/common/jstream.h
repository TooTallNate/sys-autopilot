#pragma once

#include <stdbool.h>
#include <stddef.h>

// Streaming JSON pre-pass for the MCP endpoint.
//
// Feeds raw JSON-RPC body bytes through a small scanner. Every byte is copied
// verbatim into a bounded "reduced document" buffer EXCEPT the string value
// at path $.params.arguments.content: that value's raw bytes are diverted to
// a sink callback (the caller base64-decodes them to disk) and replaced in
// the reduced document by a short placeholder string. This keeps arbitrarily
// large `upload_file` payloads out of RAM while letting the reduced document
// be parsed normally with jsmn.

#define JSTREAM_MAX_DEPTH 15
#define JSTREAM_MAX_KEY   39

// Errors returned by jstream_feed / jstream_finish.
#define JSTREAM_EDOC     -1  // reduced document exceeds the buffer cap
#define JSTREAM_EDEPTH   -2  // nesting too deep / unbalanced
#define JSTREAM_ECONTENT -3  // escape sequence inside content (invalid base64)
#define JSTREAM_ESINK    -4  // sink callback reported failure
#define JSTREAM_EDUP     -5  // multiple content fields
#define JSTREAM_EPARTIAL -6  // input ended mid-string / mid-container

// Receives raw (still base64-encoded) content bytes. Return 0 on success.
typedef int (*JstreamSink)(const char *data, size_t len, void *ctx);

typedef struct {
    char  *doc;
    size_t doc_cap;
    size_t doc_len;
    JstreamSink sink;
    void  *sink_ctx;

    int  depth;
    bool on_path[JSTREAM_MAX_DEPTH + 1];
    bool is_obj[JSTREAM_MAX_DEPTH + 1];
    bool in_string;
    bool is_key;
    bool esc;
    bool next_is_key;
    bool next_value_on_path;
    bool next_value_is_content;
    bool divert;
    bool content_found;
    char key[JSTREAM_MAX_KEY + 1];
    size_t key_len;
    bool key_overflow;
    int  err;
} Jstream;

void jstream_init(Jstream *js, char *doc_buf, size_t doc_cap,
                  JstreamSink sink, void *sink_ctx);

// Processes a chunk. Returns 0 or a JSTREAM_E* error (latched).
int jstream_feed(Jstream *js, const char *data, size_t len);

// Call after the full body. Returns 0 or a JSTREAM_E* error.
// *content_found reports whether a content field was diverted.
int jstream_finish(Jstream *js, bool *content_found);
