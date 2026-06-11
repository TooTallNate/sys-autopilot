#pragma once

#include <stdbool.h>

#define CONFIG_DIR  "sdmc:/config/sys-autopilot"
#define CONFIG_PATH CONFIG_DIR "/config.ini"

typedef struct {
    int  port;
    char username[64];
    char password[64];
} Config;

// Loads config from CONFIG_PATH, applying defaults for missing values.
// If the file does not exist, writes a commented default config first.
void config_load(Config *cfg);

// True when basic auth should be enforced (both username and password set).
bool config_auth_enabled(const Config *cfg);
