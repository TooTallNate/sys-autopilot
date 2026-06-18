#!/bin/sh
# Discover sys-autopilot consoles on the local network via DNS-SD (mDNS).
#
# sys-autopilot advertises the "_sys-autopilot._tcp" service, so any console
# running it can be found without knowing its IP address. This script browses
# for that service and prints a ready-to-use base URL (and the advertised TXT
# metadata) for each console found.
#
# Requires either `dns-sd` (macOS, or Bonjour on Windows) or `avahi-browse`
# (Linux). Usage:
#   scripts/discover.sh           # browse for ~5s and print results
#   scripts/discover.sh 10        # browse for 10s
set -eu

SERVICE="_sys-autopilot._tcp"
TIMEOUT="${1:-5}"

print_url() {
    # $1 host, $2 port
    printf 'sys-autopilot: http://%s:%s/  (MCP: http://%s:%s/mcp)\n' \
        "$1" "$2" "$1" "$2"
}

if command -v avahi-browse >/dev/null 2>&1; then
    echo "Browsing for ${SERVICE} via avahi (${TIMEOUT}s)..."
    # -r resolve, -p parseable, -t terminate after cache exhausted.
    avahi-browse -rpt "$SERVICE" 2>/dev/null | awk -F';' '
        $1 == "=" {
            host = $7; port = $9; txt = $10;
            printf "sys-autopilot: http://%s:%s/  (MCP: http://%s:%s/mcp)\n", host, port, host, port;
            if (txt != "") printf "  TXT: %s\n", txt;
        }'
    exit 0
fi

if command -v dns-sd >/dev/null 2>&1; then
    echo "Browsing for ${SERVICE} via dns-sd (${TIMEOUT}s)..."
    # dns-sd runs until killed; capture a browse window, then resolve each
    # instance name found.
    BROWSE_OUT="$(mktemp)"
    trap 'rm -f "$BROWSE_OUT"' EXIT
    ( dns-sd -B "$SERVICE" >"$BROWSE_OUT" 2>/dev/null & echo $! >"$BROWSE_OUT.pid"; ) || true
    sleep "$TIMEOUT"
    kill "$(cat "$BROWSE_OUT.pid" 2>/dev/null)" 2>/dev/null || true
    rm -f "$BROWSE_OUT.pid"

    # Instance names are the trailing field after the service type column.
    INSTANCES="$(awk -v svc="$SERVICE" '$0 ~ svc { $1=$2=$3=$4=$5=$6=""; sub(/^ +/,""); print }' "$BROWSE_OUT" | sort -u)"
    if [ -z "$INSTANCES" ]; then
        echo "No sys-autopilot consoles found. Ensure you are on the same network/subnet."
        exit 1
    fi
    echo "$INSTANCES" | while IFS= read -r name; do
        [ -z "$name" ] && continue
        echo "Found instance: $name"
        echo "  resolve with: dns-sd -L \"$name\" $SERVICE"
        echo "  then connect to http://<name>.local:<port>/"
    done
    exit 0
fi

echo "Error: need 'dns-sd' (macOS/Windows Bonjour) or 'avahi-browse' (Linux)." >&2
echo "On Linux: sudo apt install avahi-utils" >&2
exit 1
