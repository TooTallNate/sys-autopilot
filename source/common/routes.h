#pragma once

#include "http.h"

// Dispatches a parsed request to the matching endpoint handler and sends a
// complete HTTP response.
void routes_handle(HttpRequest *req);
