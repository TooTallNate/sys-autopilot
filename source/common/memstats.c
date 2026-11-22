#include "memstats.h"

#include <malloc.h>

extern void* fake_heap_start;
extern void* fake_heap_end;

void memstats_get(MemStats *out) {
    // Physical memory of the System pool, shared by all sysmodules. When it
    // runs out, system services die with 2001-0132 (resource limit exceeded).
    out->sys_ok =
        R_SUCCEEDED(svcGetSystemInfo(&out->sys_total, 0 /* total */, INVALID_HANDLE, 2 /* System */)) &&
        R_SUCCEEDED(svcGetSystemInfo(&out->sys_used, 1 /* used */, INVALID_HANDLE, 2 /* System */));

    const u8 *start = (const u8 *)fake_heap_start;
    out->heap_size = (size_t)((const u8 *)fake_heap_end - start);
    out->heap_in_use = (size_t)mallinfo().uordblks;

    // Scan down from the top for the first byte that isn't the fill pattern.
    size_t hw = out->heap_size;
    while (hw > 0 && start[hw - 1] == MEMSTATS_HEAP_FILL)
        hw--;
    out->heap_peak = hw;
}
