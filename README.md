# ESP32 BLE HID MacroPad

> An open-source, battery-powered wireless BLE HID macro pad built on ESP32 with NimBLE stack. Hand-wired with a custom FDM-printed enclosure, fully self-contained shortcut controller with an on-device UI — no companion app required.

[![License: CC BY-NC 4.0](https://img.shields.io/badge/License-CC%20BY--NC%204.0-lightgrey.svg)](https://creativecommons.org/licenses/by-nc/4.0/)

---

## Versions

| Version | Status | Description |
|---------|--------|-------------|
| v3 | ✅ Complete | 8-button, ST7735 128×160, ESP32-BLE-Keyboard — flagship build |
| v4 | ✅ Stable | NimBLE stack, ghost connection fix, BUILD MODE |
| v4 + Media Remote | 🔬 Testing | Wireless AS5600 scroll wheel via ESP-NOW |
| v5 | 🚧 In Development | 12-key MX matrix, ST7789 240×320, AS5600 encoder, custom PCB |

---

## v3 — Flagship Build

> Primary portfolio entry. 8-button macro pad with ST7735 display, NimBLE BLE HID, and on-device preset switching with BUILD MODE.

### Build Photos

| | |
|---|---|
| ![Assembled](images/assembled.jpg) | ![Internals](images/internals.jpg) |
| Assembled v3 | Internal wiring |

| | |
|---|---|
| ![CAD Body](images/cad_body.png) | ![CAD Knob](images/cad_knob.png) |
| Enclosure CAD | Encoder knob CAD |

### Hardware
- ESP32 WROOM-32
- ST7735 128×160 TN display
- 8× tactile switches in 4×2 layout
- EC11 rotary encoder + custom skull knob
- TP4056 + 18650 2600mAh battery
- M3 corner studs with heat-set inserts
- Hand-wired, FDM printed enclosure (PLA, 0.2mm, 20% gyroid)

### Features
- 8 presets: OnShape, KiCad, Music, Gaming, LTspice, SYS, DEV, EDIT
- BUILD MODE — remap any button live (hold encoder 1.5s)
- Encoder modes: VOL / SCROLL / ZOOM / ALT-TAB
- Settings menu: brightness, sleep timeout, sensitivity, OS layout
- OS Layout: Windows / Linux — remaps shortcuts live
- RTOS light-sleep with GPIO wakeup
- Wake-key buffer + reconnect HUD
- BLE bond clear (hold encoder 3s)
- Estimated battery life: 3–4 months typical desk use

### GPIO Allocation (v3/v4)

| GPIO | Function |
|------|----------|
| 14,13,26,25,22,21,35,19 | 8× buttons |
| 2, 4 | Encoder CLK, DT |
| 15 | Encoder SW |
| 5, 17, 16, 12 | TFT CS, DC, RST, BL |
| 23, 18 | SPI MOSI, SCLK |

---

## v4 — Current Stable

> Migrated to NimBLE-Arduino to fix persistent BLE ghost-connection bug on Windows sleep/wake cycles.

### Key Changes from v3
- NimBLE-Arduino replaces ESP32-BLE-Keyboard
- GAP-layer callbacks for reliable connect/disconnect tracking
- 1N5819 Schottky diode on VIN rail isolating charger from USB supply
- `esp_sleep_disable_wakeup_source()` before re-arming GPIO wakeup each cycle
- `ledcAttach()` called on every wake to restore PWM backlight
- Firmware: `macropad_v4_nimble_espnow.ino`

### TFT_eSPI User_Setup.h

```cpp
#define ST7735_DRIVER
#define TFT_CS   5
#define TFT_DC   17
#define TFT_RST  16
#define TFT_WIDTH  128
#define TFT_HEIGHT 160
#define TFT_MOSI 23
#define TFT_SCLK 18
```

---

## Media Remote — Test Phase

> Standalone wireless scroll wheel using AS5600 magnetic encoder. Connects to v4 macropad via ESP-NOW or directly to PC via BLE HID. Designed for high-resolution smooth scrolling with velocity-based acceleration.

### Hardware
- ESP32 NodeMCU dev board
- AS5600 magnetic rotary encoder (I2C, 12-bit resolution, 4096 steps/rev)
- Custom 3D printed enclosure with bearing-mounted 50mm scroll wheel
- Diametrically magnetized cylinder magnet on axle
- 3× tactile buttons — Play/Pause, Next, Prev

### Design & Prototype

| | |
|---|---|
| ![Enclosure CAD](images/media_remote/enclosure_cad.png) | ![Scroll wheel CAD](images/media_remote/scroll_wheel_cad.png) |
| Enclosure CAD | Scroll wheel with bearing mount CAD |

![Media remote prototype](images/media_remote/prototype_assembled.jpeg)

*Prototype — printed enclosure with 50mm scroll wheel, AS5600 underneath, wired to NodeMCU ESP32S*

### Features
- Velocity-accelerated volume control — slow turn = fine steps, fast spin = large jumps
- Adjustable sensitivity 1–10, saved to NVS
- Hold Btn1 + turn wheel = adjust sensitivity live
- Hold Btn1 + Btn3 for 1.5s = toggle ESP-NOW ↔ BLE mode
- Mode and sensitivity persist across power cycles
- LED blink feedback: 1 blink = ESP-NOW, 3 blinks = BLE

### Modes

| Mode | Description |
|------|-------------|
| BLE HID | Connects directly to PC as "MediaRemote" |
| ESP-NOW | Sends to v4 macropad which relays via BLE to PC |

### Wiring

| Pin | GPIO | Function |
|-----|------|----------|
| AS5600 SDA | 21 | I2C data |
| AS5600 SCL | 22 | I2C clock |
| BTN1 Play/Pause | 27 | INPUT_PULLUP → GND |
| BTN2 Next | 26 | INPUT_PULLUP → GND |
| BTN3 Prev | 25 | INPUT_PULLUP → GND |
| LED | 2 | Onboard LED |

---

## v5 — In Development

> Full redesign. 12-key MX mechanical switch matrix, ST7789 240×320 IPS display, AS5600 magnetic encoder on custom bearing mount, and custom copper-tape ridge substrate PCB.

### Hardware Spec
- ESP32 WROOM-32 dev board
- ST7789 240×320 IPS display, glass flush-mounted (30.6×40.8mm active area)
- 12× MX compatible mechanical switches, plate mount, 3×4 matrix, 19.05mm pitch
- 12× 1N4148 diodes for n-key rollover
- AS5600 + 50mm bearing-mounted scroll dial
- EC12 push button (button only — AS5600 handles rotation)
- CKCS charge+boost module + 18650 cell
- Custom copper-tape ridge substrate PCB

### Enclosure
- 115×95×30mm main body
- Encoder dial lobe: half-proud right edge, 50mm diameter, 25mm proud (XP-Pen style)
- Screen: 31.0×41.2mm flush glass window, 35.2×48.4mm module ledge 2.1mm deep
- Bottom cover: M3 screws into brass heat-set inserts, 4× corners

### PCB Technique — Copper Tape Ridge Substrate
3D printed plate with elevated ridges as trace isolation guides. A single copper sheet is laid over the entire surface — the ridges act as cutting guides. Scalpel-cut along ridge edges isolates traces. Looks like a real PCB, built from scratch.

### v5 GPIO Allocation

| GPIO | Board Label | Function | Notes |
|------|------------|----------|-------|
| 13 | P13 | Key Col 0 | Drive LOW to scan |
| 14 | P14 | Key Col 1 | Drive LOW to scan |
| 25 | P25 | Key Col 2 | Drive LOW to scan |
| 26 | P26 | Key Row 0 | INPUT_PULLUP |
| 32 | P32 | Key Row 1 | INPUT_PULLUP |
| 33 | P33 | Key Row 2 | INPUT_PULLUP |
| 34 | P34 | Key Row 3 | External 10kΩ pullup |
| 39 | SVN | EC12 SW | External 10kΩ pullup |
| 21 | P21 | I2C SDA | AS5600 |
| 22 | P22 | I2C SCL | AS5600 |
| 23 | P23 | SPI MOSI | ST7789 SDA |
| 18 | P18 | SPI SCLK | ST7789 SCL |
| 27 | P27 | TFT CS | 10kΩ pullup to 3.3V |
| 17 | P17 | TFT DC | |
| 16 | P16 | TFT RST | |
| 19 | P19 | TFT BL | PWM via LEDC |

> Zero strapping pins used — clean boot every time, no pull resistors needed for boot.

---

## Repository Structure

```
ESP32-BLE-MacroPad/
├── firmware/
│   ├── v4/
│   │   └── macropad_v4_nimble_espnow.ino  — v4 + optional ESP-NOW
│   ├── v5/
│   │   └── macropad_v5.ino                — v5 in development
│   ├── media_remote/
│   │   ├── media_remote.ino               — BLE + ESP-NOW dual mode
│   │   └── media_remote_dual_mode.ino     — switchable mode version
│   └── tests/
│       ├── AS5600_test.ino                — encoder verification
│       ├── ST7789_UI_test.ino             — display UI test
│       ├── button_test.ino                — button GPIO test
│       └── display_test_minimal.ino       — minimal display check
├── hardware/
│   ├── pin_reference_v5.md               — complete v5 GPIO table
│   └── ESP32_DevKitC_v4.kicad_mod        — custom KiCad footprint
├── images/
│   ├── assembled.jpg                      — v3 assembled
│   ├── internals.jpg                      — v3 internals
│   ├── cad_body.png                       — v3 enclosure CAD
│   ├── cad_knob.png                       — v3 encoder knob CAD
│   └── media_remote/
│       ├── enclosure_cad.png              — media remote enclosure CAD
│       ├── scroll_wheel_cad.png           — scroll wheel CAD
│       └── prototype_assembled.jpeg       — built prototype
└── README.md
```

---

## Dependencies

| Library | Purpose |
|---------|---------|
| NimBLE-Arduino | BLE HID stack |
| TFT_eSPI | Display driver |
| Preferences | NVS persistent storage |
| Wire | I2C for AS5600 |
| esp_now | ESP-NOW for media remote |

---

## Key Learnings

- **NimBLE vs ESP32-BLE-Keyboard** — NimBLE with GAP callbacks is required for reliable connect/disconnect tracking. The Arduino BLE stack returns false positives after Windows sleep/wake.
- **GPIO 12 strapping pin** — Must read LOW at boot. Avoided entirely in v5 by moving TFT backlight to GPIO 19.
- **LEDC + BLE init order** — BLE radio init resets the LEDC peripheral. Always call `ledcAttach()` after `NimBLEDevice::init()`.
- **WiFi + BLE coexistence** — ESP-NOW and NimBLE share the antenna. Made optional in v4 (toggle in settings) to avoid connection instability.
- **ST7789 RAM constraint** — Full 240×320 16-bit sprite = 153KB, too large for WROOM-32 with BLE active. Solution: direct TFT drawing with small reusable sprites.
- **Input-only GPIOs** — GPIO 34, 35, 36, 39 have no internal pullups. Always use external 10kΩ resistors.

---

## License

**CC BY-NC 4.0** — Free to use and modify for personal and non-commercial purposes only. Commercial use requires explicit written permission from the author.
