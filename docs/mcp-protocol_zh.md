# MCP (Model Context Protocol) Interaction Flow

This file is the English translation of the former Chinese `docs/mcp-protocol_zh.md`.

**Complete specification:** [mcp-protocol.md](./mcp-protocol.md)

MCP rides inside WebSocket or MQTT as:

```json
{
  "session_id": "...",
  "type": "mcp",
  "payload": { "jsonrpc": "2.0", "method": "...", "params": {}, "id": 1 }
}
```

The `payload` is JSON-RPC 2.0. Device methods: `initialize`, `tools/list` (optional `cursor`, `withUserTools`), `tools/call` (`name` + `arguments` object). Notifications whose method starts with `notifications` are ignored. `id` must be a JSON number.

`initialize` result: `protocolVersion: "2024-11-05"`, `capabilities.tools: {}`, `serverInfo.name` = `BOARD_NAME`, `serverInfo.version` = firmware version. Optional `params.capabilities.vision.url` / `token` become the camera explain HTTP endpoint.

`tools/list` paginates when the JSON would exceed 8000 bytes (`nextCursor` = next tool name). User-only tools are omitted unless `withUserTools` is true.

Tool call results:

```json
{
  "jsonrpc": "2.0",
  "id": 1,
  "result": {
    "content": [{ "type": "text", "text": "true" }],
    "isError": false
  }
}
```

See [mcp-protocol.md](./mcp-protocol.md) and [mcp-usage.md](./mcp-usage.md). Device tool catalog for Phoe Lone / otto-robot is in [backend_spec.md](../backend_spec.md).
