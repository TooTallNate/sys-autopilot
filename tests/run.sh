#!/bin/sh
# Builds and runs the host-side test suite (no devkitPro required).
set -e
cd "$(dirname "$0")"

SRC=../source/common
JSMN=../lib/jsmn
OUT=build
FAKE_SD=/tmp/sys-autopilot-fakesd

mkdir -p "$OUT"

CFLAGS="-g -Wall -Wextra -Wno-unused-parameter -I$SRC -I$JSMN"

echo "== test_http =="
cc $CFLAGS -o "$OUT/test_http" test_http.c "$SRC/http.c" "$SRC/base64.c"
"$OUT/test_http"

echo "== test_core =="
cc $CFLAGS -o "$OUT/test_core" test_core.c \
    "$SRC/base64.c" "$SRC/buttons.c" "$SRC/json.c" "$SRC/jstream.c"
"$OUT/test_core"

echo "== test_oauth =="
cc $CFLAGS \
    -DOAUTH_TOKENS_PATH="\"$FAKE_SD-tokens.txt\"" \
    -o "$OUT/test_oauth" test_oauth.c \
    "$SRC/oauth.c" "$SRC/sha256.c" "$SRC/http.c" "$SRC/base64.c" \
    "$SRC/json.c" "$SRC/config.c"
"$OUT/test_oauth"

echo "== test_mcp =="
cc $CFLAGS -Ifake \
    -DFILES_ROOT="\"$FAKE_SD\"" \
    -DMCP_UPLOAD_TMP="\"$FAKE_SD/.upload.tmp\"" \
    -DFAKE_SD="\"$FAKE_SD\"" \
    -DOAUTH_TOKENS_PATH="\"$FAKE_SD-mcp-tokens.txt\"" \
    -o "$OUT/test_mcp" test_mcp.c stubs.c \
    "$SRC/mcp.c" "$SRC/http.c" "$SRC/base64.c" "$SRC/json.c" "$SRC/jstream.c" \
    "$SRC/buttons.c" "$SRC/apiargs.c" "$SRC/files.c" "$SRC/power.c" \
    "$SRC/oauth.c" "$SRC/sha256.c" "$SRC/config.c"
"$OUT/test_mcp"

echo "ALL HOST TESTS PASSED"
