# ESP32 BLE HID MacroPad

An open-source, battery-powered wireless macro pad built on ESP32 with NimBLE BLE HID.

## Repository Structure

```
firmware/
  v4/                    — v4 stable firmware (NimBLE + optional ESP-NOW)
  v5/                    — v5 firmware (in development)
  media_remote/          — standalone AS5600 media remote firmware
  tests/                 — hardware test sketches

hardware/
  pin_reference_v5.md   — complete GPIO allocation for v5

docs/
  ROADMAP.md            — version history and development plan
  TESTING_LOG.md        — hardware test results
```

## Versions

| Version | Status | Description |
|---------|--------|-------------|
| v3 | Complete | 8-button, ST7735, ESP32-BLE-Keyboard |
| v4 | Stable | NimBLE stack, ghost connection fix |
| v4+ESP-NOW | Testing | Optional media remote via AS5600 |
| v5 | In Development | 12-key MX matrix, ST7789 240×320, AS5600 |

## Quick Start

### v4 Firmware
1. Install NimBLE-Arduino via Library Manager
2. Install TFT_eSPI and configure User_Setup.h for ST7735 128×160
3. Flash `firmware/v4/macropad_v4_nimble_espnow.ino`
4. Pair via Bluetooth as "ESP32 MacroPad"

### Media Remote
1. Flash `firmware/media_remote/media_remote.ino` to a second ESP32
2. Wire: AS5600 SDA→GPIO21, SCL→GPIO22, BTN1→GPIO27, BTN2→GPIO26, BTN3→GPIO25
3. Pair via Bluetooth as "MediaRemote"
4. Or set macropadMAC[] and use ESP-NOW mode

### Test Sketches
- `AS5600_test.ino` — verify encoder wiring and magnet placement
- `ST7789_UI_test.ino` — verify display wiring and UI rendering
- `button_test.ino` — verify button wiring and GPIO states
- `display_test_minimal.ino` — minimal display sanity check

## License
CC BY-NC 4.0 — Sai Prashanth
