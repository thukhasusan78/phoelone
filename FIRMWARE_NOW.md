# Mickey firmware sprint — do this now

**Status:** implementation brief for the ESP32 repo. Do not implement C++ in this FastAPI tree.  
**Date:** 2026-09-02  
**Firmware repo:** [thukhasusan78/phoelone](https://github.com/thukhasusan78/phoelone)  
**Board:** `mickey` (ESP32-S3 N16R8). Build: `python scripts/build.py mickey --name mickey --language en-US`  
**OTA:** `https://phoelone.thukha.online/xiaozhi/ota/` baked in `main/boards/mickey/config.json`  
**Wire contract:** [backend_spec.md](backend_spec.md) (frozen). Longer GPIO / sensor notes: [CLIENT_PRODUCTION_PLAN.md](CLIENT_PRODUCTION_PLAN.md). Companion Phase 6: [COMPANION_ROADMAP.md](COMPANION_ROADMAP.md) §7.1. Vision coprocessor: [VISION_PLAN.md](VISION_PLAN.md).

The VPS brain and companion dashboard (chat, care, RPS, tic-tac-toe, alarm, settings) are **ahead of the robot**. This sprint makes Mickey safe, alive on the desk, and reachable from the phone without a wake word.

**Invariant:** voice, wake word, Opus, Otto MCP, GIFs, alarm/sleep MCP, and the existing hello/listen/abort path must keep working. New work runs on **separate FreeRTOS tasks below audio priority**.

Do not skip Slice 1. Stop after each slice and run that slice’s acceptance list.

---

## 0. Why this order

The cloud already:

- Sends JSON `type: ping` every 30 s (including during TTS/music) with `ts_ms`.
- Accepts `type: pong` (not required; old firmware stays valid).
- Routes `notifications/phoe_lone.event` (`pet` / `pickup` / `putdown` / `fall` / `sleep`). Pet while listening can speak a short Burmese line. Fall inhibits Otto motion for 5 s except `self.otto.stop`.
- Skips the ~100 s device-idle close **while a dashboard tab is open** — that cannot invent a socket if Mickey never connected.

Firmware still typically:

- Logs `Unknown message type: ping` every 30 s.
- May `vTaskDelete` on `self.otto.stop` → servo sag.
- Treats GPIO 12 as a hand servo **and** LCD CS.
- Returns IMU/light/touch as `wired: false`.
- Sits still in idle (XiaoZhi session model: wake → talk → close → statue).
- Opens `/xiaozhi/v1/` only for a voice turn → dashboard says “Wake Mickey first.”

Idle life and pet/fall **must work with the WebSocket closed**. Cloud is an enhancement.

```
Slice 1  safety (stop hold + hands NC + low battery)
Slice 2  ping → pong (silence WARN; prove app task alive)
Slice 3  sensors local + notify (MPU / touch / light)
Slice 4  idle director (EMO fidget, no cloud)
Slice 5  always-on WS (companion play without wake)
```

Slice 5 depends on Slice 2. Slices 3–4 do not depend on Slice 5. Sensors may wait on a pin photo if modules are not soldered yet — do not invent GPIOs.

---

## 1. Frozen hardware (do not remap)

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
| Battery ADC2_CH3 | **14** |

Reject for new sensors: `0`, `3–7`, `9–12`, `14–18`, `19–21`, `26–39`, `43`, `44`, `46`. Camera-board I2C 15/16 is **speaker BCLK/LRCK** on this SKU — never reuse.

**Proposed sensor pins** (confirm on the bench; if already soldered to other *free* pins, use those):

| Sensor | GPIO |
|--------|------|
| MPU6050 SDA / SCL | **41** / **42** |
| MPU6050 INT (optional) | **40** |
| Light (BH1750 / VEML7700) | same 41/42 |
| Light fallback LDR | **1** (ADC1 only; not ADC2) |
| Touch TTP223 | **47** (3.3 V digital) |

MPU at **3.3 V**, shared GND, 4.7 kΩ pull-ups if the module has none. Servos stay on external 5 V.

---

## 2. What the VPS already speaks (copy these shapes)

Do not add `type: iot`. Do not require MQTT. Do not wait for a Python change.

### 2.1 Keepalive

Server → device every ~30 s:

```json
{ "session_id": "<hello uuid>", "type": "ping", "ts_ms": 1710000000000 }
```

Device → server (Slice 2):

```json
{ "session_id": "<hello uuid>", "type": "pong", "ts_ms": 1710000000000 }
```

No emotion, no TTS, no MCP. `ESP_LOGD` only. Echo `ts_ms` if present.

### 2.2 Sensor pull (after Slice 3)

Keep `wired` forever. Until drivers work, **keep** `wired: false` stubs.

Live IMU success: `wired: true`, `ax/ay/az` (g), `gx/gy/gz` (deg/s), `pitch`, `roll`, `temp_c`, `event` one of `still` | `moving` | `pickup` | `putdown` | `fall` | `shake`.  
I2C fail: `{ "wired": true, "ok": false, "error": "i2c_nack" }` — never fake 1 g.

Light: `{ "wired": true, "lux": 120, "bucket": "indoor", "raw": 1840 }`  
`bucket`: `dark` | `dim` | `indoor` | `bright`. Analog fallback may omit `lux`.

Touch: `{ "wired": true, "touched": true, "count": 14, "ms_held": 320 }`.

### 2.3 Sensor push (no JSON-RPC `id`)

Only if the audio channel is open. **No reply expected.** Coalesce max **2 events/s**. During speaking/music send **`fall` only** (drop `pet`). Do **not** emit `bright` / `dark` in this sprint (backend_spec: firmware does not emit those).

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
      "imu": { "pitch": 8.0, "az": 0.2 }
    }
  }
}
```

`event`: `pickup` | `putdown` | `fall` | `pet` | `sleep` (sleep already used ~400 ms before socket close).

Fall: **stop servos locally first**, then notify if WS open.

---

## 3. Slice 1 — Safety (must flash first)

**Owner:** firmware. **Goal:** stop does not sag; GPIO 12 is never a servo; low battery does not walk.

### 3.1 Cooperative stop (P0.1)

Patches live in this repo: [`firmware/otto-robot/patches/`](firmware/otto-robot/patches/). Apply in the **phoelone** tree. Live files are under `main/boards/mickey/` (legacy patches say `otto-robot/` — retarget the path if `patch -p1` misses).

| Patch | Effect |
|-------|--------|
| `001-oscillator-keep-hold.patch` | Re-`Attach` must not `ledc_stop`; refresh `Write(pos_)` |
| `002-home-force-stance.patch` | `Home()` always reapplies 90° even if resting |
| `003-stop-cooperative-home.patch` | Stop = flag + drain queue + `ACTION_HOME`; **no** `vTaskDelete` |

Action loops abort early when `stop_requested_`. Trims in NVS unchanged.

### 3.2 No hands (P0.2)

Mickey is a **4-servo** robot. Hand pins, `has_hands_`, and hand MCP actions are removed. GPIO 12 stays **LCD CS only**. GPIO 8 is free for a future vision IRQ (see `VISION_PLAN.md`). Do not assign GPIO 8 until the bench photo confirms it is unused.

### 3.3 Low battery (P0.4)

`PowerManager` already polls ADC and GPIO 21. If level &lt; ~15% (NVS-tunable) and not charging:

- Reject `QueueAction` except home/stop.
- Play low-battery OGG at most once per N minutes.
- Dim backlight; `SetEmotion("sleepy")`.
- No walk / jump / showcase.

### Acceptance (Slice 1)

- [ ] `self.otto.stop` (voice, dashboard Stop, or MCP) holds pose ≥ 30 s with 5 V on the servos.
- [ ] `self.otto.action` does not list hand motions; LCD never glitches.
- [ ] Below threshold: walk/jump refused; face sleepy.

---

## 4. Slice 2 — JSON ping / pong (P0.6)

**File:** `main/application.cc` `OnIncomingJson`, **before** the unknown-type WARN.

If `type == "ping"`:

1. Do not change device state, emotion, or audio.
2. Schedule send of `type: pong` with the hello `session_id` and echoed `ts_ms`.
3. Return.

**Also verify (P0.7):** whether WebSocket opcode 0x9 ping updates XiaoZhi’s **120 s** last-incoming timer (`websocket_protocol.cc`). If only TEXT/BINARY reset it, JSON ping remains mandatory. Document in `docs/websocket.md`.

**Do not** handle ping with `alert`, TTS, or empty Opus.

### Acceptance (Slice 2)

- [ ] Serial: no `Unknown message type: ping` at WARN.
- [ ] Optional `pong` visible in server logs (`session` receive path, not `unknown_type`).
- [ ] Channel stays open &gt; 3 minutes of silence (listening or after `tts/stop` auto-listen).
- [ ] Local music / long TTS ≥ 4 minutes does not die from the device 120 s timer (server already keeps pinging during SPEAKING).

---

## 5. Slice 3 — Sensors local + notify (P0.S)

**Hard rule:** personality works with WS **closed**. Notifications are best-effort.

### 5.1 Pins

Photograph SDA/SCL/INT/light/touch. Diff against §1. Commit `#define`s in `main/boards/mickey/config.h` (e.g. `PHOE_LONE_IMU_SDA`). First boot with **servo 5 V unpowered** until WHO_AM_I succeeds.

