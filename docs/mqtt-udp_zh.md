# MQTT + UDP Hybrid Communication Protocol

This file is the English translation of the former Chinese `docs/mqtt_udp_zh.md` / `docs/mqtt-udp_zh.md`.

**Complete specification:** [mqtt-udp.md](./mqtt-udp.md)

MQTT carries JSON control messages. UDP carries AES-CTR encrypted Opus audio.

MQTT settings are written by the OTA JSON `mqtt` object into NVS namespace `mqtt`: `endpoint`, `client_id`, `username`, `password`, `keepalive` (default 240 s), `publish_topic`. Default broker port is 8883 if `endpoint` has no `:port`.

Device hello uses `transport: "udp"`. Server hello must include `udp.server`, `udp.port`, `udp.key` (hex AES-128), `udp.nonce` (hex). UDP packets:

```
|type 1B=0x01|flags 1B|payload_len 2B BE|ssrc 4B|timestamp 4B BE|sequence 4B BE|encrypted Opus|
```

Control JSON types match WebSocket (`listen`, `abort`, `mcp`, `stt`, `tts`, `llm`, `system`, `alert`, `goodbye`).

See [mqtt-udp.md](./mqtt-udp.md) for encryption, sequence numbers, and reconnect. For Phoe Lone VPS implementation use [backend_spec.md](../backend_spec.md).
