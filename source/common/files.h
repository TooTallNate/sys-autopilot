#pragma once

#include "http.h"

// Handlers for the /files endpoint. Each one sends a complete HTTP response.
void files_handle_get(HttpRequest *req);    // file download or directory listing
void files_handle_put(HttpRequest *req);    // upload (streamed write)
void files_handle_delete(HttpRequest *req); // remove file or empty directory