If modules are not on the desk yet: keep stubs, skip to Slice 4, return here.

### 5.2 Architecture

New files (suggested): `main/boards/mickey/phoe_lone_sensors.h/.cc`.

```
SensorTask  (priority < audio encoder, stack ~4–6 KB)
  loop 20–50 ms (or block on INT + 20 Hz poll)
    I2C mutex: MPU + light
    touch: ISR sets flag; task debounces 30–50 ms
    classify IMU / light bucket / touch edge
    --- local, never wait on WS ---
    fall: OttoStopAndHome() immediately
    pet / pickup: SetEmotion + optional OGG; pause idle director
    --- if audio channel open ---
    SendMcpMessage(notification), no id
MCP get_reading / get_level / get_state: memcpy last snapshot
```

Never I2C from the WebSocket or audio callback. Watchdog: the task must delay or block, not spin.

MPU bring-up: 100 kHz first; probe `0x68` then `0x69`; WHO_AM_I in serial; wake `PWR_MGMT_1`; ±2 g / ±250 dps. Classify `fall` as tilt ≳ 55° for &gt; 150 ms or `|az|` far from 1 g with large gyro (tune in NVS). Unplug → `ok: false`, not a frozen 1 g.

### 5.3 Local reactions vs cloud

| Situation | Firmware |
|-----------|----------|
| WS closed, pet | Face + optional tiny motion/OGG |
| WS open, listening, pet | Same + notify |
| WS open, speaking/music, pet | Local only; no pet notify |
| Fall | Stop+home **&lt; 200 ms**, then notify if WS open |
| Pickup | Pause idle director; held face |

