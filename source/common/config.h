#pragma once

#include <stdbool.h>
#include <stddef.h>

#define CONFIG_DIR  "sdmc:/config/sys-autopilot"
#define CONFIG_PATH CONFIG_DIR "/config.ini"

typedef struct {
    int  port;
    char username[64];
    char password[64];
    char token[128];
    // mDNS / DNS-SD advertised name (without the ".local" suffix). When empty,
    // a default name is derived from the device serial so discovery still
    // works out of the box and two consoles don't collide.
    char hostname[64];
    // Write diagnostics to sdmc:/config/sys-autopilot/log.txt (sysmodule has
    // no stdout). Off by default.
    bool log;
} Config;

// Prefix for the auto-generated default hostname ("switch-<serial suffix>").
#define CONFIG_DEFAULT_PREFIX "switch"
// How many trailing serial characters to append to the default hostname.
#define CONFIG_SERIAL_SUFFIX_LEN 4

// Loads config from CONFIG_PATH, applying defaults for missing values.
// If the file does not exist, writes a commented default config first.
void config_load(Config *cfg);

// True when auth should be enforced (username+password set, or token set).
bool config_auth_enabled(const Config *cfg);

// Resolves the mDNS/DNS-SD name to advertise into `out` (must be non-NULL,
// cap > 0). Returns `out`. Uses cfg->hostname when set; otherwise builds
// "switch-<last chars of serial>", or just "switch" if `serial` is NULL/empty.
// `serial` may be NULL.
const char *config_hostname(const Config *cfg, const char *serial,
                            char *out, size_t cap);
