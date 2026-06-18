#pragma once

// Device facts advertised in the DNS-SD TXT record (and usable elsewhere).
// All strings are pre-formatted and stay valid for the process lifetime.
typedef struct {
    const char *model;       // "v1", "v2", "lite", "oled", "unknown"
    const char *firmware;    // OS version, e.g. "19.0.1"
    const char *atmosphere;  // Atmosphère/Exosphère version, e.g. "1.7.1" or "" if N/A
    const char *serial;      // Console serial, e.g. "XAW10012345678" or "" if N/A
} DeviceInfo;

// Queries the system services (set:sys, spl) and caches the results. MUST be
// called from __appInit while the 'sm' session is still open, because the
// underlying smGetService calls fail once smExit() has run. Safe to call more
// than once (subsequent calls are no-ops). No-op on host builds.
void device_info_init(void);

// Returns the cached device facts gathered by device_info_init(). Never NULL;
// fields are empty/"unknown" if init didn't run or a query failed.
const DeviceInfo *device_info_get(void);
