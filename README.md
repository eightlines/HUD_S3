# HUD S3 — LILYGO T-Display S3 AMOLED Beamsplitter HUD

ESP-IDF firmware and host streaming/upload tools for turning a **LILYGO T-Display S3 AMOLED (v1.0)** into a **beamsplitter Heads-Up Display (HUD)** with a pure-black (`#000000`) background.

## Features
- **RM67162 QSPI AMOLED Driver (240×536)** for T-Display S3 AMOLED v1.0
  - Hardware horizontal/vertical mirroring (`MADCTL` `0x36`) for beamsplitter optics
  - Hardware brightness control (`0x51`)
  - Pure-black background so unlit AMOLED pixels stay invisible in the beamsplitter
- **Built-in Procedural HUD Animations**
  - Reticle & horizon HUD (`HUD_MODE_RETICLE`)
  - Dual-phase waveform & spectrum (`HUD_MODE_WAVEFORM`)
  - Cycle modes anytime with the **USER button (GPIO 21)**
- **Modify, Upload & Live-Stream**
  - **Wi-Fi Web UI + HTTP Upload (`http://192.168.4.1`)**: Upload `.hud` animations to flash (14 MB SPIFFS) and toggle mirror/brightness/orientation live
  - **Wi-Fi UDP Live Stream (port `5555`)**: Stream RLE16-compressed RGB565 frames in real time
  - **USB Serial Live Stream & Upload**: Wired streaming/uploading over the ESP32-S3 USB Serial/JTAG port
- **Rive Animation Support**: See [docs/HUD S3.md](docs/HUD%20S3.md) for both the host-baked `.riv` → `.hud` workflow (60 FPS) and native `rive-cpp` + ThorVG notes.

## Quick Start (ESP-IDF on Windows)

1. Open **ESP-IDF PowerShell / Command Prompt** and navigate to this project.
2. (Optional) Configure Wi-Fi credentials or HUD defaults:
   ```powershell
   idf.py set-target esp32s3
   idf.py menuconfig
   ```
   *(If you leave Wi-Fi SSID empty, the board creates an open SoftAP named `HUD-S3` at `192.168.4.1`.)*
3. Build, flash, and monitor:
   ```powershell
   idf.py build flash monitor
   ```

## Host Python CLI (`tools/hud_cli.py`)

Install dependencies:
```powershell
pip install -r tools/requirements.txt
```

Convert a GIF or PNG sequence (black background) to `.hud` and upload/stream:
```powershell
# Convert GIF -> RLE16 .hud framepack
python tools/hud_cli.py convert my_hud.gif -o anim.hud

# Upload to flash over Wi-Fi (plays on boot!)
python tools/hud_cli.py upload anim.hud --host 192.168.4.1

# Or upload over USB Serial
python tools/hud_cli.py upload anim.hud --port COM5

# Live-stream over Wi-Fi UDP or USB Serial
python tools/hud_cli.py stream my_hud.gif --host 192.168.4.1
python tools/hud_cli.py demo-stream --host 192.168.4.1
```

## Hardware Pinout (T-Display S3 AMOLED v1.0)
| Signal | GPIO |
| :--- | :--- |
| CS | `GPIO 6` |
| SCLK | `GPIO 47` |
| D0–D3 | `GPIO 18, 7, 48, 5` |
| RST | `GPIO 17` |
| TE | `GPIO 9` |
| USER Button | `GPIO 21` |
