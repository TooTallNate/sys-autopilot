#pragma once

#include <stddef.h>
#include <stdint.h>

// SHA-256. On the Switch this delegates to libnx's hardware-accelerated
// implementation; host builds (tests) use a portable implementation.
//
// Two APIs are provided:
//   - sha256_hash(): one-shot, whole buffer in memory (used by OAuth PKCE).
//   - Sha256Stream + sha256_stream_*(): incremental, so large inputs (e.g.
//     files) can be hashed in fixed-size chunks with constant memory.

// Opaque-ish streaming context. The host and Switch builds use different
// internal layouts, but both fit comfortably in this storage.
typedef struct {
    // Large enough for either libnx's Sha256Context or the portable state.
    uint8_t opaque[128];
} Sha256Stream;

void sha256_stream_init(Sha256Stream *st);
void sha256_stream_update(Sha256Stream *st, const void *data, size_t len);
void sha256_stream_final(Sha256Stream *st, uint8_t out[32]);

// One-shot convenience (for OAuth PKCE S256 verification).
void sha256_hash(uint8_t out[32], const void *data, size_t len);
