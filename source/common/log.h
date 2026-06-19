#pragma once

#include <stdbool.h>

// Lightweight logging with two sinks:
//
//   ENABLE_LOG    -> printf to stdout (dev .nro app: visible on the console).
//   LOG_TO_FILE   -> append to a log file on the SD card. Always compiled into
//                    the sysmodule build, but writes only when enabled at
//                    runtime via log_set_enabled() (driven by the `log` key in
//                    config.ini), since a sysmodule has no stdout.
//
// File logging is best-effort: a failed open/write is silently ignored so it
// never affects server behavior.
//
// IMPORTANT: the file sink performs SD-card (fsp-srv) I/O. No filesystem I/O
// may happen during the PSC sleep transition (between the sleep notification
// and acknowledgement) or the console hangs on wake. log_set_suspended(true)
// hard-blocks all writes for that window, regardless of the user setting.

#if defined(ENABLE_LOG)
#include <stdio.h>
#define LOGF(...) printf(__VA_ARGS__)

// No-ops in the app build (stdout is always safe).
static inline void log_set_enabled(bool e) { (void)e; }
static inline void log_set_suspended(bool s) { (void)s; }

#elif defined(LOG_TO_FILE)
#ifndef LOG_FILE_PATH
#define LOG_FILE_PATH "sdmc:/config/sys-autopilot/log.txt"
#endif
// Enable/disable the file sink at runtime. Disabled until called with true.
void log_set_enabled(bool enabled);
// Temporarily block ALL file writes (used across the sleep/wake window so no
// SD-card I/O occurs while the system is suspending). Independent of the
// enabled flag set by log_set_enabled().
void log_set_suspended(bool suspended);
void log_to_file(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
#define LOGF(...) log_to_file(__VA_ARGS__)

#else
#define LOGF(...) ((void)0)
static inline void log_set_enabled(bool e) { (void)e; }
static inline void log_set_suspended(bool s) { (void)s; }
#endif
