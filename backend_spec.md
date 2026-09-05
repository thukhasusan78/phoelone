# Mickey Backend Specification

This document is the XiaoZhi **wire protocol** (OTA, WebSocket, MCP, audio framing) as implemented by the Mickey ESP-IDF client (`main/boards/mickey/`). How the FastAPI repo runs (Silero VAD, Gemini Live, companion dashboard) is in [phoelone-backend](https://github.com/thukhasusan78/phoelone-backend). Do **not** invent extra device-side APIs.

**Primary transport for Mickey:** WebSocket. Companion builds (`CONFIG_COMPANION_KEEP_CHANNEL`) **prefer WebSocket even if OTA also includes `mqtt`**. Omit `mqtt` from production OTA JSON anyway.

**Firmware profile:** `python scripts/build.py mickey --name mickey --language en-US`  
**Chip:** ESP32-S3, 16 MB flash, 8 MB octal PSRAM  
**Board identity:** `board.type` / `board.name` = **`mickey`** (legacy OTA identities `otto-robot` and `phoe-lone` are still accepted by the VPS).  
**Baked OTA URL:** `https://phoelone.thukha.online/xiaozhi/ota/` in `main/boards/mickey/config.json`.

Point a lab device at another host with NVS `wifi.ota_url` or a one-off `sdkconfig` `CONFIG_OTA_URL`. Prefer HTTPS in production.

---

## 1. System architecture

```
ESP32 (mickey firmware)
  │  boot
  ├─ HTTP POST  /xiaozhi/ota/     →  writes websocket NVS, optional firmware URL
  ├─ WebSocket  wss://host/xiaozhi/v1/   →  hello, Opus, JSON, MCP, ping/pong
  └─ companion keepalive keeps the socket open in idle (no listen/start)

VPS backend (you implement)
  ├─ OTA HTTP service
  ├─ WebSocket voice session (ASR → LLM → TTS → MCP)
  ├─ Server-side tools (weather, news, music, knowledge, email, …)
  └─ Device-side MCP client (initialize, tools/list, tools/call)
```

The ESP32 is an **MCP server** (it exposes tools). The VPS is an **MCP client** (it discovers and calls those tools) **and** an LLM host that also has **its own** tools (weather/news/music). Device tools never include `search_weather`; those live only on the server.

---

## 2. Boot and OTA HTTP

### 2.1 When it runs

After Wi-Fi connects, `Ota::CheckVersion()` (`main/ota.cc`) POSTs (or GETs if body empty) to the OTA URL. A 200 JSON body is required. Non-200 fails activation.

### 2.2 Request

- **Methods:** `POST` (normal) or `GET` if the device has no system-info body.
- **URL:** `CONFIG_OTA_URL` or NVS `wifi` / `ota_url`.
- **Also accept** the same path without trailing slash.

**Headers (always):**

| Header | Value |
|--------|--------|
| `Activation-Version` | `"1"` or `"2"` (2 if eFuse serial exists) |
| `Device-Id` | Wi-Fi MAC, e.g. `aa:bb:cc:dd:ee:ff` |
| `Client-Id` | UUID string (NVS; changes if NVS erased) |
| `Serial-Number` | only if Activation-Version is 2 |
| `User-Agent` | ESP-IDF user-agent string |
| `Accept-Language` | firmware language code, e.g. `en-US` |
| `Content-Type` | `application/json` |

**POST body** (`Board::GetSystemInfoJson()`), schema (version 2):

```json
{
  "version": 2,
  "language": "en-US",
  "flash_size": 16777216,
  "minimum_free_heap_size": "123456",
  "mac_address": "aa:bb:cc:dd:ee:ff",
  "uuid": "xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx",
  "chip_model_name": "esp32s3",
  "chip_info": { "model": 9, "cores": 2, "revision": 0, "features": 0 },
  "application": {
    "name": "xiaozhi",
    "version": "2.4.2",
    "compile_time": "Aug 18 2026T12:00:00Z",
    "idf_version": "v6.0.2",
    "elf_sha256": "<64 hex chars>"
  },
  "partition_table": [
    { "label": "nvs", "type": 1, "subtype": 2, "address": 0, "size": 0 }
  ],
  "ota": { "label": "ota_0" },
  "display": { "monochrome": false, "width": 240, "height": 240 },
  "board": {
    "type": "mickey",
    "name": "mickey",
    "manufacturer": "<BOARD_MANUFACTURER>",
    "ssid": "HomeWifi",
    "rssi": -45,
    "channel": 6,
    "ip": "192.168.1.50",
    "mac": "aa:bb:cc:dd:ee:ff"
  }
}
```

`board.ssid` / `rssi` / `channel` / `ip` are omitted while the device is in Wi-Fi config mode.

### 2.3 Response (HTTP 200 JSON)

The device parses these **top-level objects**. Unknown keys are ignored. String/number fields inside `websocket` and `mqtt` are copied **verbatim** into NVS namespaces of the same name.

```json
{
  "websocket": {
    "url": "ws://<VPS-HOST>:8000/xiaozhi/v1/",
    "token": "<opaque-token>",
    "version": 1
  },
  "mqtt": {
    "endpoint": "mqtt.example.com:8883",
    "client_id": "device-mac",
    "username": "user",
    "password": "pass",
    "keepalive": 240,
    "publish_topic": "device/out"
  },
  "server_time": {
    "timestamp": 1710000000000,
    "timezone_offset": 390
  },
  "firmware": {
    "version": "0.0.0",
    "url": "http://<VPS-HOST>:8000/firmware/none.bin",
    "force": 0
  },
  "activation": {
    "message": "Enter this code on the console",
    "code": "123456",
    "challenge": "<optional hmac challenge>",
    "timeout_ms": 30000
  }
}
```

**Field rules:**

| Object | Required for Mickey WS | Behavior |
|--------|---------------------------|----------|
| `websocket.url` | **Yes** | Stored as NVS `websocket`/`url`. Must be `ws://` or `wss://`. Path typically `/xiaozhi/v1/` or `/xiaozhi/v1`. |
| `websocket.token` | Recommended | NVS `websocket`/`token`. Device sends `Authorization: Bearer <token>` unless the token already contains a space. |
| `websocket.version` | Optional number | NVS `websocket`/`version`. `1` (default raw Opus), `2`, or `3`. |
| `mqtt.*` | Only if using MQTT transport | Every string/number child is stored under NVS `mqtt`. If omitted, device logs “No mqtt section”. |
| `server_time.timestamp` | Recommended | Milliseconds. Device adds `timezone_offset` minutes then `settimeofday`. Myanmar offset example: `390`. |
| `firmware.version` + `firmware.url` | Recommended | If `version` is **newer** than running firmware, device starts OTA download. Use a dummy version `0.0.0` and a non-downloadable URL to skip upgrades. `force: 1` forces install even if not newer. |
| `activation` | Optional | If `code` is present, device shows activation UI and plays digit sounds. Omit this object for an always-open local/VPS server. |

**Do not** advertise MQTT in production OTA. Companion Mickey prefers WebSocket if both sections exist.

Mickey does **not** register `self.set_press_to_talk` (no hold-to-talk / tap-to-talk tool). Volume, brightness, theme, and trims remain.

---

## 3. WebSocket voice session (required)

### 3.1 Connection

- **URL:** NVS `websocket.url` from OTA.
- **Subprotocol:** none required.
- **Timeouts:** server hello must arrive within **10 s**. Idle timeout on device is **120 s** since last incoming frame, unless a companion heartbeat `pong` send succeeds (that send refreshes the timer so a silent READY socket is not treated as dead). Failed hello/connect paths **close** the socket immediately so the VPS does not accumulate half-open sessions.
- **Companion radio:** while `/xiaozhi/v1/` is open, Mickey keeps Wi-Fi in PERFORMANCE (no `WIFI_PS_MAX_MODEM`). Modem sleep on an idle TLS socket was dropping the session, then reconnecting every 1 s, which flaps the dashboard and can stall the droplet (including SSH). Short-lived sessions back off 1→2→4…→60 s; backoff resets only after 20 s stable.

Handshake headers from device:

```
Authorization: Bearer <token>
Protocol-Version: 1
Device-Id: <mac>
Client-Id: <uuid>
```

### 3.2 Device → server `hello` (first text frame)

Built by `WebsocketProtocol::GetHelloMessage()`:

```json
{
  "type": "hello",
  "version": 1,
  "features": {
    "mcp": true
  },
  "transport": "websocket",
  "audio_params": {
    "format": "opus",
    "sample_rate": 16000,
    "channels": 1,
    "frame_duration": 60
  }
}
```

If `CONFIG_USE_SERVER_AEC` is enabled, `features.aec` is `true`. If assets advertise glyph push, `features.glyph_push` is `true` and `text_font` is present (`bundle`, `charset`, `size`, `bpp`).

`version` matches `Protocol-Version` (1/2/3). `frame_duration` is `OPUS_FRAME_DURATION_MS` = **60**.

### 3.3 Server → device `hello` (required)

Must include `"type":"hello"` and `"transport":"websocket"`. Any other `transport` is rejected and the channel never opens.

```json
{
  "type": "hello",
  "transport": "websocket",
  "session_id": "a-uuid-you-generate",
  "audio_params": {
    "format": "opus",
    "sample_rate": 24000,
    "channels": 1,
    "frame_duration": 60
  }
}
```

The device stores `session_id` and uses `audio_params.sample_rate` / `frame_duration` for **downlink** Opus decode (typically 24000 Hz). Uplink stays 16000 Hz.

### 3.4 Binary audio

| Direction | Content |
|-----------|---------|
| Device → server | Opus frames of microphone audio after local processing. **v1:** raw Opus bytes as WS binary. **v2:** packed `BinaryProtocol2` (all multi-byte fields **network byte order**). **v3:** `BinaryProtocol3`. |
| Server → device | Opus frames to play. Device **drops** downlink audio while in `listening` state. Send audio only after `tts/start` (device enters `speaking`). |

`BinaryProtocol2` (`__attribute__((packed))`):

```
uint16 version; uint16 type;  // type 0 = OPUS, 1 = JSON
uint32 reserved;
uint32 timestamp_ms;          // useful if features.aec
uint32 payload_size;
uint8  payload[];
```

`BinaryProtocol3`:

```
uint8  type;
uint8  reserved;
uint16 payload_size;          // network byte order
uint8  payload[];
```

Uplink Opus: 16 kHz, mono, 60 ms frames. Downlink: match `audio_params` in server hello (24 kHz recommended).

### 3.5 Device → server JSON

Every message except the first hello includes `session_id`.

#### listen / start

```json
{ "session_id": "xxx", "type": "listen", "state": "start", "mode": "auto" }
```

`mode`:

- `"auto"` — `kListeningModeAutoStop` (typical after wake word / button in auto mode)
- `"manual"` — press-to-talk / click-to-talk until stop
- `"realtime"` — full duplex / server AEC mode

After `start`, the device streams binary Opus until `listen/stop`, abort, or `tts/start`.

**This backend:** in `auto` / wake-word mode the ESP32 often never sends `listen/stop`. The server runs Silero VAD, ends the Gemini turn on silence (or `MAX_FORWARDED_AUDIO_SECONDS`), and sends `tts/start` so the device leaves listening.

#### listen / stop

```json
{ "session_id": "xxx", "type": "listen", "state": "stop" }
```

#### listen / detect (wake word)

```json
{ "session_id": "xxx", "type": "listen", "state": "detect", "text": "Hi XiaoZhi" }
```

`text` is the wake-word phrase. Opus of the wake snippet may already have been sent.

#### abort

```json
{ "session_id": "xxx", "type": "abort", "reason": "wake_word_detected" }
```

`reason` is omitted unless abort is due to a new wake word (`kAbortReasonWakeWordDetected`). Stop TTS immediately.

#### mcp

```json
{ "session_id": "xxx", "type": "mcp", "payload": { "jsonrpc": "2.0", "id": 1, "result": {} } }
```

`payload` is a JSON-RPC **response** (or later a notification). See §5.

WebSocket does **not** send `goodbye`. Closing the socket is enough.

### 3.6 Server → device JSON (`Application::OnIncomingJson`)

Unknown `type` is logged and ignored. Missing `type` is logged as an error.

#### tts

```json
{ "session_id": "xxx", "type": "tts", "state": "start" }
```

Device → `kDeviceStateSpeaking`, starts playing binary Opus.

```json
{ "session_id": "xxx", "type": "tts", "state": "sentence_start", "text": "Hello, I am Mickey." }
```

Shows assistant subtitle. Optional glyph-push fields: see `docs/glyph-push.md`.

```json
{ "session_id": "xxx", "type": "tts", "state": "stop" }
```

If listening mode is manual → Idle; else → Listening again (auto continue).

**Minimum TTS cycle for a spoken reply:** `start` → one or more `sentence_start` + binary Opus → `stop`.

#### stt

```json
{ "session_id": "xxx", "type": "stt", "text": "walk forward two steps" }
```

Shows user bubble. Send after ASR of the current utterance.

#### llm (face / emotion)

```json
{ "session_id": "xxx", "type": "llm", "emotion": "happy", "text": "😀" }
```

`emotion` is passed to `Display::SetEmotion`. Otto GIF assets use names such as `staticstate`, `neutral`, `happy`, `sad`, `sleepy`, plus other otto-gif keys. If unknown, the display logs and may keep the last face. `text` is optional for servers; the official cloud sometimes sends an emoji.

#### mcp

```json
{
  "session_id": "xxx",
  "type": "mcp",
  "payload": {
    "jsonrpc": "2.0",
    "method": "tools/call",
    "id": 2,
    "params": { "name": "self.otto.action", "arguments": { "action": "walk", "steps": 1, "speed": 2000, "direction": 1 } }
  }
}
```

#### system

```json
{ "session_id": "xxx", "type": "system", "command": "reboot" }
```

Only `reboot` is implemented. Device schedules `Application::Reboot()`.

#### alert

All three strings required:

```json
{
  "session_id": "xxx",
  "type": "alert",
  "status": "Warning",
  "message": "Battery low",
  "emotion": "sad"
}
```

Plays vibration OGG and shows status/message/emotion.

#### custom (only if `CONFIG_RECEIVE_CUSTOM_MESSAGE`)

```json
{ "session_id": "xxx", "type": "custom", "payload": { "message": "anything" } }
```

Shows `payload` JSON as a system chat line. Otto default builds may not enable this.

#### ping (keepalive)

```json
{ "session_id": "xxx", "type": "ping", "ts_ms": 1710000000000 }
```

Device replies `{ "type": "pong", "ts_ms": <echo or device clock> }`. Send every ~30 s **including during TTS/music** so the device 120 s idle timer does not fire.

Mickey companion keepalive also sends **unsolicited** `pong` every `CONFIG_COMPANION_HEARTBEAT_INTERVAL_S` (default 30) while `/xiaozhi/v1/` is open in idle. Treat inbound `pong` as a known type; do not require it before the session is valid.

### 3.7 Recommended session sequence (auto mode)

1. Device connects, sends `hello`.
2. Server replies `hello` with `session_id`.
3. Server immediately sends MCP `initialize` then `tools/list` (may be after first listen; doing it at hello is better so tools exist before the LLM turn).
4. Device sends `listen/start` + Opus.
5. Server: ASR → `stt` → LLM (optionally MCP `tools/call`) → `llm` emotion → `tts/start` → Opus + `sentence_start` → `tts/stop`.
6. Device returns to listening (auto) or idle (manual).
7. Socket close → device Idle. With `CONFIG_COMPANION_KEEP_CHANNEL`, Mickey reconnects without `listen/start` so the dashboard can send MCP while idle.

### 3.8 Companion idle socket

After activation Mickey opens `/xiaozhi/v1/` and returns to `kDeviceStateIdle` with the channel still open and Wi-Fi left in PERFORMANCE. Do not idle-close a silent READY session for at least several minutes; the device heartbeats with `pong`. Unexpected drops reconnect with exponential backoff (1–60 s), not every second. Deep sleep (`self.mickey.sleep.now`) emits `notifications/phoe_lone.event` with `"event":"sleep"` then closes the socket. The dashboard should show sleeping until the alarm, not a generic offline error.

---

## 4. MQTT + UDP (optional)

Implement if OTA includes a `mqtt` object. JSON **control** messages are identical to §3.5–3.6 except:

- Device hello uses `"transport": "udp"` and typically `"version": 3`.
- Server hello **must** include:

```json
{
  "type": "hello",
  "transport": "udp",
  "session_id": "xxx",
  "audio_params": { "format": "opus", "sample_rate": 24000, "channels": 1, "frame_duration": 60 },
  "udp": {
    "server": "203.0.113.10",
    "port": 8888,
    "key": "0123456789ABCDEF0123456789ABCDEF",
    "nonce": "0123456789ABCDEF0123456789ABCDEF"
  }
}
```

`key` and `nonce` are hex-encoded 128-bit AES-CTR material.

- MQTT settings from OTA: `endpoint` (`host` or `host:port`, default port **8883**), `client_id`, `username`, `password`, `keepalive` (default 240), `publish_topic` (device publishes hello/listen/mcp here). Incoming MQTT payloads are parsed as JSON (subscribe topic is server-defined; typically a per-device topic).
- Extra types: device may send `{ "type": "goodbye", "session_id": "xxx" }`. Server `goodbye` with matching `session_id` closes UDP **without** the device echoing goodbye.
- UDP packet (AES-CTR on Opus payload): `type=0x01`, `flags`, `payload_len` BE, `ssrc`, `timestamp` BE, `sequence` BE, then ciphertext. Counter is derived from timestamp + sequence (see `mqtt_protocol.cc` and `docs/mqtt-udp.md`). Drop replayed sequences.

Mickey can ship WS-only: omit `mqtt` from OTA JSON.

---

## 5. MCP (device tools)

### 5.1 Envelope

Always wrap JSON-RPC in:

```json
{ "session_id": "<from hello>", "type": "mcp", "payload": { } }
```

`payload.jsonrpc` **must** be `"2.0"`. Request/response `payload.id` **must** be a JSON **number** (not a string). JSON-RPC notifications have **no** `id`. The server handles `notifications/phoe_lone.event` (`pickup` | `putdown` | `fall` | `pet` | `sleep`) and must not send a JSON-RPC reply. Unknown notification methods are logged and dropped. Firmware does **not** emit `bright` / `dark` (no light GPIO). Params are `{ "event", "ts_ms", "imu": { "pitch", "az" } }` — no `light` object.

Do **not** send `type: llm` with `confused` on fall. Firmware already homes servos and does not change the face on fall (pickup uses `surprised`). `confused` maps to `sad` on Mickey.

Device replies are sent with `Protocol::SendMcpMessage` as the same envelope; `payload` is already a JSON-RPC object string.

### 5.2 initialize

**Server → device**

```json
{
  "jsonrpc": "2.0",
  "method": "initialize",
  "id": 1,
  "params": {
    "capabilities": {
      "vision": {
        "url": "http://<VPS>/vision/explain",
        "token": "optional-bearer"
      }
    }
  }
}
```

`vision.url` must be **HTTP(S)**, never WebSocket. Otto no-camera builds ignore it (no camera). If a camera exists, `self.camera.take_photo` POSTs the JPEG there.

**Device → server**

```json
{
  "jsonrpc": "2.0",
  "id": 1,
  "result": {
    "protocolVersion": "2024-11-05",
    "capabilities": { "tools": {} },
    "serverInfo": { "name": "mickey", "version": "2.4.2" }
  }
}
```

(`name` is compile-time `BOARD_NAME`.)

### 5.3 tools/list

```json
{
  "jsonrpc": "2.0",
  "method": "tools/list",
  "id": 2,
  "params": { "cursor": "", "withUserTools": false }
}
```

Response:

```json
{
  "jsonrpc": "2.0",
  "id": 2,
  "result": {
    "tools": [
      {
        "name": "self.get_device_status",
        "description": "...",
        "inputSchema": { "type": "object", "properties": {} }
      }
    ]
  }
}
```

If `nextCursor` is a non-empty string, call `tools/list` again with `cursor` equal to that name. Max payload ~8000 bytes.

`withUserTools: true` includes user-only tools (`annotations.audience: ["user"]`). The LLM should **not** receive those.

### 5.4 tools/call

```json
{
  "jsonrpc": "2.0",
  "method": "tools/call",
  "id": 3,
  "params": {
    "name": "self.audio_speaker.set_volume",
    "arguments": { "volume": 70 }
  }
}
```

Missing required args → JSON-RPC error `{ "message": "Missing valid argument: ..." }`. Unknown tool → `"Unknown tool: ..."`.

**Success result** (device wraps return values as text):

```json
{
  "jsonrpc": "2.0",
  "id": 3,
  "result": {
    "content": [{ "type": "text", "text": "true" }],
    "isError": false
  }
}
```

- `bool` → `"true"` / `"false"`
- `int` → decimal string
- `string` / `cJSON*` → that string (JSON objects are **stringified**, not nested)
- image content is rare (camera explain may return JSON text)

Tool callbacks run on the application task; do not block the WS loop on the server waiting more than a few seconds for motion tools.

### 5.5 Common tools (all otto-robot builds)

Registered in `McpServer::AddCommonTools` (`main/mcp_server.cc`). Visible to the LLM.

#### `self.get_device_status`

- Args: none
- Returns JSON string, example:

```json
{
  "audio_speaker": { "volume": 70 },
  "screen": { "brightness": 80, "theme": "dark" },
  "battery": { "level": 85, "charging": false },
  "network": { "type": "wifi", "ssid": "HomeWifi", "signal": "strong" },
  "chip": { "temperature": 45.2 }
}
```

`signal` is `strong` (rssi ≥ -60), `medium` (≥ -70), or `weak`. Battery/chip omitted if hardware reports none.

#### `self.audio_speaker.set_volume`

- `volume` (integer, **required**, 0–100)

#### `self.screen.set_brightness`

- `brightness` (integer, **required**, 0–100)
- Only registered if the board has a backlight (otto does: GPIO BLK 3).

#### `self.screen.set_theme`

- `theme` (string, **required**): `"light"` or `"dark"`

#### `self.camera.take_photo`

- Only if a camera object exists. Otto **no-camera** auto-detect does **not** register this.
- `question` (string, required): what to ask about the photo.
- Device captures, then HTTP-explains via the vision URL from `initialize`.

### 5.6 User-only tools (companion app)

Registered by `AddUserOnlyTools`. List only with `withUserTools: true`. Do not give these to the LLM.

| Name | Args | Effect |
|------|------|--------|
| `self.get_system_info` | none | Same JSON as OTA POST body |
| `self.reboot` | none | Reboots after ~1 s; returns `true` immediately |
| `self.upgrade_firmware` | `url` string | Downloads firmware from URL, installs, reboots |
| `self.screen.get_info` | none | `{ "width": 240, "height": 240, "monochrome": false }` |
| `self.screen.snapshot` | `url`, `quality` 1–100 default 80 | JPEG multipart POST `file=screenshot.jpg` |
| `self.screen.preview_image` | `url` | HTTP GET image, show on LCD |
| `self.assets.set_download_url` | `url` | NVS `assets`/`download_url` |

Snapshot/preview require `CONFIG_LV_USE_SNAPSHOT`.

### 5.7 Press-to-talk

`self.set_press_to_talk` is registered only on boards that construct `PressToTalkMcpTool`. **Mickey does not.** The dashboard should not offer Hold-to-Talk / Tap-to-Talk for this SKU.

### 5.8 Otto / Mickey motion tools (`otto_controller.cc`)

These are LLM-visible. On the **no-camera** Otto profile, hands GPIOs are `NC`; hand actions return an error string.

#### `self.otto.action`

Unified locomotion / dance / pose tool.

| Property | Type | Default | Range | Meaning |
|----------|------|---------|-------|---------|
| `action` | string | `"sit"` | see list | Motion name |
| `steps` | int | 3 | 1–100 | Repeats |
| `speed` | int | 700 | 100–3000 | **Smaller = faster** (ms-style) |
| `direction` | int | 1 | -1, 0, 1 | 1=forward/left, -1=back/right, 0=both |
| `amount` | int | 30 | 0–170 | Amplitude |

**`action` values (4-servo no-hands Mickey):**

| action | Extra args used | Notes |
|--------|-----------------|-------|
| `walk` | steps, speed, direction | Forward/back walk |
| `turn` | steps, speed, direction | Turn in place |
| `jump` | steps, speed | |
| `swing` | steps, speed, amount | |
| `moonwalk` | steps, speed, direction, amount | |
| `bend` | steps, speed, direction | |
| `shake_leg` | steps, speed, direction | |
| `updown` | steps, speed, amount | |
| `whirlwind_leg` | steps, speed, amount | |
| `sit` | (ignored) | Sit pose |
| `showcase` | | Demo sequence |
| `home` | | Return to stand / home |

This robot has **no hand servos**. Do not advertise or call `hands_up`, `hands_down`, `hand_wave`, `windmill`, `takeoff`, `fitness`, `greeting`, `shy`, `radio_calisthenics`, `magic_circle`, or `arm_swing`.

Unknown action returns an error listing the available names above.

**Voice mapping for the LLM (examples):**

- “move forward” / “walk” → `action=walk`, `direction=1`, `steps=2`, `speed=2000` (slow first tests)
- “go back” → `walk`, `direction=-1`
- “turn left” → `turn`, `direction=1`
- “turn right” → `turn`, `direction=-1`
- “dance” / “swing” → `swing` or `showcase`
- “jump” → `jump`
- “sit down” → `sit`
- “stand” / “reset” / “home” → `home`
- “stop” → **do not** use this tool; call `self.otto.stop`

Return: `true` or an error string.

#### `self.otto.stop`

- Args: none
- Deletes the action task, clears the queue, homes servos. **Always expose this to the LLM for safety.**

#### `self.otto.servo_sequences`

AI-authored servo programming. Single argument `sequence` (string containing JSON).

Top-level JSON:

```json
{
  "a": [ { "s": { "ll": 90, "rl": 90 }, "v": 1000, "d": 0 } ],
  "d": 0
}
```

- `a`: array of action objects
- top-level `d`: delay ms after the sequence

Each action is either:

**Move mode:** `s` map of servo → 0–180 deg, `v` duration 100–3000 ms (default 1000), `d` post-delay ms.

Servo keys: `ll` left leg, `rl` right leg, `lf` left foot, `rf` right foot. There are no hand keys.

**Oscillator mode:** `osc` object with `a` amplitudes, `o` centers, `ph` phase degrees, `p` period ms, `c` cycle count.

Safety: when oscillating legs/feet, **one foot must stay at 90°**. After multiple sequences, call `self.otto.action` with `home` or `self.otto.stop`. Queue holds 10 items; send short sequences.

The `sequence` property is a **string** (JSON escaped inside the MCP arguments), e.g.:

```json
{
  "name": "self.otto.servo_sequences",
  "arguments": {
    "sequence": "{\"a\":[{\"s\":{\"ll\":90,\"rl\":90},\"v\":1000}]}"
  }
}
```

#### `self.otto.set_trim`

- `servo_type`: `left_leg` | `right_leg` | `left_foot` | `right_foot`
- `trim_value`: int -50..50, persisted in NVS `otto_trims`
- Triggers a small jump to preview.

#### `self.otto.get_trims`

Returns JSON string:

```json
{ "left_leg": 0, "right_leg": 0, "left_foot": 0, "right_foot": 0 }
```

#### `self.otto.get_status`

Returns plain text `"moving"` or `"idle"`.

#### `self.battery.get_level`

```json
{ "level": 85, "charging": false }
```

#### `self.otto.get_ip`

```json
{ "ip": "192.168.1.50", "connected": true }
```

Empty IP → `{ "ip": "", "connected": false }`.

#### Phoe Lone / Mickey sensors (live IMU + touch; light stub)

Firmware registers **both** namespaces. Prefer `self.phoe_lone.*` (backend catalog). `self.mickey.*` remains as an alias for IMU/touch/light.

| Name | Return JSON |
|------|-------------|
| `self.phoe_lone.imu.get_reading` / `self.mickey.imu.get_reading` | Live MPU6050: `{ "wired": true, "sensor": "MPU6050", "ax", "ay", "az", "gx", "gy", "gz", "pitch", "roll", "temp_c", "event" }` where `event` is `still` \| `moving` \| `pickup` \| `putdown` \| `fall` \| `shake`. Fail: `{ "wired": true, "ok": false, "error": "i2c_nack" }`. |
| `self.phoe_lone.touch.get_state` / `self.mickey.touch.get_state` | Live TTP223: `{ "wired": true, "touched", "count", "ms_held" }`. |
| `self.phoe_lone.light.get_level` / `self.mickey.light.get_level` | Still unwired: `{ "wired": false, "sensor": "light", "reason": "..." }`. |

A pet may also arrive as `notifications/phoe_lone.event`. If `wired:false` or `ok:false`, say the sensor is not connected — never invent ax/lux.

Mickey does **not** expose `self.set_press_to_talk`. Alarm tools are `self.mickey.alarm.set` / `.get` / `.cancel` and `self.mickey.sleep.now` (no `self.phoe_lone.alarm.*` alias).

---

## 6. Server-side LLM tools (NOT on the ESP32)

The official XiaoZhi cloud and compatible servers expose **host** tools. The device never registers `search_weather`. Implement these on the VPS and let the LLM call them **locally**, then speak the answer via TTS.

Minimum set so “nothing is left behind” versus a full XiaoZhi-style assistant:

| Server tool | Typical args | Behavior |
|-------------|--------------|----------|
| `search_weather` | `location` string, optional `date` | Query a weather API; summarize in the user’s language; TTS the forecast. Do **not** send this name to device MCP. |
| `search_news` | `query` or `topic`, optional `count` | News API / RSS; speak headlines. |
| `search_music` | `query`, optional `play` bool | This backend auto-scans `data/local_music/` (`Artist - Title.mp3`). Generic “play a song” picks a random **local** file (no iTunes/YouTube). Named artist/title scores local first. After a short TTS announce, stream the **full** track as 24 kHz Opus on the TTS WebSocket (FFmpeg). Do not hum or invent lyrics. |
| `search_web` / knowledge | `query` | Web or RAG search; cite briefly in speech. |
| `send_email` | `to`, `subject`, `body` | Optional; original cloud MCP extension. |
| smart-home / PC control | vendor-specific | Original cloud MCP; out of scope unless you add cloud MCP servers. |

**LLM policy for Mickey:**

1. If the user asks about device state (volume, battery, Wi-Fi) → device `self.get_device_status`.
2. If the user asks to move/dance/stop → device `self.otto.*`.
3. If the user asks weather/news/music/facts → **server** tools, then TTS.
4. Never block audio: run device MCP in parallel with LLM, but stop motion before long TTS if servos would fight.

---

## 7. Audio / ASR / TTS requirements

| Item | Value |
|------|--------|
| Uplink | Opus, 16 kHz, mono, 60 ms |
| Downlink | Opus, typically 24 kHz, mono, 60 ms |
| STT | Decode uplink Opus; this backend endpoints with server Silero VAD (device may not send `listen/stop`) then Gemini Live transcription |
| TTS | After LLM text, send `tts/start`, binary Opus, `sentence_start` per sentence, `tts/stop` |
| Barge-in | On new `listen/detect` or abort, stop TTS |
| AEC | If `features.aec` is true, prefer `listen/start` `mode: realtime` and binary protocol v2 timestamps |

If you cannot run real ASR/TTS yet, still send a valid TTS cycle (even a short silence Opus + “I heard you”) so the device leaves `listening`. Never leave the device in listening forever without `tts` or closing the socket.

---

## 8. Emotions / display

After each assistant turn send `type: llm` with `emotion` before or during TTS. Useful names: `neutral`, `happy`, `sad`, `sleepy`, `staticstate`. Otto idle default after UI setup is `staticstate`.

---

## 9. Optional on-device WebSocket (LAN debug)

Otto starts `WebSocketControlServer` on **port 8080**, path `/ws`. This is **not** the cloud protocol. A LAN client may send either:

```json
{ "type": "mcp", "payload": { "jsonrpc": "2.0", "method": "tools/call", "id": 1, "params": { "name": "self.otto.stop", "arguments": {} } } }
```

or a bare JSON-RPC object. MCP replies are broadcast to those clients. Do not confuse this with `ws://vps/xiaozhi/v1/`.

---

## 10. Authentication and multi-device

- Treat `Device-Id` (MAC) + `Client-Id` (UUID) + `Authorization` as the device identity.
- Issue a per-device `websocket.token` in OTA.
- Reject WS connections with a bad token (close handshake).
- `session_id` is per audio channel, not per device lifetime.

---

## 11. Error handling

| Case | Device behavior | Server should |
|------|-----------------|---------------|
| OTA non-200 / invalid JSON | Activation fails / retry | Always 200 + valid JSON |
| WS connect fail | Alert “cannot connect” | Keep `/xiaozhi/v1/` up |
| No hello in 10 s | Timeout error | Reply hello immediately |
| Malformed JSON | Log, ignore | Send valid `type` |
| MCP unknown tool | error message in JSON-RPC | Only call listed tools |
| 120 s silence | Channel timeout | Send keep-alive JSON or audio |

---

## 12. Implementation checklist (VPS Python agent)

1. HTTP `GET`+`POST` `/xiaozhi/ota` and `/xiaozhi/ota/` returning the JSON in §2.3 with this VPS WebSocket URL.
2. WebSocket `/xiaozhi/v1/` and `/xiaozhi/v1` accepting the headers in §3.1.
3. Hello handshake §3.2–3.3.
4. Parse listen/abort/mcp; decode Opus; server VAD; Gemini Live STT.
5. MCP initialize + tools/list + tools/call for **every** §5 tool you intend to use (at least otto action/stop + volume).
6. LLM with **server** tools weather/news/music/knowledge.
7. TTS cycle §3.6 + downlink Opus.
8. Emotion `llm` messages.
9. Optional MQTT/UDP §4.
10. TLS (`wss` / `https`) and a firewall allowing 443 or 8000.
11. JSON `ping` every 30 s during TTS/music; accept device `pong`; handle `notifications/phoe_lone.event` including `sleep`.
12. Production OTA: `websocket` only (no `mqtt`), `board.type` `mickey`, stable bearer, `firmware.version` `0.0.0` until a real image is published.

Reference client sources (do not copy cloud servers):

- `main/ota.cc`
- `main/protocols/websocket_protocol.cc`
- `main/protocols/mqtt_protocol.cc`
- `main/protocols/protocol.cc`
- `main/application.cc` (`OnIncomingJson`)
- `main/mcp_server.cc` / `main/mcp_server.h`
- `main/boards/mickey/otto_controller.cc`
- `docs/websocket.md`, `docs/mqtt-udp.md`, `docs/mcp-protocol.md`

---

## 13. What this backend must not do

- Do not require pin changes. OTA `board.type` is **`mickey`** (legacy `otto-robot` / `phoe-lone` still accepted on the VPS).
- Do not send `type: iot` (deprecated).
- Do not call `self.chassis.*` / `self.dog.*` / `self.electron.*` on Mickey; those belong to other boards.
- Do not call `self.set_press_to_talk` (not registered). Light MCP still returns `wired: false`.
- Do not invent IMU/touch numbers; live tools return `wired: true` with real samples (or `ok: false` on I2C fail).
- Do not put Python FastAPI sources in this ESP-IDF repository; deploy them on the VPS only.
