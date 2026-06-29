#pragma once

#include "http.h"

// Handles POST /mcp: a stateless MCP "Streamable HTTP" endpoint speaking
// JSON-RPC 2.0. Every request gets a plain application/json response (SSE is
// not needed for a server with no server-initiated messages). Supported
// methods: initialize, notifications/* (202), ping, tools/list, tools/call.
void mcp_handle_post(HttpRequest *req);

// Handles GET /mcp: declines the optional server->client SSE channel with 405
// and Connection: close (we emit no server-initiated messages). Per the
// Streamable HTTP spec this is a valid way to indicate the GET stream is not
// offered; capable clients proceed over POST.
void mcp_handle_get(HttpRequest *req);