Do **not** start a cloud voice turn from a pet. The VPS may speak one line if listening; duplicate Otto dance from the device fights the idle director.

### Acceptance (Slice 3)

- [ ] Serial: MPU WHO_AM_I ok; touch edges; light buckets change with a flashlight.
- [ ] MCP pull `wired: true` and plausible numbers.
- [ ] Unplug I2C → `ok: false`.
- [ ] Pet: face changes &lt; 200 ms with **WS closed**.
- [ ] Fall: servos stop &lt; 200 ms **before** any notify.
- [ ] VPS journal: `session.phoe_lone_event` for pet/fall (when WS open).
- [ ] Audio / wake word still clean; no watchdog during I2C.

---

## 6. Slice 4 — Idle director (P1.1–P1.5)

Highest EMO ROI. **No cloud.** New `phoe_lone_behavior.h/.cc`.

- Runs in `kDeviceStateIdle` (typical: WS closed). v1: **off while the mic is hot** (servo noise vs VAD).
- Every **8–20 s** (random): blink GIF (`staticstate` / `winking` / `sleepy`) **or** 1–2° look, duration &lt; 800 ms, then home. Queue depth **1**. Bound speed.
- Preempt immediately: wake word, GPIO 0, pet, pickup, fall, `OpenAudioChannel`, any `self.otto.action`.
- Incoming `type: llm` emotion: show that GIF ~30 s, then resume the cycle.
- After sensors: pickup freezes fidget; putdown resumes after ~1 s stable; dark bucket → sleepy + dim (local only; still no `bright`/`dark` notify this sprint).

