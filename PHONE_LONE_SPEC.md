# Phoe Lone AI Robot Firmware Specification

## 1. Purpose

Phoe Lone is an ESP32-S3-based desktop AI robot inspired by the interaction style of LivingAI's EMO. It uses the open-source [XiaoZhi](https://github.com/78/xiaozhi-esp32) client as its voice, networking, display, and device-control foundation. This project is an independent implementation and must not copy proprietary LivingAI firmware, assets, branding, cloud APIs, or industrial design.

This first firmware profile uses XiaoZhi's existing `otto-robot` board implementation because it already provides the closest available combination of voice interaction, animated expressions, camera support, and servo motion.

**Build system:** official ESP-IDF extension and `scripts/build.py`. PlatformIO is not used.

## 2. Known Hardware

- MCU: ESP32-S3
- Module capacity: N16R8 (16 MB flash, 8 MB octal PSRAM)
- Framework: ESP-IDF v6.0.2 (preferred; CI and `scripts/build.py` default)
- Minimum declared IDF: `>=5.5.2` in `main/idf_component.yml`
- Initial board implementation: `main/boards/otto-robot`
- Firmware language: English (`en-US`)
- Flash / PSRAM defaults already match N16R8:
  - `sdkconfig.defaults`: 16 MB flash, custom partition `partitions/v2/16m.csv`
  - `sdkconfig.defaults.esp32s3`: QIO flash, 240 MHz CPU, octal PSRAM @ 80 MHz

N16R8 describes memory capacity but does not define the development board's GPIO wiring. The existing Otto pin map is only a starting profile and must not be flashed to connected peripherals until the real wiring has been checked.

## 3. Hardware Information Still Required

Record the exact part number and GPIO connection for each installed device:

- ESP32-S3 development board or module schematic
- Display controller, resolution, interface, and all pins
- Touch controller and pins, if present
- Microphone model (analog, PDM, or I2S) and pins
- Speaker amplifier or audio codec model and I2S/I2C pins
- Camera sensor and DVP pins, if present
- Servo count, function, signal pins, voltage, and external power rating
- Motor driver and pins, if wheeled motion is used
- Battery chemistry, capacity, charger, voltage-divider values, and ADC pin
- Buttons, LEDs, IMU, proximity sensors, and capacitive touch sensors

Servos and motors must use a suitable external power rail with a shared ground. They must not be powered from an ESP32-S3 GPIO or from an under-rated 3.3 V rail.

## 4. Functional Scope

### 4.1 First Prototype

- Boot reliably from 16 MB flash and initialize 8 MB octal PSRAM.
- Provision and reconnect to Wi-Fi.
- Capture microphone audio and play synthesized speech.
- Connect to a XiaoZhi-compatible server over WebSocket or MQTT/UDP.
- Display idle, listening, thinking, speaking, and error expressions.
- Accept button and wake-word activation.
- Expose safe MCP actions for robot movement.
- Stop motion immediately when requested or when a safety condition occurs.
- Save settings and servo trims in NVS.
- Report useful errors over a 115200-baud serial monitor.

### 4.2 Later Enhancements

- Camera-assisted interaction.
- Touch and proximity reactions.
- IMU-based fall or pickup detection.
- Battery gauge and low-voltage shutdown.
- Charging/dock behavior.
- Custom Phoe Lone expression assets.
- Idle personality animations and non-blocking behavior scheduling.
- Local control page or application.

## 5. Software Architecture

- `main/application.*`: application lifecycle and high-level interaction.
- `main/device_state_machine.*`: legal runtime state transitions.
- `main/audio/`: capture, playback, wake word, and audio processing.
- `main/protocols/`: XiaoZhi WebSocket and MQTT/UDP transports.
- `main/display/`: reusable display and expression support.
- `main/mcp_server.*`: device-side MCP registration and dispatch.
- `main/boards/otto-robot/`: current Phoe Lone hardware and movement baseline.
- `main/boards/otto-robot/config.h`: current GPIO definitions.
- `scripts/build.py`: canonical board/variant build entry point.
- `docs/custom-board.md`: how to add a unique `phoe-lone` board later.

Closest existing references for an EMO-like desk robot:

| Reference | Why it matters |
|-----------|----------------|
| `main/boards/otto-robot` | Servo + GIF emoji + MCP action queue + WebSocket debug |
| `main/boards/electron-bot` | Desktop robot with 6-DOF head/body/hand servos |
| `main/boards/spotpear/sp-esp32-s3-1.54-muma` | Explicit N16R8 companion hardware profile |
| `main/boards/movecall/moji-esp32s3` | Round 240×240 LVGL UI pattern |
| `main/boards/espressif/esp-vocat` | EmoteDisplay animated expressions |

Board-specific code must remain in a unique board directory. Before production or OTA use, copy the baseline into `main/boards/phoe-lone` and assign a unique `phoe-lone` board identity; do not ship using the `otto-robot` identity.

## 6. ESP-IDF Workflow

Use the official Espressif ESP-IDF VS Code / Cursor extension. Do not use PlatformIO for this project.

1. Install **ESP-IDF v6.0.2** through the ESP-IDF extension (or a manual Espressif installer).
2. Open this repository folder in Cursor or VS Code.
3. Set the extension target to **esp32s3**.
4. Source the ESP-IDF environment, then build the Otto baseline:

