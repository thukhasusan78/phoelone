# Mickey Vision Plan — EMO-class local reflexes

**Status:** implementation brief. Do not start C++ until Slice 0 hardware is confirmed on the bench.  
**Date:** 2026-09-05  
**Board:** `mickey` (ESP32-S3 N16R8, no-camera, **no hands**, 4 servos).  
**Invariant:** voice, wake word, Opus, LVGL GIFs, MPU/touch, alarm/sleep, and Otto MCP must keep working. Vision is optional: if the coprocessor is missing or wedged, Mickey stays a voice robot.

This SKU **does not have hand servos**. Face, body, and voice carry personality. Vision events must never call removed actions (`hands_up`, `hand_wave`, `greeting`, `lh`/`rh`, `arm_swing`).

---

## 0. Why this architecture

The main S3 is already at its pin and CPU limit:

| Load | Why it cannot also own a camera |
|------|----------------------------------|
| Simplex I2S mic/amp | GPIO 4–7, 15, 16. Camera-SKU SCCB on 15/16 is speaker BCLK/LRCK here. |
| ST7789 SPI3 + backlight | GPIO 9–11, 46, 3. Camera XCLK fights LEDC with the backlight. |
| 4 servos | GPIO 17, 18, 38, 39 |
| MPU6050 + TTP223 | GPIO 40–42, 47 |
| AFE wake word + Opus + LVGL GIF | Continuous Core-0 / PSRAM work |

A DVP camera needs ~11 pins. Frozen map forbids remapping. Cloud-only Face ID fails the product rule already used for pet/fall: **idle life must work with the WebSocket closed**.

**Decision:** a dedicated vision coprocessor owns the camera and on-device models. The main S3 only receives tiny events and, on request, one JPEG.

```
  Vision coprocessor (camera + NPU)
  Face ID (5 slots), presence, gaze, wave, approach, leave
  JPEG on demand only
         | IRQ (us)              | UART events / JPEG
         v                       v
  Main ESP32-S3 (unchanged pin map)
  Audio, 4 servos, ST7789, MPU, Wi-Fi
  Local reaction table (no cloud)
         |
         | WebSocket / HTTP (slow is OK)
         v
  Companion / VPS
  VLM caption, photo store, memory
```

| Tier | Latency budget | Where | What |
|------|----------------|-------|------|
| Reflex | < 200 ms perceived | Coprocessor + IRQ + `MickeyBehavior` | Person entered, known face, gaze, wave, leave |
| Context | 200–800 ms | Coprocessor classifier | Closed-set scene class |
| Understanding | 1–3 s | Cloud VLM via existing `Camera::Explain()` | “What are they doing?” in language |
| Memory | async | Companion HTTP | Enroll names, store photos |

Zero-latency means **zero cloud hops on the reflex path**, not zero physics.

---

## 1. Hardware

### 1.1 Do not do

| Option | Why it fails |
|--------|----------------|
| OV2640 on the main S3 | No pins; kills audio/display/sensors; OTA identity collision with Otto camera SKU |
| Cloud-only Face ID / scene | 0.5–2 s+ RTT; dead when companion socket is closed |
| Raspberry Pi / Linux SBC | Boot time, power, heat for a desk robot |
| ESP-Hosted | Wrong problem (radio offload, not vision) |
| Remap frozen Mickey pins | Violates `AGENTS.md` / `FIRMWARE_NOW.md` |

### 1.2 Coprocessor (default)

**Second ESP32-S3 + camera** (XIAO ESP32-S3 Sense or ESP32-S3-CAM):

- Face ID of 5 people is the ESP-WHO enrollment demo.
- JPEG capture is native (`esp_camera`).
- Same ESP-IDF toolchain. **Disable Wi-Fi on the coprocessor** — it is not a second XiaoZhi.
- Closed-set scene (person, face-front, wave, approach, leave, phone-up) is a small FOMO / classifier.

**Himax WiseEye2 / Grove Vision AI V2** is the alternative if idle power is the top constraint. This repo already talks to Himax over SPI (`main/boards/sensecap-watcher/sscma_camera.cc`). Face-ID-of-5 and “save this JPEG” are weaker on that path, so it is not the default for this product.

ESP32-P4 is a later upgrade if on-device VLM is required. Not needed for EMO-like reflexes.

### 1.3 Main-board interconnect (4 free GPIOs)

Photos are not latency-critical. Events are ~16 bytes. Do **not** spend the pin budget on SPI.

| GPIO | Role | Notes |
|------|------|--------|
| **8** | `VISION_IRQ` | Input, pull-down, rising edge. Coprocessor asserts when an event is waiting. |
| **1** | UART1 RX | From coprocessor TX |
| **2** | UART1 TX | To coprocessor RX |
| **13** | `VISION_RST` | Output. Pulse to recover a wedged vision MCU. |

Leave **45 / 48** unused (future light / spare). Confirm 1, 2, 8, 13 on the bench before soldering.

UART: **921600 8N1**. A 50 KB JPEG is ~0.5–0.7 s. Presence/face events are sub-millisecond on the wire; IRQ means the main chip does not poll.

