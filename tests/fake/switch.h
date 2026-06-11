// Minimal libnx shim so the pure-logic modules (and mcp.c with stubs) can be
// compiled and unit-tested on the host. Only the symbols referenced from the
// shared sources are provided.
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <unistd.h>

typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int8_t   s8;
typedef int16_t  s16;
typedef int32_t  s32;
typedef int64_t  s64;
typedef u32 Result;

#define R_FAILED(r)    ((r) != 0)
#define R_SUCCEEDED(r) ((r) == 0)
#define MAKERESULT(m, d) (((m) & 0x1FF) | (((d) & 0x1FFF) << 9))

enum { Module_Libnx = 345 };
enum { LibnxError_NotInitialized = 2 };

typedef enum {
    ViLayerStack_Default    = 0,
    ViLayerStack_Lcd        = 1,
    ViLayerStack_Screenshot = 2,
    ViLayerStack_Recording  = 3,
    ViLayerStack_LastFrame  = 4,
} ViLayerStack;

static inline void svcSleepThread(s64 ns) {
    usleep((unsigned)(ns / 1000));
}

#define HOSVER_MAJOR(v) (((v) >> 16) & 0xFF)
#define HOSVER_MINOR(v) (((v) >> 8) & 0xFF)
#define HOSVER_MICRO(v) ((v) & 0xFF)

static inline u32 hosversionGet(void) {
    return (19u << 16) | (0u << 8) | 1u;
}
