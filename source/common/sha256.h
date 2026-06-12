#pragma once

#include <stddef.h>
#include <stdint.h>

// SHA-256 (for OAuth PKCE S256 verification). On the Switch this delegates to
// libnx's hardware-accelerated sha256CalculateHash(); host builds (tests) use
// a portable implementation.
void sha256_hash(uint8_t out[32], const void *data, size_t len);