Power: shared 3.3 V + GND. Separate 5 V only if the camera module requires it. Soft-reset on GPIO 13 so vision cannot take down Mickey.

### 1.4 Board identity

Do not remap existing Mickey pins. Detect the coprocessor at boot (heartbeat). If OTA needs a distinct SKU, add `mickey-vision` rather than mutating `mickey`.

---

## 2. Protocol (event-first, binary)

JSON is fine on the coprocessor log. It is the wrong wire format for reflexes.

Frame: COBS or SLIP, little-endian, CRC16.

```
magic 0xA5 0x5A | type u8 | seq u16 | len u16 | payload | crc16
```

**Coprocessor → main (unsolicited):**

| Type | Name | Payload |
|------|------|---------|
| `0x01` | `PRESENCE` | `state` enter\|still\|leave, `dist` near\|mid\|far |
| `0x02` | `FACE` | `slot` 0–4 or `0xFF` unknown, `conf` 0–100 |
| `0x03` | `SCENE` | `class` (see §4), `conf` 0–100 |
| `0x04` | `HEARTBEAT` | uptime, temp |
| `0x10` | `JPEG` | seq, offset, total, bytes — **only after `CAPTURE`** |

**Main → coprocessor:**

| Type | Name | Payload |
|------|------|---------|
| `0x20` | `CAPTURE` | quality, reason: explain\|store |
| `0x21` | `ENROLL` | slot 0–4 |
| `0x22` | `FORGET` | slot |
| `0x23` | `MODE` | idle \| track \| enroll |

IRQ meaning: at least one event is waiting. Main reads until UART is empty, ACKs, coprocessor deasserts IRQ.

Debounce on the coprocessor: 150–300 ms. Emit **edges** (enter, recognize, wave start, leave), not every frame. Hold a face lock 1–2 s; require N consecutive misses before `leave`.

---

## 3. Software on the main S3

Board-local only. Do not put Mickey vision into core `Application`. Keep `camera.h` as capture + explain; streaming events live in Mickey.

| Piece | File (proposed) | Priority | Role |
|-------|-----------------|----------|------|
| `VisionLink` | `main/boards/mickey/mickey_vision.cc` | 3 (below audio 8) | UART + IRQ ISR → semaphore → parse one frame |
| `MickeyBehaviorOnVisionEvent` | `mickey_behavior.cc` | same director task | Local reflex table |
| `CoprocessorCamera` | `mickey_coprocessor_camera.cc` | — | Implements `Camera`; JPEG then HTTP |
| MCP enroll / store | `otto_controller.cc` or `mickey_vision.cc` | — | `self.mickey.vision.*` |

ISR on GPIO 8 only gives a semaphore. The VisionLink task reads UART. **No JPEG on the reflex path.**

If the main S3 is listening/speaking: still consume UART (do not block), but **gate body motion** the same way pet is dropped during speech. Always honor `leave`.

Heartbeat timeout → pulse `VISION_RST`. After 3 failed recovers, mark vision unwired and stop IRQ handling.

### 3.1 Local reflex table (no hands)

This robot has **no arms**. Reactions are face GIF + 4-servo body + optional later TTS.

| Event | Immediate reaction |
|-------|--------------------|
| `enter` + unknown | Curious / `surprised` face, micro-sway (`swing` height 8) |
| `FACE` slot 0–4 | Person-specific emotion (NVS name → clip). Optional small `swing` or `jump` — **never** `hand_wave` |
| `gaze` | Eye-contact GIF, pause idle fidget |
| `wave` (user waving) | Happy face + `swing` or reduced `walk` |
| `leave` | Soft `sad` / `staticstate`, return to idle director |
| `phone` | Slightly ignored / `embarrassed` look |

Slot → display-name lives in **main NVS** (`mickey_vision`). Embeddings live on the **coprocessor**.

### 3.2 Existing hooks to reuse

- `MickeyBehaviorOnImuEvent` / `MickeyBehaviorOnPetBegin` — same local-first pattern.
- `notifications/phoe_lone.event` — optional cloud notify (`face`, `presence`), max 2/s, only if audio channel open.
- `Board::GetCamera()` + `self.camera.take_photo` — auto-registers once `GetCamera()` is non-null (`mcp_server.cc`).
- `Camera::SetExplainUrl` / `Explain()` — VLM path already exists. Do not put appear/wave on it.

### 3.3 New MCP (after Slice 5)

| Tool | Purpose |
|------|---------|
| `self.mickey.vision.enroll` | `slot` 0–4, optional `name` |
| `self.mickey.vision.forget` | `slot` |
| `self.mickey.vision.list` | Slot table + last seen |
| `self.mickey.vision.who` | Last `FACE` event |
| `self.mickey.vision.capture_store` | JPEG → PSRAM → companion POST |
| `self.camera.take_photo` | Existing explain tool (VLM) |

Do **not** wear main flash with JPEGs. 8 MB octal PSRAM holds one frame.

---

## 4. Software on the coprocessor

Always-on camera owner. No voice stack. No Wi-Fi.

