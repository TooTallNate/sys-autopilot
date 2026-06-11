#pragma once

#include "config.h"

// Optional callback invoked while idle (roughly every 100ms) and between
// requests. Return false to shut the server down (used by the dev .nro app);
// pass NULL to run forever (sysmodule).
typedef bool (*ServerIdleCb)(void);

// Runs the HTTP server (blocking). Binds to cfg->port, enforcing basic auth
// when configured. Retries bind/listen failures internally.
void server_run(const Config *cfg, ServerIdleCb idle);