Do **not** call the LLM to pick a fidget. Do **not** auto-dance to music unless MCP asked.

### Acceptance (Slice 4)

- [ ] Visible fidget on the desk with nobody talking.
- [ ] Wake word / button cancels fidget immediately.
- [ ] Pickup (if IMU live) freezes motion; pet still plays a reaction.

---

## 7. Slice 5 — Always-on companion socket (Phase 6a)

**Goal:** open https://phoelone.thukha.online/ and play dance / RPS / TTT / chat without a wake word.

Today the device opens `/xiaozhi/v1/` for a **voice** session. The dashboard cannot move the body if `SessionManager` has no session.

Firmware:

- Stay connected in idle, **or** reconnect automatically, without being in a listen/speak turn.
- Keep answering JSON `ping` with `pong` (Slice 2).
- Idle director still runs locally and is preempted by MCP / wake / pet / fall.
- Do not send uplink Opus in this idle-connected mode unless the user wakes or the server starts TTS (existing speaking path).
- Sleep (`self.mickey.sleep.now`) still emits `event: sleep` then closes; dashboard already shows “Mickey is sleeping.”

Do **not** use the idle socket to request fidget decisions from Gemini.

**Not in this slice:** LCD `preview_image` / RPS icons (Phase 6b), signed OTA `.bin`, protocol v2 / server AEC.

### Acceptance (Slice 5)

- [ ] Power on, no wake word: dashboard presence **Online** within a few seconds of Wi-Fi.
- [ ] Dance pad / TTT / chat work from the phone while Mickey is idle-connected.
- [ ] Wake word still starts a normal listen turn.
- [ ] Sleep now → dashboard sleeping hint; wake/button restores Online.

---

## 8. Out of this sprint

| Item | Why later |
|------|-----------|
| Copy board to `main/boards/phoe-lone/` | Live identity is already `mickey`; do this only before a **real** OTA `.bin` if you still want that name |
| Signed OTA / dual-bank product channel | Lab must stay `0.0.0` + 404 `none.bin`, `force: 0` |
| `preview_image` LCD icons | Needs firmware snapshot + static assets on the VPS |
| `bright` / `dark` notifications | Spec currently forbids them; local dim is enough |
| Camera / ToF | This SKU is no-camera |
| MQTT voice, 4G, LivingAI APIs | Out of scope |
| Simon / trivia on the dashboard | Python; not firmware |

---

## 9. File map (phoelone tree)

| Path | Slice |
|------|--------|
| `main/boards/mickey/config.h` | 1 hands NC; 3 sensor pins |
| `main/boards/mickey/oscillator.cc` | 1 patch 001 |
| `main/boards/mickey/otto_movements.cc` | 1 patch 002 |
| `main/boards/mickey/otto_controller.cc` | 1 patch 003; 3 MCP live sensors |
| `main/boards/mickey/phoe_lone_sensors.*` | 3 **new** |
| `main/boards/mickey/phoe_lone_behavior.*` | 4 **new** |
| `main/application.cc` | 2 ping/pong |
| `main/protocols/websocket_protocol.cc` | 2 last-incoming vs opcode ping; 5 idle connect |
| `docs/websocket.md` | 2 ping/pong; 5 idle WS |
| `main/boards/mickey/config.json` | keep HTTPS OTA URL; `type: mickey` |

Servo patches to copy from the VPS:

```bash
# from phoelone repo root; retarget otto-robot → mickey if needed
patch -p1 < /path/to/phoe_lone_server/firmware/otto-robot/patches/001-oscillator-keep-hold.patch
patch -p1 < /path/to/phoe_lone_server/firmware/otto-robot/patches/002-home-force-stance.patch
patch -p1 < /path/to/phoe_lone_server/firmware/otto-robot/patches/003-stop-cooperative-home.patch
```

---

## 10. Smoke after each flash (do not skip)

1. Hello within 10 s of opening the channel.
2. Burmese STT then TTS.
3. Walk / stop (pose holds).
4. Dashboard Stop during TTS.
5. After Slice 2: no ping WARN; 4-minute local track survives.
6. After Slice 5: phone Play tab without wake word.

