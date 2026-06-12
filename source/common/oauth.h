#pragma once

#include "config.h"
#include "http.h"

// Minimal OAuth 2.1 authorization server + resource server glue for the MCP
// browser auth flow:
//   - RFC 9728 protected resource metadata (/.well-known/oauth-protected-resource)
//   - RFC 8414 AS metadata (/.well-known/oauth-authorization-server)
//   - RFC 7591 dynamic client registration (POST /oauth/register)
//   - Authorization endpoint with an HTML login form (GET/POST /oauth/authorize)
//   - Token endpoint with PKCE S256 (POST /oauth/token)
//
// Issued bearer tokens are non-expiring and persisted one-per-line to
// OAUTH_TOKENS_PATH; revoke by deleting a line. The browser flow requires
// username+password to be configured.

#ifndef OAUTH_TOKENS_PATH
#define OAUTH_TOKENS_PATH CONFIG_DIR "/tokens.txt"
#endif

// Stores the config reference and loads the persisted token list.
void oauth_init(const Config *cfg);

// True if `token` matches an issued token (constant-time per entry). Reloads
// the tokens file when its mtime changes, so edits apply without a reboot.
bool oauth_token_valid(const char *token);

// Endpoint handlers (each sends a complete HTTP response).
void oauth_handle_protected_resource(HttpRequest *req); // GET well-known
void oauth_handle_as_metadata(HttpRequest *req);        // GET well-known
void oauth_handle_register(HttpRequest *req);           // POST /oauth/register
void oauth_handle_authorize_get(HttpRequest *req);      // GET /oauth/authorize
void oauth_handle_authorize_post(HttpRequest *req);     // POST /oauth/authorize
void oauth_handle_token(HttpRequest *req);              // POST /oauth/token
