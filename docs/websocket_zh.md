# WebSocket Communication Protocol

This file is the English translation of the former Chinese `docs/websocket_zh.md`.

**Complete specification:** [websocket.md](./websocket.md)

The ESP32 client (`main/protocols/websocket_protocol.cc`) connects to the WebSocket URL stored in NVS namespace `websocket` key `url` (written by the OTA HTTP response). Handshake headers:

- `Authorization: Bearer <token>` (token from NVS `websocket.token`; `Bearer ` is prefixed if the token has no space)
- `Protocol-Version`: integer, default 1
- `Device-Id`: Wi-Fi MAC address
- `Client-Id`: UUID from NVS

The device sends a JSON `hello` (`transport: "websocket"`, Opus 16 kHz mono, `features.mcp: true`). The server must reply within 10 seconds with `type: "hello"` and `transport: "websocket"`. After that, text frames are JSON (`tts`, `stt`, `llm`, `mcp`, `system`, `alert`, optional `custom`) and binary frames are Opus audio.

Binary protocol versions: v1 raw Opus, v2 `BinaryProtocol2` (big-endian metadata + timestamp), v3 `BinaryProtocol3`.

See [websocket.md](./websocket.md) for full JSON examples, state machines, and audio rules. For Mickey VPS implementation use [backend_spec.md](../backend_spec.md).