If audio, wake word, or display regresses, revert the last slice before continuing.

---

## 11. Next sprint (Slices 6–9) — append 2026-09-02

**Do not delete Slices 1–5 above.** They remain the historical brief. This section is what the Firmware Agent implements **next**.

On `phoelone` `main`, Slices 1–5 are largely already flashed (`mickey_sensors.cc`, `mickey_behavior.cc`, `CONFIG_COMPANION_KEEP_CHANNEL`, ping/pong, hands NC, cooperative stop). IMU/touch are live; light is still `wired: false`. Idle director exists (face first; body after 60 s; no tip-over clips). Always-on `/xiaozhi/v1/` is on.

The VPS already handles `ping`/`pong`, `notifications/phoe_lone.event` (pet / pickup / fall / sleep), catalog aliases for `self.mickey.*` and `self.phoe_lone.*`, and dashboard copy that does not say “wake first.” Cloud is still an enhancement. Idle life, pet, pickup, and fall **must keep working with the WebSocket closed**.

```
Slice 6  emotion → tiny motion (P1.2)     ← start here
Slice 7  idle director refinements
Slice 8  face consistency (neutral vs staticstate)
Slice 9  light sensor (only if the module is on the desk)
```

Slice 9 waits on a pin photo. Do not invent GPIOs. Slices 6–8 need no new hardware. Stop after each slice and run that slice’s acceptance list.

Live idle clips (refine in Slice 7; do not throw them away): face `winking` any time, `sleepy` after 60 s; body after 60 s = slow `swing` (amount 20, period 2800 ms) or reduced `walk` (amount 18, period 3200 ms). **No** tiptoe-swing / shake-leg / bend. Fidget IMU mask while a clip runs (+400 ms). Idle-director **body** clips stay off while listening or speaking. Incoming `type: llm` emotion yields the director for 30 s.

LCD CS on this SKU is strapped to GND (`display_cs_pin = NC`). **Never drive GPIO 12.**

### Illusion of Life — firmware owns the servo map (Slices 6–9)

The VPS does **not** pick fidgets, idle ticks, or rest-face. It only sends `{ "type": "llm", "emotion": "<name>" }` (before `tts/start`, and immediately on dashboard Face / Gemini `set_emotion`; aliases already canonicalized: `shocked`→`surprised`, `crying`→`sad`, `funny`→`laughing`, `anger`→`angry`). **Firmware completely owns** the emotion → body table in Slice 6, when each clip may run, and when servos must freeze. Do not invent a new JSON `type`. Do not ask Gemini to walk for a smile.

Three physical rules (EMO-like). They apply for the whole of Slices 6–9 and **amend** safety item 5 below:

1. **STRICT FREEZE while Listening** (`kDeviceStateListening`). **Zero** servo movement. Protect VAD. Face GIF may still change if an `llm` arrives. Cancel any in-flight jitter / micro-sway / idle clip immediately on entering Listening, wake word, or GPIO 0. Do not start MCP dance clips while Listening.
2. **PRE-SPEECH JITTER.** The Slice 6 emotion → tiny-motion reaction (**&lt; 800 ms**, then home, except `sit`) must execute **immediately** on the incoming `type: llm` packet, in the Idle window **before** `tts/start`. Do not wait for TTS. Do not wait for a second packet. Fire once per `llm`. Skip if Listening, if `OttoIsBusy()` (user/dashboard MCP preempts), if motion-inhibited / pet afterglow / pickup pause. This is the smile jitter.
3. **MICRO-SWAY while Speaking** (`kDeviceStateSpeaking`). Optional, **very slow**, **silent**, face-led sway only — not a walk, not a replay of the Slice 6 jitter, not idle-director amplitude. Bound well below idle clips (1–2° look **or** amount ≤ 8, period ≥ 3000 ms, or a single slow lean). Never block Opus. Cancel immediately on `tts/stop`, abort, wake, pet, pickup, fall, `self.otto.stop` / `action`. The moment the device returns to Listening, servos **stop**.

---

## 12. Safety constraints (Slices 6–9)

These override cleverness. Do not violate them.

