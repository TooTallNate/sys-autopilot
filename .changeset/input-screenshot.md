---
"sys-autopilot": minor
---

Input MCP tools (`tap_buttons`, `tap_sequence`, `hold_buttons`,
`release_buttons`, `set_stick`, `clear_input`) accept an optional
`"screenshot": true` argument that appends a screenshot (taken after an
optional `screenshotDelayMs`, default 250ms) to the tool result as an
image content block — saving the agent a separate screenshot round trip
after every input.
