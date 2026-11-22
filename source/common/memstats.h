#pragma once
#include <switch.h>
#include <stdbool.h>
#include <stddef.h>

// Byte the inner heap is filled with at startup, so the highest byte that
// was ever handed out can be found later (heap high-water mark).
#define MEMSTATS_HEAP_FILL 0xA5

typedef struct {
    bool sys_ok;          // system pool numbers below are valid
    u64 sys_total;        // System pool size (bytes)
    u64 sys_used;         // System pool in use (bytes)
    size_t heap_size;     // inner heap size (bytes)
    size_t heap_in_use;   // bytes currently allocated by malloc
    size_t heap_peak;     // highest heap offset ever touched (bytes)
} MemStats;

void memstats_get(MemStats *out);