1. **4-servo balance.** No large dual-leg / dual-foot oscillation. No `shake_leg`, `tiptoe_swing`, or `bend` in idle or emotion maps (they tip this SKU). Emotion motion is **jitter / tiny swing / sit / freeze / home** only.
2. **Cap duration.** Any emotion-triggered motion **&lt; 800 ms**, then home (except `sit`, which may hold). Queue depth **1**. If Otto is already busy with user/dashboard MCP, **skip** the gesture.
3. **Never block Opus.** Do not wait on servos from the audio / WS task. Gesture goes through the existing fidget/action queue (`OttoTryQueueFidget` / equivalent), same priority as idle clips.
4. **Preempt immediately:** wake word, GPIO 0, pet, pickup, fall, `OpenAudioChannel`, `self.otto.action` / `servo_sequences` / `stop`, sleep. Emotion gestures yield to all of these.
5. **Listening freeze / speaking micro-sway.** While `kDeviceStateListening`: **zero** servo movement (VAD). Face GIF may change. While `kDeviceStateSpeaking`: idle-director body clips stay **off**; optional face-led **micro-sway** from §11 Illusion of Life is allowed. Slice 6 `&lt; 800 ms` jitter is Idle / pre-`tts/start` only — do not replay it during TTS.
6. **Fall is local.** `OttoStopAndHome()` **&lt; 200 ms** before any notify. Do not wait on the VPS. Keep the fidget IMU mask so self-motion does not false-fall; true freefall (`|a| < 0.25 g`) still homes.
7. **Low battery.** No body fidget, no emotion walk/swing, no MCP dance. Home + `sleepy` + dim still allowed. `self.otto.stop` / `home` still work.
8. **No cloud fidget.** Do not call Gemini / open a turn to pick a blink. Do not auto-dance to music unless MCP asked.
9. **Do not double-dance.** The VPS already refuses Otto tools on pet/pickup/fall. Firmware must not queue a second showcase/walk on those events. Pet stays happy GIF + existing tiny jitter.
10. **Pins and protocol.** No remap of §1. No `type: iot`. No MQTT voice. No `bright` / `dark` MCP notifications this sprint (local dim is enough). No protocol v2 / `features.aec`. Lab OTA stays `0.0.0` + 404 `none.bin`, `force: 0`.

Leave both `self.mickey.*` and `self.phoe_lone.*` sensor aliases unless you coordinate a backend PR. The VPS de-dupes to Mickey names when both are listed.

---

## 13. Slice 6 — Emotion → tiny motion (P1.2)

**Owner:** firmware. **Goal:** a voice/`llm` smile moves the body a little, without fighting audio or tipping over.

**Files:** `main/boards/mickey/mickey_behavior.cc` (and `.h`), hook from the existing `RegisterExternalEmotionCallback` / `OnIncomingJson` `type: llm` path. Reuse `OttoTryQueueFidget` — do **not** call `self.otto.action` from C++ by forging MCP.

Today `type: llm` only changes the GIF and pauses the idle director for 30 s. The body stays still. Implement a **capped table**:

| Incoming emotion (and aliases) | Face (already SetEmotion) | Body (new, idle only) |
|--------------------------------|---------------------------|------------------------|
| `happy`, `laughing`, `loving` | keep GIF | `kOttoFidgetJitter` **or** 1-cycle slow `swing` amount ≤ 12, duration &lt; 800 ms, then home |
| `sad`, `sleepy` | keep GIF | **no** walk. Optional `sit` **or** still + home. If sit, do not auto-home until next clip / wake / pet |
| `surprised` | keep GIF | freeze: `OttoCancelFidget` only (same as pickup). No extra jump |
| `angry` | keep GIF | **skip body** (shake_leg tips this SKU) |
| `thinking`, `confused`, `listening`, `speaking`, `neutral`, `staticstate`, others | keep GIF | no body |

Rules:

