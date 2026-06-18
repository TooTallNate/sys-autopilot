#include "log.h"

#ifdef LOG_TO_FILE

#include <stdarg.h>
#include <stdio.h>

static bool g_log_enabled;

void log_set_enabled(bool enabled) {
    g_log_enabled = enabled;
}

// Best-effort append to the SD-card log file. Opened per-call (the server is
// single-threaded and not log-heavy) so output is flushed promptly and
// survives a crash. Failures are ignored.
void log_to_file(const char *fmt, ...) {
    if (!g_log_enabled)
        return;
    FILE *f = fopen(LOG_FILE_PATH, "ab");
    if (!f)
        return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fclose(f);
}

#endif
