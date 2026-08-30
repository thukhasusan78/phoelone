# Mickey Option B bring-up

Stock `mickey` firmware (no-camera, no-hands). Servo power stays off until LCD and audio work.

Wake word: say **Mickey**, **Hey Mickey**, or **Hi Mickey**. AFE/WakeNet also accepts **Hi ESP** (`wn9_hiesp`) as a reliable English fallback.

## Build

ESP-IDF v6.0.2 only (not PlatformIO). **Use `C:\Mickey`** — a space-free junction to this repo. Do not build from a path that contains spaces (the linker can fail at the final step).

One-time junction:

```powershell
cmd /c rmdir C:\Mickey
cmd /c mklink /J C:\Mickey "C:\Users\USER\Desktop\phoe_lone"
```

Build:

```powershell
$env:IDF_PATH = "C:\esp\v6.0.2\esp-idf"
$env:IDF_TOOLS_PATH = "C:\Espressif"
cmd /c "call `"%IDF_PATH%\export.bat`" && cd /d C:\Mickey && python scripts\build.py mickey --name mickey --language en-US"
```

## Flash (servos unpowered)

1. Disconnect the external 5 V servo rail. Keep GND shared only when you later power servos.
2. GPIO 12 is LCD CS only (hand servos are `GPIO_NUM_NC`).
3. Flash and open the serial monitor at 115200:

```powershell
.\scripts\mickey_flash.ps1
# or: .\scripts\mickey_flash.ps1 -Port COMx
```

Replace `COMx` with the ESP32-S3 USB serial port if auto-detect fails.

## Serial checks

Stop if auto-detect logs a camera map. This SKU forces no-camera (`OTTO_VERSION_NO_CAMERA`).

Expect:

- No reset loop
- 16 MB flash, octal PSRAM
- `Forcing no-camera hardware config`
- LCD backlight / image
- Soft-AP SSID prefix `Mickey`
- AFE log `detector: WakeNet+MultiNet`

First boot uses `CONFIG_OTA_URL` from `mickey/config.json`:

```
https://phoelone.thukha.online/xiaozhi/ota/
```

Paste that **plain URL only**. Do not paste a Markdown link such as `](http://...)` — that makes the device POST a 404 path. The OTA `board.type` identity is `mickey`.

Build English firmware:

```powershell
python scripts/build.py mickey --name mickey --language en-US
```

## Servo smoke (external 5 V, shared GND)

See `scripts/mickey_hardware_test.md` for the full checklist.

## Local backend (after bring-up)

```powershell
cd backend
python server.py
```

Then set only this plain string (no Markdown):

```
CONFIG_OTA_URL="https://phoelone.thukha.online/xiaozhi/ota/"
```