- Fire **once** per incoming `llm` emotion, not on every idle tick.
- Skip if not `kDeviceStateIdle`, if `OttoIsBusy()`, if `OttoMotionInhibited()`, if pet afterglow / pickup pause is active.
- After the gesture, keep the 30 s yield so idle clips do not immediately overwrite the face.
- Do **not** start a cloud turn. Do **not** walk on every smile.
- **Timing (do not wait):** run this table as soon as `type: llm` is parsed. Firmware is still Idle until `tts/start`; that is the pre-speech jitter window. See §11 Illusion of Life.
- During `kDeviceStateSpeaking`, do **not** re-run this table. Optional face-led micro-sway is a **separate**, slower, silent clip (firmware-owned). Listening remains a strict freeze.

### Acceptance (Slice 6)

- [ ] Dashboard or voice sets emotion `happy` while idle-connected: tiny jitter or sway &lt; 800 ms, then still; audio uninterrupted.
- [ ] Jitter starts on the `type: llm` packet itself (Idle, **before** `tts/start`), not after speech begins.
- [ ] Same test while **listening**: face may change; **no** servo motion.
- [ ] While **speaking** (TTS playing): optional very slow silent face-led micro-sway only — not a walk, not a second jitter.
- [ ] Emotion `sad`: sit or still; robot does not tip; no walk.
- [ ] Emotion `angry`: face only.
- [ ] Wake word during the tiny gesture cancels it immediately.
- [ ] User/dashboard `self.otto.action` still preempts; no stuck fidget flag.
- [ ] Pet jitter and emotion jitter do not queue on top of each other (depth 1).

---

## 14. Slice 7 — Idle director refinements

**Owner:** firmware. **Goal:** more alive on the desk without becoming a drunk waiter. **No cloud.**

**File:** `mickey_behavior.cc`. Keep the 60 s body gate and the no-tip clip list.

Changes:

1. **Face variety before body.** Weighted face-only pool while `idle_ms < 60000`: add `loving` (and keep `winking`). Do not add body to this pool. After 60 s, keep current `sleepy` / slow sway / slow step.
2. **Reset the 60 s body gate after real interaction.** Already resets on pet, pickup, and non-fidget Otto busy. Also reset when leaving `Listening`/`Speaking` back to `Idle` (chat or dashboard dance just finished) so he does **not** immediately shuffle. A 10–20 s face-only pause after a conversation is better than an instant walk.
3. **Time of day (if `has_server_time_`).** After ~22:00 local: prefer `sleepy` face, skip body clips, optionally dim backlight one step. Morning (after alarm `QueueMorningWake` or hour ≥ 7): prefer `winking` / `happy` face, allow body clips as today. If server time is unknown, keep the current 60 s behavior.
4. **Keep idle-director body clips off while listening/speaking.** Listening = **strict freeze** (do not regress). Speaking may run the §11 face-led **micro-sway** only — not idle walk/swing.
5. **Do not ask Gemini to pick a fidget.** Already true — do not regress.

Do **not** widen body amplitude. Do **not** add moonwalk / jump / showcase to idle.

### Acceptance (Slice 7)

- [ ] First minute on the desk: blinks / loving face; **no** walk.
- [ ] After 60 s with nobody touching him: occasional slow sway or tiny step; still no tip-over clips.
- [ ] After a voice chat or dashboard dance: at least ~10 s of still/face-only before body clips resume.
- [ ] At night with synced clock: sleepy face, no body fidget; backlight not full-blast.
- [ ] Wake word / button / pet / pickup still cancel immediately.
- [ ] WS closed: same personality (idle director does not need the cloud).

---

## 15. Slice 8 — Face consistency

**Owner:** firmware. **Goal:** stop flickering `neutral` vs `staticstate` after reconnect / alert / pet.

Today the idle director uses `staticstate` / `winking` / `happy` / `sleepy`. `Application::DismissAlert` and some idle-state handlers still `SetEmotion("neutral")`. After a WS error or companion reconnect the face can jump.

Pick **one** idle rest face: `staticstate` (Otto GIF default on this SKU).

- `DismissAlert` in idle → `staticstate` (not `neutral`).
- Entering `kDeviceStateIdle` after listening/speaking → `staticstate` unless pet afterglow or a 30 s llm yield is still active.
- Pet afterglow end already returns to `staticstate` — keep that.
- Do not remap GIF assets.

### Acceptance (Slice 8)

