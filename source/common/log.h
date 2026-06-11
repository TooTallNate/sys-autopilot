#pragma once

// Lightweight logging. Enabled only in the dev .nro app build (console
// output); compiled out in the sysmodule build where there is no stdout.
#ifdef ENABLE_LOG
#include <stdio.h>
#define LOGF(...) printf(__VA_ARGS__)
#else
#define LOGF(...) ((void)0)
#endif
