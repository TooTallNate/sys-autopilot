#include "config.h"
#include "log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <sys/stat.h>

static const char *kDefaultConfig =
    "; sys-autopilot configuration\n"
    "; Changes take effect after a reboot (or sysmodule restart).\n"
    "\n"
    "[server]\n"
    "; TCP port the HTTP server listens on.\n"
    "port = 4150\n"
    "\n"
    "; Optional HTTP Basic authentication.\n"
    "; Auth is enforced only when BOTH username and password are set.\n"
    "username =\n"
    "password =\n";

static char *trim(char *s) {
    while (isspace((unsigned char)*s)) s++;
    char *end = s + strlen(s);
    while (end > s && isspace((unsigned char)end[-1])) end--;
    *end = '\0';
    return s;
}

static void write_default_config(void) {
    mkdir("sdmc:/config", 0777);
    mkdir(CONFIG_DIR, 0777);
    FILE *f = fopen(CONFIG_PATH, "wb");
    if (f) {
        fwrite(kDefaultConfig, 1, strlen(kDefaultConfig), f);
        fclose(f);
    }
}

void config_load(Config *cfg) {
    memset(cfg, 0, sizeof(*cfg));
    cfg->port = 4150;

    FILE *f = fopen(CONFIG_PATH, "rb");
    if (!f) {
        write_default_config();
        LOGF("config: wrote default config to %s\n", CONFIG_PATH);
        return;
    }

    char line[256];
    char section[32] = "";
    while (fgets(line, sizeof(line), f)) {
        char *s = trim(line);
        if (*s == '\0' || *s == ';' || *s == '#')
            continue;
        if (*s == '[') {
            char *end = strchr(s, ']');
            if (end) {
                *end = '\0';
                snprintf(section, sizeof(section), "%s", s + 1);
            }
            continue;
        }
        char *eq = strchr(s, '=');
        if (!eq)
            continue;
        *eq = '\0';
        char *key = trim(s);
        char *val = trim(eq + 1);

        if (strcasecmp(section, "server") != 0)
            continue;

        if (strcasecmp(key, "port") == 0) {
            int port = atoi(val);
            if (port > 0 && port <= 65535)
                cfg->port = port;
        } else if (strcasecmp(key, "username") == 0) {
            snprintf(cfg->username, sizeof(cfg->username), "%s", val);
        } else if (strcasecmp(key, "password") == 0) {
            snprintf(cfg->password, sizeof(cfg->password), "%s", val);
        }
    }
    fclose(f);

    LOGF("config: port=%d auth=%s\n", cfg->port,
         config_auth_enabled(cfg) ? "enabled" : "disabled");
}

bool config_auth_enabled(const Config *cfg) {
    return cfg->username[0] != '\0' && cfg->password[0] != '\0';
}
