#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

// --- Encoding ----------------------------------------------------------------

// Exact encoded length (no NUL) for n input bytes.
static inline size_t b64_encoded_len(size_t n) {
    return ((n + 2) / 3) * 4;
}

// Encodes n bytes into out (must hold b64_encoded_len(n)). Returns chars
// written. Feed full-stream chunks whose size is a multiple of 3 to avoid
// padding except on the final chunk.
size_t b64_encode(const uint8_t *in, size_t n, char *out);

// --- Incremental decoding ----------------------------------------------------

typedef struct {
    uint32_t acc;
    int bits;
    bool err;
} B64Decoder;

void b64dec_init(B64Decoder *d);

// Decodes a chunk of base64 text. Whitespace is skipped; '=' padding ends the
// stream. out must hold at least (inlen / 4 + 1) * 3 bytes. Returns bytes
// written, or -1 on invalid input (also latches d->err).
ssize_t b64dec_update(B64Decoder *d, const char *in, size_t inlen, uint8_t *out);

// True if the stream ended on a valid boundary.
bool b64dec_finish(const B64Decoder *d);

// One-shot decode helper (used for auth headers). Returns bytes written or 0
// on error; output is NUL-terminated.
size_t b64_decode(const char *in, char *out, size_t outsz);
