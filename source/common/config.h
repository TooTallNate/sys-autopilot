#pragma once

#include <stdbool.h>

#define CONFIG_DIR  "sdmc:/config/sys-autopilot"
#define CONFIG_PATH CONFIG_DIR "/config.ini"

typedef struct {
    int  port;
    char username[64];
    char password[64];
    char token[128];
} Config;

// Loads config from CONFIG_PATH, applying defaults for missing values.
// If the file does not exist, writes a commented default config first.
void config_load(Config *cfg);

// True when auth should be enforced (username+password set, or token set).
bool config_auth_enabled(const Config *cfg);
