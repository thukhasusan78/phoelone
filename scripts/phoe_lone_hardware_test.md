# Phoe Lone Option B — post-flash hardware checklist

Run only after `scripts/phoe_lone_flash.ps1` shows a stable boot log.

## Before servos

- [ ] No reset loop
- [ ] Log: `自动检测硬件版本: 无摄像头版` (no-camera)
- [ ] 16 MB flash / octal PSRAM reported
- [ ] LCD backlight and image
- [ ] Speaker / mic smoke test
- [ ] Wi-Fi provision (soft-AP or BluFi)

## Servo smoke (external 5 V, shared GND)

- [ ] Servo rail powered; USB/debug still connected
- [ ] One slow walk via MCP or voice: `self.otto.action` with `action=walk`, `steps=1`, `speed=2000`, `direction=1`
- [ ] Immediately: `self.otto.stop`
- [ ] Serial still shows no-camera after motion

## Stop conditions

- Log says **摄像头版** (camera version) — do not continue; wrong pin map
- GPIO 12 must stay unwired (PCB ties LCD CS to GND)

## Local backend (after bring-up)

```powershell
cd backend
python server.py
```

Set only `CONFIG_OTA_URL` in menuconfig to `http://<LAN-IP>:8000/xiaozhi/ota/`, then rebuild.
