# Mickey — flash + serial monitor (servos unpowered)
param(
    [string]$Port = "",
    [int]$Baud = 115200
)

$ErrorActionPreference = "Stop"
$ProjectRoot = Split-Path -Parent $PSScriptRoot
Set-Location $ProjectRoot

$env:IDF_PATH = if ($env:IDF_PATH) { $env:IDF_PATH } else { "C:\esp\v6.0.2\esp-idf" }
$env:IDF_TOOLS_PATH = if ($env:IDF_TOOLS_PATH) { $env:IDF_TOOLS_PATH } else { "C:\Espressif" }

function Find-EspPort {
    $py = "C:\Espressif\tools\python\v6.0.2\venv\Scripts\python.exe"
    if (-not (Test-Path $py)) { $py = "python" }
    & $py -c @"
from serial.tools import list_ports
for p in list_ports.comports():
    d = (p.description or '').lower()
    h = (p.hwid or '').lower()
    if any(x in d for x in ('usb serial', 'cp210', 'ch340', 'ftdi', 'jtag', 'usb-serial')) or '303a' in h:
        print(p.device)
"@
}

if (-not $Port) {
    $Port = (Find-EspPort | Select-Object -First 1)
}

if (-not $Port) {
    Write-Host "No ESP32 USB serial port found. Connect the board (servo power OFF) and rerun:"
    Write-Host "  .\scripts\mickey_flash.ps1 -Port COMx"
    exit 2
}

Write-Host "Using port $Port (servo 5V should stay disconnected)."
Write-Host "Expect serial: no-camera config, PSRAM, LCD. Stop if log says camera version."

$cmd = @"
call "$env:IDF_PATH\export.bat" && python "$env:IDF_PATH\tools\idf.py" -p $Port -b $Baud flash monitor
"@

cmd /c $cmd
