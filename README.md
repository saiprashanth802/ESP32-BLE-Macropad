# ESP32 BLE MacroPad

> **v4 hardware + v4.2 firmware** — fully assembled, battery-powered, wireless BLE macro pad with 8 buttons, rotary encoder, 1.8" TFT display, M3 screw-mount enclosure, isolated charging circuit, and NimBLE-based fast pairing.

[![License: CC BY-NC 4.0](https://img.shields.io/badge/License-CC%20BY--NC%204.0-lightgrey.svg)](https://creativecommons.org/licenses/by-nc/4.0/)

---

## 📸 Build Photos

| Assembled | Internals |
|:---:|:---:|
| ![Assembled](images/assembled.jpg) | ![Internals](images/internals.jpg) |

| CAD — Body | CAD — Encoder Knob |
|:---:|:---:|
| ![CAD Body](images/cad_body.png) | ![CAD Knob](images/cad_knob.png) |

---

## ✨ Features

### Firmware (v4.2 — NimBLE)
- **8-button layout** — 4×2 grid with custom FDM printed keycaps
- **8 presets** — OnShape · KiCad · Music · Gaming · LTspice · Custom ×3
- **BUILD MODE** — remap any button live, no reflashing (hold encoder 1.5s)
- **60+ actions** — system shortcuts, media, edit, CAD, PCB, browser, gaming, LTspice
- **Rotary encoder** — 4 modes: VOL / SCROLL / ZOOM / ALT-TAB
- **1.8" ST7735 TFT** — double-buffered sprite UI, per-preset accent colours
- **NimBLE HID stack** — clean pairing on Windows and Android, no ghost connections
- **Wake-key buffer** — press a button to wake; keystroke fires automatically once connected
- **Reconnect HUD** — animated "RECONNECTING…" screen with queued key shown while waiting
- **Reliable multi-cycle sleep** — GPIO wakeup sources cleared between cycles, LEDC re-attached on every wake
- **Settings menu** — brightness, sleep timeout, encoder sensitivity (hold encoder 0.7s)
- **RTOS light-sleep** — 3–4 months typical desk use per charge

### Hardware (v4)
- **M3 screw-mount enclosure** — four corner studs with M3×4 heat-set inserts, M3×6 SHCS lid screws
- **Isolated charging circuit** — 1N5819 Schottky diode on VIN rail prevents backfeed between ESP32 USB and TP4056 charger
- **2600mAh 18650 Li-Ion** with TP4056/HW-107 protection module

---

## 🕹 Controls

| Action | What it does |
|---|---|
| Short press encoder | Lock / unlock mode |
| Hold encoder 0.7s | Open settings menu |
| Hold encoder 1.5s | Enter BUILD MODE |
| Turn encoder (unlocked) | Cycle encoder mode |
| Turn encoder (locked) | Control active mode (vol / scroll / zoom / alt-tab) |
| Press button (unlocked) | Switch to that preset |
| Press button (locked) | Fire assigned macro |

---

## 🎹 Default Presets

### OnShape (8 buttons)
| Fit | Front | Top | Right |
|---|---|---|---|
| Iso | Extrude | Sketch | Mate |

### KiCad (8 buttons)
| Route | ZFit | DRC | 3D View |
|---|---|---|---|
| Copper | Gerber | Ratsnest | Add Net |

### Music (6 buttons)
| Play/Pause | Next | Prev | Mute |
|---|---|---|---|
| Spotify | Min All | — | — |

### Gaming (8 buttons)
| PTT | Reload | Map | Scoreboard |
|---|---|---|---|
| Fullscreen | OBS Rec | OBS Stream | Discord |

### LTspice (8 buttons)
| Move | GND | VCC | Resistor |
|---|---|---|---|
| Capacitor | Add Component | Wire | Run Sim |

### Custom 1–3
Fully remappable via BUILD MODE.

---

## 🔌 Pin Reference

| Function | GPIO |
|---|---|
| Button 1–8 | 14, 13, 26, 25, 22, 21, 35, 19 |
| Encoder CLK | 2 |
| Encoder DT | 4 |
| Encoder SW | 15 |
| TFT CS | 5 |
| TFT DC | 17 |
| TFT RST | 16 |
| TFT Backlight (PWM) | 12 |

All buttons INPUT_PULLUP, active LOW. See [`hardware/pin_reference.md`](hardware/pin_reference.md) for full v4 power circuit details.

---

## ⚡ Charging Circuit (v4)

v4 adds a 1N5819 Schottky diode in series on the VIN rail between the ESP32 board and the TP4056 charger's 5V input. This prevents the ESP32's onboard USB supply from backfeeding into the charger when USB is connected.

```
USB-C ──► ESP32 VIN ──► [1N5819] ──► TP4056 5V IN ──► 18650 cell
```

See [`hardware/pin_reference.md`](hardware/pin_reference.md) for full topology and component specs.

---

## 📦 Dependencies (Arduino IDE)

| Library | Author | Install |
|---|---|---|
| [NimBLE-Arduino](https://github.com/h2zero/NimBLE-Arduino) | h2zero | Library Manager |
| [TFT_eSPI](https://github.com/Bodmer/TFT_eSPI) | Bodmer | Library Manager |
| Arduino core for ESP32 | Espressif | Boards Manager |

> ⚠️ v4.2 uses **NimBLE-Arduino**, not ESP32-BLE-Keyboard. If you previously had BLE-Keyboard installed, remove it first to avoid conflicts.

### First flash — erase flash before uploading
Tools → **Erase All Flash Before Sketch Upload** → Enabled

This clears any stale bond data from previous firmware. Disable it again after the first flash.

### TFT_eSPI `User_Setup.h`

```cpp
#define ST7735_DRIVER
#define TFT_CS   5
#define TFT_DC  17
#define TFT_RST 16
#define TFT_WIDTH  128
#define TFT_HEIGHT 160
```

---

## 🖨 CAD & Enclosure

- Modelled in **Onshape** — browser-based, free for public projects
- Printed in **PLA**, 0.2mm layer height, 3 walls, 20% gyroid infill
- Custom square keycaps — press-fit over tact switches
- Skull encoder knob — press-fit on EC11 D-shaft
- **v4 enclosure** — M3×4 heat-set inserts in corner studs, M3×6 SHCS lid screws

---

## 🔋 Power

| Component | Detail |
|---|---|
| Cell | 18650 Li-Ion, 2600mAh |
| Charger | TP4056 (HW-107) with DW01A protection |
| Isolation diode | 1N5819 Schottky on VIN rail |
| Estimated battery life | **3–4 months** typical desk use with RTOS light-sleep |

---

## 🗂 Repo Structure

```
ESP32-BLE-MacroPad/
├── firmware/
│   └── macropad_v4_nimble.ino  # v4.2 — NimBLE HID, clean pairing
├── hardware/
│   └── pin_reference.md        # GPIO table + v4 power circuit
├── cad/
│   └── README.md               # Print settings, Onshape notes
├── images/                     # Build photos
└── README.md
```

---

## 🗺 Roadmap

- [x] v1 — 6-button prototype on perf board, basic firmware
- [x] v2 — 8-button build, FDM enclosure, battery, custom keycaps + skull knob
- [x] v3 firmware — 8 presets, BUILD MODE, 60+ actions, settings menu, sleep
- [x] v4 hardware — M3 screw-mount enclosure, isolated charging circuit (1N5819 diode)
- [x] v4 firmware — wake-key buffer, reconnect HUD, reliable multi-cycle sleep
- [x] v4.2 firmware — migrated to NimBLE, clean pairing on Windows + Android, no ghost connections
- [ ] v5 — custom KiCad PCB, tighter CAD tolerances, snap-fit lid

---

## 🪪 License

**CC BY-NC 4.0** — Free to use and modify for personal/non-commercial purposes only.
Commercial use is not permitted without explicit written permission from the author.

Full license: [creativecommons.org/licenses/by-nc/4.0](https://creativecommons.org/licenses/by-nc/4.0/)

---

## 🙌 Credits

Built by **Sai Prashanth**
CAD in Onshape · Firmware in Arduino C++ · Hand-wired on perf board

🔗 GitHub: [github.com/saiprashanth802/ESP32-BLE-Macropad](https://github.com/saiprashanth802/ESP32-BLE-Macropad)