1. **Presence / motion gate** — empty desk → 1–2 FPS or motion-wake; person present → 10–15 FPS.
2. **Face ID** — 5 slots, embeddings in coprocessor NVS. Output `slot 0–4 | unknown` + confidence. Never send raw face images unless enroll/photo was requested.
3. **Scene head (closed set)** — `none`, `present`, `approach`, `leave`, `gaze`, `wave`, `phone`, `unknown`. Not open-world VLM.
4. **Event compressor** — edges only.
5. **JPEG only on `CAPTURE`**.

Open-world “they are packing a bag” stays on the companion VLM.

---

## 5. Photo store and Face ID

**Enroll (companion or voice):** main sends `ENROLL slot`. Coprocessor captures N frames, builds embedding, ACKs. Main stores `slot → name` in NVS. Recognition never needs the cloud.

**Photo:** user asks → `CAPTURE reason=store` → JPEG chunks over UART → PSRAM → HTTP POST to companion. Main does not keep the file.

**Explain:** same JPEG path, then existing `Explain(question)` multipart POST to `vision.url`.

Privacy: do not stream the camera. Upload only on explicit photo or explain.

---

## 6. What must not land on the main S3

- Frame buffers, face embeddings, MobileNet, JPEG encode
- A second Wi-Fi stack
- Blocking UART reads in `audio_input` or `otto_action`
- Polling the camera from the fidget director
- Extending `Camera` into a frame pump
- Any hand / arm MCP or servo keys

---

## 7. Build slices

Stop after each slice. Do not skip Slice 0.

### Slice 0 — Bench pins

Confirm GPIO 1, 2, 8, 13 are free on the assembled PCB. Do not invent GPIOs.

**Accept:** continuity + no LCD/audio/servo regression with those pins as inputs (pulled).

### Slice 1 — Heartbeat only

UART + IRQ + RST. Coprocessor sends `HEARTBEAT`. VisionLink logs it. No behavior change.

**Accept:** wake word, Opus, GIFs, 4-servo walk still clean. Missing module = “vision unwired”, no crash.

### Slice 2 — Presence reflex

`PRESENCE enter/leave` → `MickeyBehaviorOnVisionEvent`. Curious face + micro-sway on enter; idle director on leave.

**Accept:** person steps into view → face changes in < 200 ms with WebSocket **closed**.

### Slice 3 — Face ID (5 slots)

Enroll MCP + per-slot reaction table. Unknown vs known.

**Accept:** five enrolled people get distinct emotions; unknown gets the generic enter clip.

### Slice 4 — Scene classes

`gaze`, `wave`, `approach`, `phone`. Still local, still no VLM.

**Accept:** user waves → happy + body swing (not a hand action). User looks away at a phone → ignored look.

### Slice 5 — Camera + store

`CoprocessorCamera` so `self.camera.take_photo` and `self.mickey.vision.capture_store` work.

**Accept:** “take a photo” stores on the companion; “what am I holding?” returns VLM text. Audio does not glitch during the 0.5–1.5 s UART JPEG.

### Slice 6 — Optional cloud greet

`notifications/phoe_lone.event` `face` / `presence` so the companion can greet by name. Body/face already moved in Slice 2–3.

---

## 8. File map (when implementing)

| Action | Where |
|--------|--------|
| Pins | `main/boards/mickey/config.h` — add `MICKEY_VISION_*` only after Slice 0 |
| Link task | `main/boards/mickey/mickey_vision.cc/.h` (new) |
| Reflex | `main/boards/mickey/mickey_behavior.cc` |
| Camera impl | `main/boards/mickey/mickey_coprocessor_camera.cc` implementing `boards/common/camera.h` |
| `GetCamera()` | `main/boards/mickey/otto_robot.cc` — return coprocessor camera when heartbeat OK |
| MCP | Mickey board files only |
| Do not edit | Frozen servo/audio/display pins; core `Application` / `Protocol`; other boards |

New FreeRTOS work stays **below audio priority**. Schedule application mutations with `Application::Schedule()`.

---

## 9. Validation

| Slice | On-device | Still needed |
|-------|-----------|--------------|
| 0 | Bench photo of 1/2/8/13 | — |
| 1 | `idf.py` mickey build; wake-word soak 10 min | Physical UART dongle or coprocessor |
| 2–4 | Closed-WS presence test | Real faces, lighting |
| 5 | MCP photo + explain | Companion upload URL |
| — | Host tests unchanged (`python -m unittest discover -s scripts/tests -v`) | Full EMO feel needs hardware |

A successful build is not hardware validation.

---

## 10. Decision log

| Choice | Decision |
|--------|----------|
| Camera owner | Coprocessor, never main S3 |
| Interconnect | UART 921600 + IRQ + RST (4 pins) |
| Default module | Second ESP32-S3-CAM / XIAO Sense |
| Face ID | On-device, 5 slots |
| Scene | Closed-set local; VLM only on ask |
| Photos | Companion storage, PSRAM transit |
| Hands | None. Reflexes use face + legs/feet only |
| Cloud | Enhancement, not heartbeat |