```powershell
python scripts/build.py otto-robot --name otto-robot
```

Equivalent `idf.py` flow after the board/sdkconfig chain has been applied:

```powershell
idf.py set-target esp32s3
idf.py build
idf.py -p COMx flash monitor
```

5. Connect the board and select the correct serial port in the ESP-IDF extension.
6. Disconnect or disable servo/motor power for the first firmware upload.
7. Flash with **ESP-IDF: Flash your project**.
8. Open **ESP-IDF: Monitor** at 115200 baud.

The first build downloads 50+ Component Manager dependencies into `managed_components/` and can take 30+ minutes on Windows.

When Phoe Lone has its own board directory:

```powershell
python scripts/build.py phoe-lone --name phoe-lone
```

## 7. Pin-Map Safety Gate

The current profile inherits two Otto hardware variants from `main/boards/otto-robot/config.h`. Those GPIO values must be compared with the Phoe Lone schematic before peripherals are powered.

Camera-version Otto servo pins (starting profile only):

- Right leg: GPIO 43
- Right foot: GPIO 44
- Left leg: GPIO 5
- Left foot: GPIO 6
- Left hand: GPIO 4
- Right hand: GPIO 7

Do not proceed to a full hardware test if:

- two outputs share a pin unintentionally;
- a peripheral uses flash/PSRAM pins;
- a boot-strapping pin is driven to the wrong level at reset;
- USB pins are reused while USB upload/debug is required;
- the display, camera, or audio bus voltage is incompatible;
- servo current can flow through the development board regulator.

## 8. Motion Safety Requirements

- Movement must run outside the audio and main event-loop tasks.
- Every motion must have bounded speed, angle, duration, and queue length.
- A stop command must cancel queued motion and return to a safe pose.
- Startup must not cause uncontrolled servo movement.
- Servo calibration must be completed with limbs mechanically unloaded.
- Low battery, detected fall, or control timeout should inhibit movement.
- Network messages and MCP parameters must be validated before execution.

Existing Otto MCP tools to reuse or fork:

- `self.otto.action`
- `self.otto.stop`
- `self.otto.get_status`
- `self.otto.set_trim` / `self.otto.get_trims`
- `self.otto.servo_sequences`

A later `phoe-lone` board should use a unique tool namespace such as `self.phoe_lone.*`.

## 9. Comment and Language Policy

- New and modified source-code comments use English.
- Existing non-English comments in the selected robot board implementation (`main/boards/otto-robot`) are translated to English.
- Runtime user-facing text and protocol descriptions are not automatically changed because they affect AI behavior and localization.
- Upstream boards, generated files, managed components, and third-party code remain unchanged.

## 10. Acceptance Criteria

### Build

- ESP-IDF v6.0.2 resolves dependencies and completes an ESP32-S3 build without errors.
- The partition layout fits 16 MB flash.
- Octal PSRAM is detected and passes startup initialization.

### Hardware Bring-Up

- Serial boot is stable with no reset loop.
- Wi-Fi provisioning and reconnection work.
- Microphone input and speaker output work without watchdog resets.
- Display orientation, color order, and backlight are correct.
- Each servo can be tested individually at a conservative speed.
- Emergency stop works during every movement type.

### AI Interaction

- The device transitions correctly among idle, listening, thinking, and speaking.
- Speech interruption and reconnect behavior work.
- MCP movement commands reject invalid names and out-of-range parameters.
- Expressions and movement do not block audio streaming.

## 11. Current Constraints

- Physical hardware cannot be validated from the N16R8 marking alone.
- There is no `phoe-lone` board in this tree yet; the Otto board is a development baseline only.
- The Otto implementation may not match the final robot mechanics, display, camera, audio circuit, or power design.
- Myanmar is not currently one of the firmware's listed built-in interface locales; English is selected for this profile.
- Windows builds work but are slower and more fragile than Linux; this is the official ESP-IDF path, not a PlatformIO wrapper.

## 12. Suggested Implementation Phases

### Phase 0 — Hardware BOM

Confirm display controller, audio codec, servo count/roles, battery ADC, and whether a camera is present.

### Phase 1 — Board skeleton

Create `main/boards/phoe-lone/` (`config.h`, `config.json`, board class + `DECLARE_BOARD`, `README.md`), then wire `BOARD_TYPE_PHOE_LONE` in `main/Kconfig.projbuild` and `main/CMakeLists.txt`. Build with `python scripts/build.py phoe-lone --name phoe-lone`.

### Phase 2 — Display and personality

Choose LVGL emoji, Otto GIF pack, or EmoteDisplay. Map server emotion strings to local assets.

### Phase 3 — Audio and interaction

Initialize the real codec/I2S pins, boot button / touch wake, wake-word model, and power-save timer.

### Phase 4 — Motion MCP

Port the Otto/Electron background action-queue pattern under `self.phoe_lone.*`, with auto-home and `stop`.

### Phase 5 — Protocol and OTA

Reuse stock WebSocket/MQTT. Register a unique board type with the backend/OTA channel.

### Out of scope unless required

- 4G/ML307 path
- Core changes to `Application`, `Protocol`, or shared `mcp_server.cc`
