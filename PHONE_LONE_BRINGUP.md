# Phoe Lone Option B bring-up

Stock `otto-robot` firmware. No GPIO edits. Servo power stays off until LCD and audio work.

## Build

ESP-IDF v6.0.2 only (not PlatformIO). **Use `C:\PhoeLone`** — a space-free junction to this repo. Do not build from a path that contains spaces (the linker can fail at the final step).

One-time junction:

```powershell
cmd /c rmdir C:\PhoneLone
cmd /c mklink /J C:\PhoeLone "C:\Users\USER\Desktop\phoe_lone"
```

Build:

```powershell
$env:IDF_PATH = "C:\esp\v6.0.2\esp-idf"
$env:IDF_TOOLS_PATH = "C:\Espressif"
cmd /c "call `"%IDF_PATH%\export.bat`" && cd /d C:\PhoeLone && python scripts\build.py otto-robot --name otto-robot"
```

## Flash (servos unpowered)

1. Disconnect the external 5 V servo rail. Keep GND shared only when you later power servos.
2. Leave GPIO 12 unwired (firmware CS / optional right-hand servo).
3. Flash and open the serial monitor at 115200:

```powershell
.\scripts\phoe_lone_flash.ps1
# or: .\scripts\phoe_lone_flash.ps1 -Port COMx
```

Replace `COMx` with the ESP32-S3 USB serial port if auto-detect fails.

## Serial checks

Stop if auto-detect logs the camera map (`摄像头版`). That profile uses legs 43/44/5/6.

Expect:

- No reset loop
- 16 MB flash, octal PSRAM
- `自动检测硬件版本: 无摄像头版` (no-camera)
- LCD backlight / image
- Soft-AP or BluFi for Wi-Fi

First boot uses `CONFIG_OTA_URL` from `otto-robot/config.json`:

```
http://206.189.94.197:8000/xiaozhi/ota/
```

Paste that **plain URL only**. Do not paste a Markdown link such as `](http://...)` — that makes the device POST a 404 path.

Build English firmware:

```powershell
python scripts/build.py otto-robot --name otto-robot --language en-US
```

## Servo smoke (external 5 V, shared GND)

See `scripts/phoe_lone_hardware_test.md` for the full checklist.

## Local backend (after bring-up)

```powershell
cd backend
python server.py
```

Then set only this plain string (no Markdown):

```
CONFIG_OTA_URL="http://206.189.94.197:8000/xiaozhi/ota/"
```
