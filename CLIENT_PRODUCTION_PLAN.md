# Phoe Lone Client Production Plan (ESP32 firmware)

**Status:** planning document only. Do not change application C++ until this file is used as an implementation brief.  
**Date:** 2026-08-24  
**Upstream compared:** [78/xiaozhi-esp32](https://github.com/78/xiaozhi-esp32) `main` (2026-08). This tree is a XiaoZhi fork; core setup already exists. See [§12](#12-xiaozhi-first-boot-parity-vs-78xiaozhi-esp32).  
**Repo:** [thukhasusan78/phoelone](https://github.com/thukhasusan78/phoelone) — board profile `otto-robot` (no-camera), chip ESP32-S3 N16R8, ESP-IDF v6.0.2.  
**Companion:** server work lives in `BACKEND_PRODUCTION_PLAN.md` on the VPS. This file never assigns Python tasks.  
**Build:** `python scripts/build.py otto-robot --name otto-robot --language en-US`  
**Invariant:** voice capture, wake word, Opus uplink/downlink, Otto MCP motion, and display GIFs must keep working. New work runs on **separate FreeRTOS tasks** below audio priority.

The backend is a remote FastAPI service. Treat the JSON/MCP shapes in §4 as a frozen wire contract. If you need a server change, note it in a PR comment; do not implement it here.

---

## 0. How to use this file on the PC

| Section | Use when |
|---------|----------|
| [1. Firmware current state](#1-firmware-current-state) | Orienting a new session |
| [2. GPIO and wiring](#2-gpio-and-wiring) | Soldering or editing `config.h` |
| [3. Sensor firmware architecture](#3-sensor-firmware-architecture) | MPU6050 / light / touch C++ |
| [4. Wire contracts (client view)](#4-wire-contracts-client-view) | JSON you send/receive |
| [5. Ping/pong client handler](#5-pingpong-client-handler) | `application.cc` keepalive |
| [6. Servo safety](#6-servo-safety) | Stop / hold patches |
| [7. Idle director](#7-idle-director) | EMO presence without cloud |
| [8. Client OTA](#8-client-ota) | `ota.cc` / board identity |
| [9. P0 / P1 / P2 checklists](#9-p0--p1--p2-checklists-c-codebase) | Tick boxes in firmware PRs |
| [10. File map](#10-file-map) | Where to edit |
| [11. Decision log](#11-decision-log-firmware) | Frozen product choices |
| [12. XiaoZhi first-boot parity](#12-xiaozhi-first-boot-parity-vs-78xiaozhi-esp32) | Wi-Fi pairing, activation code, ESP-SR, NVS/OTA recovery vs stock |

---

## 1. Firmware current state

### 1.1 What already works on device

- Wi-Fi provision (Soft-AP default; BluFi optional in Kconfig, not enabled on otto-robot), reconnect, close audio channel on disconnect. First-boot AP SSID prefix is still `Xiaozhi`. Boot GPIO 0 click during `kDeviceStateStarting` enters config; there is **no** idle long-press re-pair on this board.
- Boot OTA POST to `CONFIG_OTA_URL`, parse `websocket.url` + token into NVS.
- WebSocket hello, raw Opus v1, listen/start/detect/abort, MCP as JSON-RPC inside `type: mcp`.
- Device MCP: `self.get_device_status`, volume, brightness, theme, `self.otto.*`, `self.battery.get_level`, `self.otto.get_ip`.
- GIF emotions via `Display::SetEmotion`; idle default `staticstate` after UI setup.
- Wake word (ESP-SR AFE + `wn9_nihaoxiaozhi_tts`); button GPIO 0 toggles chat; click during starting enters Wi-Fi config.
- Stock activation UI exists (`ShowActivationCode` + digit OGGs) but **does not run** unless OTA JSON includes `activation`. Lab VPS omits it.
- Speaking state **drops** downlink-conflict: mic streaming stops unless realtime AEC mode. Simplex I2S on this SKU (`audio_use_simplex = true`).
- Full OTA **download/flash/reboot** machinery exists (`UpgradeFirmware`, user-only `self.upgrade_firmware`). Lab server currently advertises dummy `0.0.0` so upgrade is skipped.
- LAN debug MCP server on port **8080** `/ws` — not the cloud protocol.

### 1.2 What is stubbed or unsafe

- MPU6050 / light / touch: MCP tools return `wired: false` and never touch hardware. **No pins in `config.h`.**
- `NON_CAMERA_VERSION_CONFIG`: `i2c_sda_pin` / `i2c_scl_pin` = `GPIO_NUM_NC`.
- Camera-variant I2C (GPIO 15/16) is **speaker BCLK/LRCK** on this robot — never reuse.
- Hands: `left_hand_pin = 8`, `right_hand_pin = 12`. `has_hands_` is true because neither is `NC`. GPIO **12 is LCD CS**. Hand actions can PWM the display.
- `self.otto.stop` on GitHub still `vTaskDelete`s the action task → PWM off → servo sag. Hold patches are in the backend repo `firmware/otto-robot/patches/` and must be applied **in this firmware tree**.
- `OnIncomingJson` has no `ping` case → serial `Unknown message type: ping` every ~30 s. The text frame still resets the **120 s** last-incoming timer (`docs/websocket.md` / `backend_spec.md`).
- No idle fidget task. XiaoZhi is session-based: wake → `OpenAudioChannel` → talk → close → statue.
- No low-battery motion inhibit (ADC + charge GPIO exist; policy does not).
- Board type is still `otto-robot`, not `phoe-lone`.
- No idle long-press re-pair; `SystemReset` is unused (same as stock otto). Wrong Wi-Fi password requires a 60 s timeout or NVS erase.

### 1.3 EMO constraint (firmware-owned)

Idle life **must run with the WebSocket closed**. Cloud is optional for talking about a pet; flinch, blink, and freeze-on-pickup are local.

---

## 2. GPIO and wiring

### 2.1 Frozen pins (GOAL.md — do not remap)

| Function | GPIO |
|----------|------|
| Boot button | 0 |
| LCD backlight | 3 |
| Mic WS / SCK / DIN | 4 / 5 / 6 |
| Speaker DOUT / BCLK / LRCK | 7 / **15** / **16** |
| LCD MOSI / CLK / DC / RST / CS | 10 / 9 / 46 / 11 / **12** |
| Left leg / left foot | 17 / 18 |
| Right foot / right leg | 38 / 39 |
| Charge detect | 21 |

Battery: `ADC_UNIT_2` + `ADC_CHANNEL_3` ⇒ **GPIO 14** on ESP32-S3. Do not steal it for light.

### 2.2 Occupied / forbidden for new sensors

Reject these for MPU / light / touch:

`0`, `3–7`, `9–12`, `14–18`, `19–21` (USB 19/20, charge 21), `26–39` (octal flash/PSRAM 26–37 plus servos 38/39), `43`, `44` (UART0 monitor on many S3 modules), `46`.

### 2.3 Hand-pin P0 policy

Set `left_hand_pin` and `right_hand_pin` to `GPIO_NUM_NC` **or** `#define OTTO_HAS_HANDS 0` so `has_hands_ == false`.

- GPIO 12 stays **display CS only**. Never attach an LEDC servo channel to it.
- GPIO 8 becomes free **after** hands are NC. Do not assign 8 to a sensor until that change is flashed and verified.
- Hand MCP actions must keep returning the existing error string (`错误：此动作需要手部舵机支持` or the English equivalent if you localize later).

### 2.4 Proposed sensor pins (confirm on the bench)

Free on the no-camera map, I2C-capable, not octal/USB/UART0:

| Sensor | Role | Proposed GPIO | Notes |
|--------|------|---------------|--------|
| MPU6050 | SDA | **41** | Camera-board I2S pins; unused here |
| MPU6050 | SCL | **42** | Same |
| MPU6050 | INT | **40** | Recommended: motion/fall without 100 Hz polling |
| Light | I2C BH1750 / VEML7700 | **same 41/42** | Preferred. ADC2 + Wi-Fi is hostile; battery already on ADC2 |
| Light fallback | ADC1 LDR | **1** (ADC1_CH0) | Only if no I2C light part |
| Touch | Digital TTP223 | **47** | 3.3 V, ISR |
| Touch fallback | Capacitive TOUCH2 | **2** | Only if no TTP223 |

If the modules are **already soldered** to other **free** pins (13, 45, 48, 1, 2), put **those** numbers in `config.h`. The rule is: named, documented, not in §2.2.

### 2.5 Electrical

- MPU6050 at **3.3 V** (not 5 V). Shared GND. 4.7 kΩ SDA/SCL pull-ups to 3.3 V if the module lacks them.
- Do not power servos + sensors from the ESP32 module 3.3 V pin if current is tight; use the board 3.3 V rail. Servos stay on external 5 V, shared GND.
- Analog light divider 0–3.3 V only.
- Touch digital must be 3.3 V logic; level-shift 5 V modules.
- INT pin: idle high or per module datasheet; use `GPIO_INTR_NEGEDGE` if the MPU pulses low on motion.

### 2.6 Pin checklist before writing drivers

1. Photograph SDA, SCL, INT, light, touch nets.
2. Diff against §2.1–2.2.
3. Commit `#define`s in `config.h` with comments (`PHOE_LONE_IMU_SDA`, etc.).
4. First boot with **servo 5 V unpowered** until I2C WHO_AM_I succeeds.

---

## 3. Sensor firmware architecture

### 3.1 Today (stubs)

**File:** `main/boards/otto-robot/otto_controller.cc` → `RegisterMcpTools()`.

| Tool | Current body |
|------|----------------|
| `self.phoe_lone.imu.get_reading` | Immediate JSON `wired:false`, reason I2C NC |
| `self.phoe_lone.light.get_level` | Immediate JSON `wired:false` |
| `self.phoe_lone.touch.get_state` | Immediate JSON `wired:false` |

No driver, no task, no ISR, no NVS thresholds.

### 3.2 Target task graph

```
SensorTask  (priority < audio encoder, stack ~4–6 KB)
  loop 20–50 ms (or block on INT + 20 Hz poll fallback)
    read MPU (I2C, mutex)
    read light (I2C same mutex, or ADC1)
    sample touch (GPIO level; ISR only sets a flag)
    complementary filter / debounce
    classify: still | moving | pickup | putdown | fall | shake
    classify light bucket; touch edge
    --- local reactions (never wait on WS) ---
    if fall: OttoStopAndHome() immediately
    if pet / pickup: SetEmotion + optional PlaySound; pause idle director
    --- optional notify if audio channel open ---
    coalesce max 2 events/s; skip pet during kDeviceStateSpeaking
    Application::SendMcpMessage(notification JSON-RPC, no id)

MCP tool callbacks (application task)
  copy last sample from SensorTask via mutex / atomic snapshot
  return JSON string (wired:true or ok:false)
```

**Hard rules**

- Never call `i2c_master_transmit` from the WebSocket or audio callback.
- I2C bus mutex shared by MPU + light.
- Fall: **stop servos before any `SendMcpMessage`**.
- If WS is closed, personality still works. Notifications are best-effort.
- Watchdog: SensorTask must `vTaskDelay` or block on a notification; no tight spin.

### 3.3 Suggested new files (firmware tree)

| File | Responsibility |
|------|----------------|
| `main/boards/otto-robot/phoe_lone_sensors.h` | Pin macros, snapshot struct, `Start()`, `GetSnapshot()` |
| `main/boards/otto-robot/phoe_lone_sensors.cc` | I2C init, MPU WHO_AM_I `0x68`/`0x69`, DMP-less raw accel/gyro, INT ISR, light, touch |
| `main/boards/otto-robot/phoe_lone_behavior.h/.cc` | Idle director (P1) |
| `config.h` | Pin `#define`s + `OTTO_HAS_HANDS 0` |

Keep MCP registration in `otto_controller.cc` (or a small `RegisterPhoeLoneSensorTools()` called from there) so tool names stay `self.phoe_lone.*`.

### 3.4 MPU6050 bring-up sequence

1. `i2c_new_master_bus` on 41/42 (or confirmed pins), 100 kHz first, internal pull-ups as needed.
2. Probe `0x68` then `0x69`. Log WHO_AM_I. Fail → tools stay `wired:true`, `ok:false`, `error:i2c_nack`.
3. Wake: clear sleep bit in `PWR_MGMT_1`. Gyro ±250 dps, accel ±2 g for desk use.
4. Optional: enable motion interrupt; GPIO 40 `gpio_isr_handler` sets `xTaskNotify`.
5. Convert raw to g and deg/s. Pitch/roll from accel at rest; do not claim yaw without mag.
6. Classify:
   - `fall`: `|az|` far from 1 g **and** large gyro, or tilt > ~55° for > 150 ms (tune in NVS).
   - `pickup`: gravity vector rotates / `|a|` leaves 1 g band, not a fall.
   - `putdown`: return to 1 g stable 300 ms.
   - `shake`: high gyro energy, short.
7. Store last snapshot. MCP `get_reading` memcpy snapshot.

### 3.5 Light

Preferred: BH1750 (addr `0x23`/`0x5C`) or VEML7700 on the same I2C bus.

Buckets (tune with a flashlight on the desk):

| bucket | meaning |
|--------|---------|
| `dark` | night / covered |
| `dim` | evening indoor |
| `indoor` | normal room |
| `bright` | window / lamp close |

Analog fallback: ADC1 GPIO 1, oversample 8, map to buckets only (`lux` omitted).

P1: `dark` for N minutes → `SetEmotion("sleepy")` + reduce backlight; `bright` edge → wake face. Preempted by chat states.

### 3.6 Touch

TTP223 (or similar) on GPIO 47:

- `gpio_config` input, pull as module requires.
- ISR → set `touched_edge`; SensorTask debounces **30–50 ms**.
- Snapshot: `touched`, `count`, `ms_held`.
- Local pet: emotion `loving` or `happy`, optional short OGG, **do not** start a cloud turn.

Capacitive fallback: ESP32-S3 `touch_pad` on GPIO 2; calibrate baseline at boot; more noise — prefer TTP223.

### 3.7 MCP pull JSON (firmware must emit)

Keep `wired` forever so older backends do not break.

**IMU** — success:

```json
{
  "wired": true,
  "sensor": "MPU6050",
  "ax": 0.02, "ay": 0.01, "az": 1.00,
  "gx": 0.1, "gy": -0.2, "gz": 0.0,
  "pitch": 2.4, "roll": -1.1,
  "temp_c": 31.2,
  "event": "still"
}
```

`event`: `still` | `moving` | `pickup` | `putdown` | `fall` | `shake`.  
I2C fail: `{"wired":true,"ok":false,"error":"i2c_nack"}`.

**Light:**

```json
{ "wired": true, "lux": 120, "bucket": "indoor", "raw": 1840 }
```

**Touch:**

```json
{ "wired": true, "touched": true, "count": 14, "ms_held": 320 }
```

Until pins work, **keep the old `wired:false` strings** so a half-flashed board does not lie.

### 3.8 MCP notification (device → server, no `id`)

Only if `protocol_->IsAudioChannelOpened()`. Use existing `Application::SendMcpMessage`.

Envelope:

```json
{
  "session_id": "<from hello>",
  "type": "mcp",
  "payload": {
    "jsonrpc": "2.0",
    "method": "notifications/phoe_lone.event",
    "params": {
      "event": "pet",
      "ts_ms": 1710000000000,
      "imu": { "pitch": 8.0, "az": 0.2 },
      "light": { "bucket": "indoor" }
    }
  }
}
```

`event`: `pickup` | `putdown` | `fall` | `pet` | `bright` | `dark`.

- **Do not** expect a JSON-RPC result (no `id`).
- Coalesce: max **2/s**.
- During `kDeviceStateSpeaking` / music: send **`fall` only**; drop `pet`.
- Always run local fall stop first.

The VPS currently **drops** `notifications/*`. Local behavior must not wait for the server. After the backend lands its handler, the same firmware notify path starts working with no second firmware change if the JSON matches.

---

## 4. Wire contracts (client view)

Do not add `type: iot`. Do not require MQTT. Voice path stays WebSocket.

### 4.1 Types you already handle

`hello` (incoming must have `transport: websocket`), `tts`, `stt`, `llm`, `mcp`, `system`/`reboot`, `alert`, optional `custom`.

Device → server: `hello`, `listen`, `abort`, `mcp` replies.

### 4.2 Types you must add

| Incoming | Action |
|----------|--------|
| `type: ping` | No UI. Optional send `type: pong` echoing `ts_ms` and `session_id`. `ESP_LOGD` only. |
| `llm` emotion while idle director running | Yield fidget for ~30 s, then resume (P1) |

| Outgoing | Action |
|----------|--------|
| `type: pong` | Reply to ping |
| `mcp` notification | §3.8 |

### 4.3 Sensor events vs device state (firmware column)

| Situation | Firmware must |
|-----------|----------------|
| WS closed, pet | Face + optional motion + optional OGG |
| WS open, listening, pet | Same + notify |
| WS open, speaking/music, pet | Local only; no pet notify |
| Fall | Stop+home immediately; then notify if WS open |
| Dark for N minutes | Sleepy GIF, dim (P1) |
| Pickup | Pause idle director; held face |

### 4.4 Incoming `alert` / `llm`

You already handle these. Low-battery **policy** is local (P0.4). Server may also send `alert`; treat it as overlay, do not double-play `low_battery.ogg` if you just played it.

---

## 5. Ping/pong client handler

### 5.1 Why serial says unknown type

Server sends every 30 s:

```json
{ "session_id": "<uuid>", "type": "ping" }
```

`Application::OnIncomingJson` has no `ping` branch → `ESP_LOGW("Unknown message type: ping")`.

### 5.2 Why the channel still lives

Idle timeout is **120 s since last incoming frame**. Unknown JSON is still a text frame. Ping **already** prevents drop. You add a handler to silence WARN and to prove the **application** task is alive via `pong`.

### 5.3 Do not

- Handle ping by playing sound, changing emotion, or entering speaking.
- Treat ping as binary Opus.
- Rely on TCP keepalive alone.

### 5.4 Required C++ behavior (`main/application.cc`)

In `OnIncomingJson`, **before** the unknown-type log:

1. If `type == "ping"`:
   - Read `session_id` if present (ignore mismatch; still pong).
   - Read optional `ts_ms`.
   - `Schedule` a send of:

```json
{ "session_id": "<same as hello>", "type": "pong", "ts_ms": <echo or device now> }
```

   - Use `protocol_->SendText` / the same path as other JSON (not MCP).
2. Return. No `SetDeviceState`. No `SetEmotion`.

Optional later: if opcode-0x9 ping does **not** reset XiaoZhi’s 120 s timer, JSON ping remains mandatory. Verify in `websocket_protocol.cc` how `last_incoming` is updated.

### 5.5 ESP-IDF transport ping (verify, do not assume)

[esp_websocket_client](https://docs.espressif.com/projects/esp-protocols/esp_websocket_client/docs/latest/index.html):

- Client may send protocol PING (`ping_interval_sec`, default 10 s) and abort if no PONG (`pingpong_timeout_sec`).
- Incoming opcode PING is answered with PONG in the stack (`WEBSOCKET_EVENT_DATA` also fires for pong).
- **Check phoelone `websocket_protocol.cc`:** whether opcode ping/pong updates the **application** 120 s clock. If only TEXT/BINARY do, keep JSON `ping`.

Do not disable the IDF ping-pong disconnect unless you have measured false disconnects (known issue when send timeouts starve the client task). Prefer fixing send timeouts over `disable_pingpong_discon`.

### 5.6 Music and long TTS

Stay in speaking for minutes (local music). JSON `ping` must still be processed on the protocol task so 120 s does not fire. Do not block `OnIncomingJson` on servo or I2C.

---

## 6. Servo safety

### 6.1 Patches to apply in **this** tree

Copy from VPS `phoe_lone_server/firmware/otto-robot/patches/` (or keep a local copy):

| Patch | Files | Effect |
|-------|-------|--------|
| `001-oscillator-keep-hold.patch` | `oscillator.cc` | Re-`Attach` must not `ledc_stop`; refresh `Write(pos_)` |
| `002-home-force-stance.patch` | `otto_movements.cc` | `Home()` always reapplies 90° even if resting |
| `003-stop-cooperative-home.patch` | `otto_controller.cc` | Stop = flag + drain queue + `ACTION_HOME`; **no** `vTaskDelete` |

GitHub `self.otto.stop` today deletes the task, then a new `ActionTask` calls `Attach()` which released PWM. That is the sag bug.

### 6.2 After patch

- Action loops abort early when `stop_requested_`.
- Queue reset + home.
- `PowerManager::ResumeBatteryUpdate()` still runs after stop.
- Trims in NVS unchanged.

### 6.3 Low battery (P0.4)

`PowerManager` already polls ADC 1 Hz and charge GPIO 21.

Add: if level &lt; threshold (start ~15%, NVS-tunable) and not charging:

- Reject `QueueAction` except home/stop.
- Play `Lang::Sounds` low battery OGG once per N minutes.
- Dim backlight; `SetEmotion("sleepy")`.
- Do not walk/jump/showcase.

Fall (IMU) uses the same inhibit path as an emergency stop.

### 6.4 Motion vs SensorTask

- Pause battery ADC during motion (already).
- Pause **idle director** during queued Otto actions.
- SensorTask keeps running (fall must work mid-dance).

---

## 7. Idle director

Highest EMO ROI. **No cloud.** New module `phoe_lone_behavior.cc`.

### 7.1 When it runs

- `kDeviceStateIdle` (typical: WS closed).
- Optional: listening with no speech — **keep fidget off** while mic is hot to avoid servo noise in VAD. Prefer idle-only for v1.

### 7.2 Loop

Every **8–20 s** (random):

- Blink / swap GIF: `staticstate`, `winking`, `sleepy` (weights).
- Or 1–2° look: `servo_sequences` equivalent **internal** call, duration &lt; 800 ms, then home. Do not use LLM tools.
- Bound speed and queue depth **1**.

### 7.3 Preempt immediately

Wake word, GPIO 0, touch pet, pickup, fall, `OpenAudioChannel`, any `self.otto.action` from MCP.

On preempt: `stop_requested` on fidget only (not a full robot panic unless fall).

### 7.4 Yield to cloud emotion

If `type: llm` arrives while a session is open, show that GIF for **30 s**, then resume the cycle.

### 7.5 Pickup / light (P1, after sensors)

- Pickup: freeze fidget; `SetEmotion` “surprised” or similar.
- Putdown: resume after 1 s stable.
- Dark bucket: sleepy + dim; do not fidget large motions in the dark.

### 7.6 Music dance (P1 optional)

If speaking **and** firmware can detect “music mode” (it cannot, unless you infer long TTS without `sentence_start` text — **do not infer**). Safer: only dance when MCP `self.otto.action` is called, or add a later `notifications` from server. Client v1: skip auto-dance unless the server sends a dedicated MCP action (backend P1). Firmware may expose a short `dance_idle` sequence the LLM already has (`swing`).

---

## 8. Client OTA

### 8.1 Today

1. `Ota::CheckVersion()` POST (or GET) to `CONFIG_OTA_URL` (today hardcoded in `otto-robot/config.json` to the VPS).
2. Headers: `Device-Id`, `Client-Id`, `Activation-Version`, `Accept-Language`, etc.
3. Body: `Board::GetSystemInfoJson()` including `board.type` = `otto-robot`.
4. Parse `websocket.*` into NVS, `server_time`, optional `firmware.version` + `url`.
5. If version **newer** (or `force: 1`), `UpgradeFirmware(url)`: progress UI, write partition, reboot.
6. `MarkCurrentVersionValid()` after good boot (IDF rollback).
7. User-only MCP `self.upgrade_firmware` with `url`.

Lab backend returns `0.0.0` + `/firmware/none.bin` **404** → skip. That is correct until a real image exists.

### 8.2 Firmware tasks (not server)

| ID | Task |
|----|------|
| C-OTA.1 | Keep skip-upgrade when version is `0.0.0` or download 404s (already). |
| C-OTA.2 | Never `force` from device. |
| C-OTA.3 | Before **production** binaries: copy board to `main/boards/phoe-lone/`, unique `BOARD_TYPE`, `config.json` `"type": "phoe-lone"`. OTA POST `board.type` must match the VPS channel. |
| C-OTA.4 | HTTPS OTA URL in production `sdkconfig` / NVS `wifi.ota_url`. |
| C-OTA.5 | Confirm dual-bank partition (`partitions/v2/16m.csv`); test rollback by crashing once after a staging flash. |
| C-OTA.6 | During upgrade: `SetPowerSaveLevel(PERFORMANCE)`, stop audio, no servo motion. |
| C-OTA.7 | Do not erase NVS websocket token mid-upgrade except as stock XiaoZhi already does. Token rotate is a **server** bug; client just writes whatever OTA JSON contains. |

### 8.3 Activation (keep stock firmware path)

The client **already** implements stock XiaoZhi activation (`ShowActivationCode`, digit OGGs, poll `POST …/activate`). Do **not** delete it.

| Mode | Server OTA JSON | Device UX |
|------|-----------------|-----------|
| Lab / open VPS | Omit `activation` | Skip UI; go Idle after version check (today) |
| Production binding | Include `activation.code` + `message` (and `challenge` if Activation-Version 2) | Status `Activation`, emotion `link`, play `activation.ogg` then digit OGGs, poll until 200 |

Hardware-test the code path even if lab stays open: temporarily return a 6-digit `activation.code` and confirm LCD + TTS + `/activate` loop. See §12.2.

---

## 9. P0 / P1 / P2 checklists (C++ codebase)

Tick these in firmware PRs. Server checkboxes live in `BACKEND_PRODUCTION_PLAN.md`.

### P0 — safety, keepalive, sensors local

- [ ] **P0.1** Apply oscillator / home / cooperative-stop patches. `self.otto.stop` does not `vTaskDelete`. Pose holds 30 s with 5 V servos.
- [ ] **P0.2** Hands NC / `OTTO_HAS_HANDS=0`. GPIO 12 never LEDC. Hand tools error.
- [ ] **P0.3** Plan `main/boards/phoe-lone/` (can land after sensors; **must** land before a real OTA `.bin`).
- [ ] **P0.4** Low battery: no walk/jump; OGG; dim; home.
- [ ] **P0.6** `ping` handler; `pong` JSON; no WARN log.
- [ ] **P0.7** Measure: opcode ping vs 120 s timer; document result in `docs/websocket.md`.
- [ ] **P0.8** Wake-word abort during TTS **and** during a long music stream (AFE wake word enabled in speaking).
- [ ] **P0.S1** Real pins in `config.h` (not NC) matching solder.
- [ ] **P0.S2** MPU WHO_AM_I in serial; MCP `wired:true` with live ax/ay/az.
- [ ] **P0.S3** Touch ISR + 30–50 ms debounce; pet GIF with **WS closed** &lt; 200 ms.
- [ ] **P0.S4** Light buckets change with flashlight; MCP JSON.
- [ ] **P0.S7** Fall/tip: servos stop &lt; 200 ms **before** any notify.
- [ ] Notify path implemented (even if VPS ignores it until their P0.S5).
- [ ] Audio/wake-word watchdog-clean during I2C.
- [ ] Unplug IMU → `ok:false`, not fake 1 g forever without `ok`.

### P1 — EMO presence

- [ ] **P1.1** Idle director 8–20 s fidget in `kDeviceStateIdle`.
- [ ] **P1.2** Map incoming `llm` emotion to a **short** local motion (cap duration; never block Opus). Optional table in behavior module.
- [ ] **P1.3** Idle GIF cycle `staticstate` / `sleepy` / `winking`.
- [ ] **P1.4** Pickup freezes fidget; putdown resumes.
- [ ] **P1.5** Dark → sleepy + dim; bright → wake face.
- [ ] **P1.6** Do not break abort-during-music. Optional: only dance if MCP action requested.

### P2 — product SKU

- [ ] **P2.1** Local clock + sleepy night pose (server_time already applied at OTA).
- [ ] **P2.3** `phoe-lone` board type in OTA JSON; HTTPS; rollback tested.
- [ ] **P2.4** If enabling `CONFIG_USE_SERVER_AEC`: hello `features.aec`, protocol v2 timestamps, `listen mode: realtime`. Simplex I2S will limit quality.
- [ ] **P2.6** Optional `esp_coredump` UART or HTTP post (needs server URL).
- [ ] **P2.7** Glyph-push consume path already in upstream; ensure Myanmar glyphs if you show STT on LCD.
- [ ] **P2.8** Camera / ToF only if hardware exists; this SKU is no-camera.
- [ ] **P2.9** Factory: servo sweep, mic loopback, WHO_AM_I, ADC print.
- [ ] **P2.UX1** First-boot SoftAP: LCD shows SSID + config URL; `wificonfig.ogg` plays; 60 s STA timeout falls back to AP.
- [ ] **P2.UX2** Idle long-press (or triple-click) GPIO 0 re-enters Wi-Fi config without waiting for a failed reconnect.
- [ ] **P2.UX3** Activation code LCD + digit TTS when OTA JSON includes `activation.code`; skip when omitted.
- [ ] **P2.UX4** Boot-held GPIO 0 (or documented gesture) erases NVS Wi-Fi / tokens so a brick can re-pair. `SystemReset` exists but is unused.
- [ ] **P2.UX5** Confirm `wn9_nihaoxiaozhi_tts` (or product wake word) is packed in assets; barge-in during speaking still works on simplex I2S.
- [ ] **P2.UX6** After a good boot, `esp_ota_mark_app_valid_cancel_rollback` ran (already in `Ota::MarkCurrentVersionValid`). Crash-loop once on a staging image to prove rollback.

Out of firmware scope: 4G, MQTT voice, LivingAI assets, Python. SmartConfig is also out of scope — stock XiaoZhi does not implement it.

### Acceptance (device-only)

- Serial: WHO_AM_I, touch edges, light buckets.
- Pet works WS closed.
- Stop holds pose; GPIO 12 untouched by servos.
- No `Unknown message type: ping`.
- Channel survives 3+ minutes listening silence **and** 4+ minutes music (depends on server still sending ping; if music dies, capture whether **device** 120 s fired).
- Fidget visible; wake word cancels it.

---

## 10. File map

| Path | Change |
|------|--------|
| `main/boards/otto-robot/config.h` | Hands NC; sensor pins |
| `main/boards/otto-robot/oscillator.cc` | Patch 001 |
| `main/boards/otto-robot/otto_movements.cc` | Patch 002 |
| `main/boards/otto-robot/otto_controller.cc` | Patch 003; MCP live sensors |
| `main/boards/otto-robot/phoe_lone_sensors.*` | **New** |
| `main/boards/otto-robot/phoe_lone_behavior.*` | **New** (P1) |
| `main/application.cc` | `ping` / `pong` |
| `main/protocols/websocket_protocol.cc` | Verify last-incoming vs opcode ping |
| `docs/websocket.md` | Document `ping`/`pong` |
| `main/boards/phoe-lone/` | P0.3 / P2.3 |
| `otto-robot/config.json` | `CONFIG_OTA_URL` HTTPS; later `type: phoe-lone` |
| `main/boards/otto-robot/otto_robot.cc` | P2.UX2 / P2.UX4: idle re-pair + NVS erase gesture (stock otto has neither) |
| `main/boards/common/wifi_board.cc` | Optional: AP SSID prefix `PhoeLone`; already has SoftAP + BluFi |
| `main/boards/common/system_reset.cc` | Wire or replace; currently unused in this tree **and** in stock otto |

### First firmware slices (order)

1. Hands NC + stop patches + ping/pong.  
2. `config.h` pins + MPU WHO_AM_I + MCP IMU.  
3. Touch + light + local pet/fall + notify emit.  
4. Idle director.  
5. `phoe-lone` board + OTA identity.  
6. First-boot UX: idle re-pair, NVS wipe gesture, activation-code hardware test (§12).

Stop after each slice and run the matching P0/P1 boxes.

---

## 11. Decision log (firmware)

| Decision | Choice |
|----------|--------|
| Idle life | On-device task, WS optional |
| Light | I2C on MPU bus preferred |
| Hands | Disabled; GPIO 12 is CS |
| Keepalive | Handle JSON `ping`; verify IDF opcode ping vs 120 s |
| Sensors | Own FreeRTOS task; fall is local |
| Camera | Not this SKU |
| Wi-Fi pairing | Stock SoftAP (not SmartConfig). BluFi optional, off on otto. |
| Activation UI | Keep firmware path; lab may omit JSON; production may emit `activation.code` |
| Factory reset | Must add a gesture; stock otto also leaves `SystemReset` unused |

---

## 12. XiaoZhi first-boot parity vs [78/xiaozhi-esp32](https://github.com/78/xiaozhi-esp32)

Compared this tree (phoelone / otto-robot, no-camera) to upstream `main` in 2026-08. **Do not rewrite the audio/protocol core.** Most pairing machinery is already here. The product gap is otto-board UX, lab server omitting `activation`, and recovery gestures that stock otto also never wired.

### 12.1 Wi-Fi provisioning

Stock XiaoZhi does **not** implement SmartConfig / ESP-TOUCH. Pairing is:

| Method | Upstream | This tree (otto-robot) |
|--------|----------|-------------------------|
| SoftAP hotspot | Default (`CONFIG_USE_HOTSPOT_WIFI_PROVISIONING=y`) | Same. AP SSID `Xiaozhi-<MAC4><MAC5>`, captive portal URL spoken/shown |
| BluFi | Optional Kconfig; mutually exclusive with hotspot | Same Kconfig. **Not** in otto `sdkconfig_append` |
| SmartConfig | Absent | Absent — do not port |

**Stock workflow** (`wifi_board.cc`, identical except upstream also allows enter-config from `kDeviceStateNotifying`):

1. Boot → `nvs_flash_init` → `StartNetwork`.
2. If NVS SSID list empty: wait 1.5 s (board/version on LCD), then SoftAP. If SSIDs exist: STA with **60 s** timeout, then SoftAP.
3. Display: `SCANNING_WIFI` / `CONNECT_TO <ssid>...` / `CONNECTED_TO <ssid>`.
4. Config mode: status `WIFI_CONFIG_MODE`, emotion `gear`, chat = hotspot SSID + browser URL, play `wificonfig.ogg`.
5. GPIO 0 **click while `kDeviceStateStarting`**: skip wait, enter config immediately (otto and stock otto both do this).
6. GPIO 0 **click while already configuring**: `ToggleChatState` → mic loopback (`kDeviceStateAudioTesting`). Same as stock.
7. After connect: BluFi deinit if used; `kDeviceStateActivating`.

**LED:** `SingleLed` blinks blue ~500 ms in config / ~100 ms at boot. Otto **does not** override `GetLed()` → `NoLed`. Stock otto is the same. This SKU’s pairing indicator is the **240×240 LCD**, not an RGB LED. Do not invent a WS2812 pin.

**What to port / change (firmware):**

| ID | Work | Why |
|----|------|-----|
| P2.UX1 | Hardware-test first-boot AP: LCD SSID+URL, `wificonfig.ogg`, 60 s STA fallback | Code exists; must not regress when adding idle director / sensors |
| P2.UX2 | Idle **long-press** (or triple-click) GPIO 0 → `EnterWifiConfigMode()` | Stock otto also lacks this. After first success, user cannot re-pair without a failed 60 s connect. Boards like `doit-s3-aibox` already do triple-click |
| Optional | AP prefix `PhoeLone` instead of `Xiaozhi` in `WifiBoard::StartNetwork` | Branding only; OTA board type is separate |
| Do not | Enable BluFi unless we ship EspBlufi docs | Hotspot is the stock default |
| Do not | Port SmartConfig | Upstream does not have it |
| Later | Upstream `kDeviceStateNotifying` + `NotifyPlayer` | Newer XiaoZhi so pairing can start during a prompt. Not required for v1 |

### 12.2 New-device activation

**Firmware already has the stock path.** Missing UX on the desk is almost always the **OTA JSON**, not missing C++.

Stock sequence after Wi-Fi:

1. `ActivationTask`: assets check → `Ota::CheckVersion()` POST to `CONFIG_OTA_URL` (headers `Device-Id`, `Client-Id`, `Activation-Version` 1 or 2).
2. If JSON has `activation.code`: `ShowActivationCode` — `Alert(..., "link", OGG_ACTIVATION)` then play `0.ogg`–`9.ogg` per digit. LCD shows server `message`.
3. Loop up to 10× `Ota::Activate()` → `POST {ota_url}/activate`. 202 = wait 3 s; other errors wait 10 s. Wake-word or button during activating → Idle (abort wait).
4. If no `activation` object: skip UI, mark OTA valid, init WebSocket/MQTT, Idle, play `success.ogg`.

Activation-Version **2** (eFuse serial): also `challenge` + HMAC-SHA256 body. Version **1** (typical modules): `{}` activate payload.

**Parity vs plan (old §8.3 was wrong):** keep `ShowActivationCode`. Lab VPS may omit `activation`. Production binding must emit `code` + `message`. Hardware-test with a fake 6-digit code.

Upstream extra (not in this tree): `kDeviceStateNotifying` so alerts are non-blocking. Port only if activation TTS collides with wake-word or pairing.

OGG files are git-tracked under `main/assets/locales/en-US/` (`activation.ogg`, `wificonfig.ogg`, digits). Build with `--language en-US` as today.

### 12.3 Wake-word engine (ESP-SR)

| Piece | Upstream / this tree | otto-robot SKU |
|-------|----------------------|----------------|
| Default on S3+PSRAM | `CONFIG_USE_AFE_WAKE_WORD` | Yes (not `USE_ESP_WAKE_WORD`; that is C3/C5/C6) |
| Default model | `CONFIG_SR_WN_WN9_NIHAOXIAOZHI_TTS=y` (`sdkconfig.defaults.esp32s3`) | Same. Phrase is 你好小智 / nihaoxiaozhi |
| Custom MultiNet | `USE_CUSTOM_WAKE_WORD` + `mn*` models | Off unless we choose it |
| Runtime “switch phrase” | No API to swap WakeNet names in RAM | Change model via **assets** download (`srmodels.bin`) or rebuild |
| Enable/disable | AFE `enable_wakenet` / `disable_wakenet` by state | Idle on; listening off unless `WAKE_WORD_DETECTION_IN_LISTENING`; **speaking keeps AFE wake word** (P0.8 already) |
| Device AEC | `aec_init` only if `codec->input_reference()` | Simplex `NoAudioCodec`: **no** reference channel → AEC never inits. Same as stock otto no-camera. `USE_DEVICE_AEC` Kconfig does not list otto-robot |
| Server AEC | `CONFIG_USE_SERVER_AEC` | Still P2.4; simplex quality is limited |

Do **not** port extra WakeNet models unless product wants English/Myanmar wake words (`scripts/build.py --wake-word …`). Do **not** enable device AEC without a hardware I2S loopback / codec reference pin (none on this SKU).

### 12.4 Boot, NVS, recovery, OTA rollback

| Mechanism | Upstream | This tree | Action |
|-----------|----------|-----------|--------|
| `nvs_flash_init`; erase on `NO_FREE_PAGES` / `NEW_VERSION` | `main.cc` | Same | Keep |
| `Settings` namespaces (`wifi`, `websocket`, `mqtt`, `assets`) | Persistent API | Same | Do not rename keys |
| `SystemReset` (hold two GPIOs at boot → erase NVS and/or otadata) | Class exists | **Never constructed** in otto **or** in any board grep | P2.UX4: wire a **single-button** gesture on GPIO 0 (this robot has no second reset pin) |
| Otto factory reset | Stock otto: none | Same dead `#include "system_reset.h"` | Port the *idea*, not dual-GPIO |
| Failed Wi-Fi → AP | 60 s | Same | Keep |
| Dual-bank OTA | `partitions/v2/16m.csv`: `ota_0`/`ota_1` + `otadata` | Same | C-OTA.5 / P2.UX6 |
| `MarkCurrentVersionValid` | After successful version check | Same | Keep; test rollback |
| Factory app partition | Not in 16M v2 table | Same | `ResetToFactory` would only erase otadata; do not rely on a factory app slot |
| Default OTA URL | `https://api.tenclass.net/xiaozhi/ota/` | otto `config.json` overrides to lab `http://206.189.94.197:8000/xiaozhi/ota/` | C-OTA.4 HTTPS + VPS channel |

### 12.5 What is already at parity (do not reimplement)

- SoftAP pairing LCD + `wificonfig.ogg` + 60 s timeout.
- Activation code LCD + digit TTS + `/activate` poll.
- AFE WakeNet, speaking barge-in, assets `srmodels.bin` reload.
- NVS init, dual-bank OTA, rollback mark.
- Config-mode audio loopback via GPIO 0 click.

### 12.6 Foundational work to implement (this plan only)

1. **P2.UX2** Idle re-pair on GPIO 0 (long-press or triple-click).  
2. **P2.UX4** Boot-held NVS wipe (Wi-Fi + websocket token) so a wrong password is recoverable.  
3. **P2.UX3** Hardware-test activation with a temporary server `activation.code`.  
4. **P2.UX1 / P2.UX5 / P2.UX6** Prove pairing audio, wake word, and OTA rollback on the no-camera robot.  
5. Optional branding: AP hostname prefix `PhoeLone`.  
6. Optional later: merge upstream `NotifyPlayer` / `kDeviceStateNotifying` if prompt playback blocks pairing.
