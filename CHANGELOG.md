# sys-autopilot

## 1.1.0

Initial public release.

- Persistent HTTP server sysmodule (auto-starts at boot via boot2)
- Native MCP endpoint at `POST /mcp` (stateless Streamable HTTP, JSON-RPC 2.0)
  with 12 tools: screenshots returned as image content, controller input
  (taps, sequences, holds, sticks), and SD card file access
- REST API with JSON request bodies for input endpoints
- JPEG screenshots via `caps:sc` (firmware 10.0.0+)
- Virtual Pro Controller input injection via `hiddbg` HDLS
- File upload/download/listing/deletion rooted at `sdmc:/`; MCP
  `upload_file` content is streamed to disk so size is bounded by the SD
  card, not RAM
- INI configuration (`/config/sys-autopilot/config.ini`): port, bearer
  token and/or HTTP Basic credentials
- Dev `.nro` flavor of the same server for fast iteration via nxlink