- [ ] After dashboard Stop / TTS end / ping-only idle: rest face is `staticstate`, not a random `neutral` flash.
- [ ] Pet 3 s afterglow still `happy`, then `staticstate` + home.
- [ ] `type: llm` emotion still shows for ~30 s, then the director resumes.

---

## 16. Slice 9 — Light sensor (hardware-gated)

**Owner:** firmware. **Skip this slice if no BH1750 / VEML7700 / LDR is soldered.** Photograph the net first. Diff against §1.

**Goal:** local night behavior (P1.5) with WS **closed**. MCP pull `wired: true`. **Do not** emit `bright` / `dark` notifications (spec forbids them this sprint).

- I2C light on the MPU bus (41/42) preferred; mutex with MPU. Fallback: ADC1 GPIO 1 only.
- Same SensorTask (`mickey_sensors.cc`); never I2C from WS/audio.
- `self.mickey.light.get_level` and `self.phoe_lone.light.get_level`: `{ "wired": true, "lux": …, "bucket": "dark"|"dim"|"indoor"|"bright", "raw": … }`. Analog may omit `lux`. Unplug / fail → `{ "wired": true, "ok": false, "error": "…" }` — never invent lux.
- Until the driver works, **keep** `wired: false` stubs (backend prompt already says light is unwired).
- Local only: `bucket == dark` for N minutes → `sleepy` GIF + dim backlight. Bright → wake face (`staticstate` / `winking`). Idle director uses the bucket; do not spam servo motion on every lux tick.

### Acceptance (Slice 9)

- [ ] Serial: light buckets change with a flashlight.
- [ ] MCP pull `wired: true` and plausible numbers.
- [ ] Cover the sensor: sleepy + dim with **WS closed**, within a few seconds.
- [ ] No `notifications/phoe_lone.event` with `bright` or `dark`.
- [ ] Audio / wake word / MPU still clean.

---

## 17. Out of Slices 6–9 (still later)

| Item | Why later |
|------|-----------|
| LCD `preview_image` / RPS icons (Phase 6b) | Needs `CONFIG_LV_USE_SNAPSHOT` plus static 240×240 assets on the VPS |
| Signed OTA / dual-bank / `phoe-lone` board copy | Lab must stay `0.0.0` + 404 `none.bin`, `force: 0` |
| Protocol v2 / server AEC / `listen: realtime` | Both sides must ship together; stay on v1 |
| Music canned dance while a track plays (P1.6) | Optional; abort path must stay first. Do not start until Slice 6 is stable |
| Drop `self.phoe_lone.*` sensor aliases | Backend still discovers both; leave aliases unless you coordinate a backend PR |
| Camera / ToF | This SKU is no-camera |

---

## 18. File map for Slices 6–9 (phoelone tree)

| Path | Slice |
|------|--------|
| `main/boards/mickey/mickey_behavior.cc` / `.h` | **6, 7, 8** |
| `main/boards/mickey/otto_controller.cc` | 6 fidget queue only if a new `OttoFidget` kind is required |
| `main/application.cc` | 8 `DismissAlert` rest face; 6 already has `llm` → `external_emotion_callback_` |
| `main/boards/mickey/mickey_sensors.cc` / `.h` | **9** light driver + JSON; keep IMU/touch |
| `main/boards/mickey/config.h` | **9** light `#define` only after pin photo |
| `main/boards/mickey/config.json` | do not change OTA URL / `type: mickey` / `CONFIG_COMPANION_KEEP_CHANNEL` |

Do **not** re-apply `firmware/otto-robot/patches/001–003` unless a rebase lost cooperative stop.

---

## 19. Extra smoke after Slices 6–9

1. Hello within 10 s of Wi-Fi (no wake word — always-on WS).
2. After Slice 6: `happy` `llm` gesture &lt; 800 ms **on the packet**, before `tts/start`; **zero** servos while listening; optional slow silent micro-sway only while TTS is playing.
3. After Slice 7: one minute of face-only, then slow sway; no tip.
4. After Slice 8: idle rest face is `staticstate` after Stop / TTS end.
5. Pet 800 ms: happy face + tiny jitter with WS closed.
6. Tip the robot: servos home &lt; 200 ms; no watchdog.

If audio, wake word, or display regresses, revert the last slice before continuing.
