#!/usr/bin/env python3
"""Generates source/common/mcp_tools.h (the MCP tools/list payload)."""
import json
import os

BTN_DESC = ("Button names: A, B, X, Y, L, R, ZL, ZR, PLUS, MINUS, UP, DOWN, LEFT, "
            "RIGHT, LSTICK, RSTICK, HOME, CAPTURE.")

def buttons_prop():
    return {"type": "array", "items": {"type": "string"},
            "description": "Buttons to act on. " + BTN_DESC}

path_prop = {"type": "string",
             "description": "Absolute path on the SD card, e.g. /switch/myapp/log.txt"}

tools = [
    {
        "name": "screenshot",
        "description": ("Capture the current Switch screen as a JPEG image (1280x720). "
                        "Returns the image directly so you can see what is on screen."),
        "inputSchema": {"type": "object", "properties": {
            "stack": {"type": "string",
                      "enum": ["screenshot", "default", "lcd", "recording", "lastframe"],
                      "description": "Layer stack to capture. Default: screenshot (what the Capture button sees)."}
        }},
    },
    {
        "name": "tap_buttons",
        "description": ("Press and release controller buttons (synchronous). "
                        "A virtual Pro Controller is attached automatically. " + BTN_DESC),
        "inputSchema": {"type": "object", "properties": {
            "buttons": buttons_prop(),
            "durationMs": {"type": "integer",
                           "description": "How long to hold before releasing, in ms. Default 100, max 10000."},
        }, "required": ["buttons"]},
    },
    {
        "name": "tap_sequence",
        "description": ("Perform a sequence of button taps in one call, e.g. to navigate menus. "
                        "Each step presses its buttons, holds, releases, then waits before the next step. "
                        + BTN_DESC),
        "inputSchema": {"type": "object", "properties": {
            "taps": {"type": "array", "maxItems": 32,
                     "description": "Sequence of taps (max 32).",
                     "items": {"type": "object", "properties": {
                         "buttons": buttons_prop(),
                         "durationMs": {"type": "integer", "description": "Hold time in ms. Default 100."},
                         "delayAfterMs": {"type": "integer", "description": "Pause after release in ms. Default 150."},
                     }, "required": ["buttons"]}},
        }, "required": ["taps"]},
    },
    {
        "name": "hold_buttons",
        "description": "Press buttons and keep them held until release_buttons or clear_input. " + BTN_DESC,
        "inputSchema": {"type": "object", "properties": {"buttons": buttons_prop()},
                        "required": ["buttons"]},
    },
    {
        "name": "release_buttons",
        "description": "Release previously held buttons.",
        "inputSchema": {"type": "object", "properties": {"buttons": buttons_prop()},
                        "required": ["buttons"]},
    },
    {
        "name": "set_stick",
        "description": ("Set an analog stick position. x/y range -1.0..1.0 (y=1.0 is up). "
                        "With durationMs the stick recenters afterwards; without it the "
                        "position persists until changed or clear_input."),
        "inputSchema": {"type": "object", "properties": {
            "side": {"type": "string", "enum": ["left", "right"]},
            "x": {"type": "number"},
            "y": {"type": "number"},
            "durationMs": {"type": "integer", "description": "Hold time before recentering. Max 10000."},
        }, "required": ["side"]},
    },
    {
        "name": "clear_input",
        "description": "Release all held buttons and recenter both sticks.",
        "inputSchema": {"type": "object", "properties": {}},
    },
    {
        "name": "status",
        "description": "Get server status: version, console firmware, virtual controller state, uptime.",
        "inputSchema": {"type": "object", "properties": {}},
    },
    {
        "name": "list_directory",
        "description": "List a directory on the SD card. Returns JSON with name/type/size/mtime per entry.",
        "inputSchema": {"type": "object", "properties": {"path": path_prop},
                        "required": ["path"]},
    },
    {
        "name": "read_file",
        "description": ("Read a text file from the SD card (logs, configs). Returns up to 32 KB per call; "
                        "use offset/length to page through larger files. A negative offset reads from "
                        "the end of the file (tail)."),
        "inputSchema": {"type": "object", "properties": {
            "path": path_prop,
            "offset": {"type": "integer", "description": "Byte offset; negative = from end of file."},
            "length": {"type": "integer", "description": "Max bytes to read (capped at 32768)."},
        }, "required": ["path"]},
    },
    {
        "name": "upload_file",
        "description": ("Write a file to the SD card. content is base64-encoded and is streamed to disk, "
                        "so size is limited only by SD space - but large binaries are cheaper to deploy "
                        "via the raw HTTP API (curl -T file 'http://<ip>:<port>/files?path=...')."),
        "inputSchema": {"type": "object", "properties": {
            "path": path_prop,
            "content": {"type": "string", "description": "Base64-encoded file content."},
        }, "required": ["path", "content"]},
    },
    {
        "name": "delete_file",
        "description": "Delete a file or empty directory on the SD card.",
        "inputSchema": {"type": "object", "properties": {"path": path_prop},
                        "required": ["path"]},
    },
]

payload = json.dumps({"tools": tools}, separators=(",", ":"))
json.loads(payload)  # sanity

def c_escape(s):
    return s.replace("\\", "\\\\").replace('"', '\\"')

lines = [
    "// Generated tools/list payload. Regenerate with scripts/gen_tools.py if",
    "// you change the tool set (or edit carefully by hand and validate as JSON).",
    "#pragma once",
    "",
    "static const char kMcpToolsJson[] =",
]
chunk = 100
for i in range(0, len(payload), chunk):
    lines.append('    "%s"' % c_escape(payload[i:i+chunk]))
lines[-1] += ";"

out = os.path.join(os.path.dirname(__file__), "..", "source", "common", "mcp_tools.h")
with open(out, "w") as f:
    f.write("\n".join(lines) + "\n")
print("wrote", os.path.normpath(out), f"({len(payload)} JSON bytes)")
