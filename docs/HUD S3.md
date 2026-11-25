# HUD S3

**Hardware:** LILYGO T-Display S3 AMOLED (v1.0) — 1.91" 240×536 RM67162 QSPI AMOLED + ESP32-S3R8 (8 MB OPI PSRAM, 16 MB Flash)
**Goal:** Beamsplitter Heads-Up Display (HUD) with pure-black (`#000000`) background, modifiable animations, and Wi-Fi + USB upload/live streaming.
**Repo:** https://github.com/eightlines/HUD_S3.git

---

## Hardware & Optics Notes
- **Why AMOLED?** Unlit `#000000` pixels emit zero light, so only the HUD graphics reflect off the 45° beamsplitter glass.
- **Mirroring:** A single 45° beamsplitter flips the image along one axis. The firmware enables `mirror_x` by default in `rm67162_set_orientation()` via `MADCTL` (`0x36`), and you can toggle `mirror_x`, `mirror_y`, `landscape`, and `brightness` (`0x51`) live from the Web UI.
- **v1.0 vs v2.0:** On **v1.0**, display power is tied directly to board power (unlike v2.0 which uses `GPIO 38`).

### RM67162 QSPI Pinout (v1.0)
| Signal | GPIO | Notes |
| :--- | :--- | :--- |
| `CS` | `6` | Chip Select |
| `SCLK` | `47` | QSPI Clock (40 MHz) |
| `D0` | `18` | QSPI Data 0 |
| `D1` | `7` | QSPI Data 1 |
| `D2` | `48` | QSPI Data 2 |
| `D3` | `5` | QSPI Data 3 |
| `RST` | `17` | Hardware Reset |
| `TE` | `9` | Tearing Effect |
| `USER` | `21` | Cycles animation modes |

---

## Firmware Architecture (ESP-IDF)
- `main/rm67162.cpp`: DC-less QSPI driver (`0x02` command framing, `0x32` + `0x002C00`/`0x003C00` 4-wire pixel writes) using a 16 KB internal DMA SRAM bounce buffer.
- `main/hud_renderer.cpp`: PSRAM RGB565 framebuffer + 2D HUD primitives + built-in Reticle & Waveform demos.
- `main/framepack.cpp`: Plays `.hud` RLE16-compressed animations from `/spiffs/anim.hud` (~14 MB flash partition). Because HUDs are ~85–95% pure black (`0x0000`), RLE16 shrinks each 257 KB frame to ~10–30 KB.
- `main/wifi_stream.cpp`:
  - SoftAP `"HUD-S3"` (`192.168.4.1`) if no SSID is set in `menuconfig`, or joins your Wi-Fi STA.
  - HTTP server on port `80` (dark Web UI, `/upload` to flash, `/api/settings`).
  - UDP live stream on port `5555` (`"HUDF"` chunked RLE16 frames).
- `main/usb_stream.cpp`: Binary `"HUDU"` framing over USB Serial/JTAG for wired live streaming and `.hud` upload.

---

## Using Rive Animations (`.riv`)
The Rive C++ runtime (`rive-cpp`) separates the animation/state-machine engine from `rive::Renderer`:
1. **Recommended for 60 FPS on ESP32-S3 (Host-Baked / Streamed):**
   - Design your HUD animation in Rive on a `#000000` artboard (`240×536` or `536×240`).
   - Export/render frames (PNG sequence or video/GIF) or stream from a desktop Rive runner over UDP/USB, then convert & upload with:
     ```powershell
     python tools/hud_cli.py convert ./rive_frames -o anim.hud --fps 30
     python tools/hud_cli.py upload anim.hud --host 192.168.4.1
     ```
2. **Native On-Device `rive-cpp`:**
   - ESP32-S3 has no 2D GPU, so running `rive-cpp` on-device requires a CPU vector rasterizer (e.g. **ThorVG**, similar to `ESP32Rive`) implementing `rive::Renderer` and `rive::Factory` into the PSRAM framebuffer. Expect ~15–30 FPS depending on vector complexity.

---

## Build & Workflow Commands
```powershell
# Build & flash from ESP-IDF terminal on Windows
idf.py set-target esp32s3
idf.py build flash monitor

# Convert, upload, or live-stream from Python
pip install -r tools/requirements.txt
python tools/hud_cli.py convert anim.gif -o anim.hud
python tools/hud_cli.py upload anim.hud --host 192.168.4.1
python tools/hud_cli.py demo-stream --host 192.168.4.1
```
