# MacroPad — Development Roadmap

## v3 (Complete)
- 6→8 button design
- ST7735 128×160 TN display
- ESP32 WROOM-32 bare module
- TP4056 + 18650 battery
- BLE via ESP32-BLE-Keyboard library
- Hand wired, direct GPIO

## v4 (Complete — current stable)
- 8-button layout
- Migrated to NimBLE-Arduino stack
- Fixed ghost BLE connection bug (Windows sleep/wake)
- M3 corner studs with heat-set inserts
- 1N5819 Schottky diode on VIN rail
- `macropad_v4_nimble.ino`

### v4 + ESP-NOW extension (In Testing)
- Optional media remote via AS5600 magnetic encoder
- ESP-NOW receiver built into v4 firmware
- Toggle in settings menu — saves to NVS
- Media remote: play/pause, next, prev, volume wheel
- Dual mode: ESP-NOW (relay via macropad) or direct BLE HID
- Velocity-accelerated smooth scrolling
- Adjustable sensitivity saved to NVS

## v5 (In Development)

### Hardware
- ESP32 WROOM-32 dev board (NodeMCU-32S)
- ST7789 240×320 IPS display (30.6×40.8mm active area)
- 3×4 MX mechanical switch matrix with 1N4148 diodes
- AS5600 magnetic encoder on custom bearing mount (50mm dial)
- EC12 encoder (button only — push switch)
- CKCS charge+boost module
- 18650 cell
- Custom copper-tape ridge substrate PCB
- 3D printed enclosure: 115×95×30mm body + encoder lobe

### Enclosure Design
- Landscape orientation
- Main body: 115×95mm
- Encoder dial lobe: half-proud right edge, 50mm diameter, 25mm proud
- Screen flush-mounted: 31.0×41.2mm glass window, 35.2×48.4mm module ledge
- Bottom cover: M3 screws into brass inserts, 4× corners
- Component cavity on underside: ESP32, CKCS, 18650

### PCB
- Copper tape ridge substrate (top face) — key matrix traces
- KiCad schematic complete
- Gerbers exported
- Ordered from JLCPCB

### Firmware (Planned)
- v5 modular architecture: config.h, types.h, matrix.h, encoders.h, display.h
- NimBLE-Arduino BLE stack
- 4×3 matrix scan with diodes (n-key rollover)
- AS5600 velocity-scaled smooth scrolling
- Settings menu via 12-key grid (each key = one setting)
- Full-screen editors per setting
- NVS persistence for all settings and custom presets
- Direct TFT drawing (no full-screen sprite — RAM constraint)
- Small sprites: 240×24 status bar, 72×60 key cells

### GPIO Allocation (v5)
- Cols: GPIO 13, 14, 25
- Rows: GPIO 26, 32, 33, 34
- EC12 SW: GPIO 39
- AS5600 SDA/SCL: GPIO 21, 22
- ST7789: MOSI=23, SCLK=18, CS=27, DC=17, RST=16, BL=19
- Zero strapping pins used

## Key Learnings

- **NimBLE vs ESP32-BLE-Keyboard**: NimBLE with GAP callbacks is required for reliable connect/disconnect tracking. The Arduino BLE stack returns false positives after Windows sleep/wake.
- **GPIO 12 strapping pin**: Must read LOW at boot. Used as TFT backlight in v4 — requires INPUT_PULLDOWN and active-HIGH wiring. Avoided entirely in v5.
- **LEDC + BLE init order**: BLE radio init resets LEDC peripheral. Always call `ledcAttach()` AFTER `NimBLEDevice::init()` — otherwise PWM backlight control is lost.
- **WiFi + BLE coexistence**: ESP-NOW (WiFi) and NimBLE (BT) share the antenna but use different protocols. Coexist but cause connection instability at high packet rates. Mitigated by making ESP-NOW optional (off by default).
- **ST7789 RAM constraint**: Full 240×320 16-bit sprite = 153KB — too large for WROOM-32 with BLE active. Solution: direct TFT drawing with small reusable sprites (status bar 240×24, key cell 72×60).
- **Input-only GPIOs**: GPIO 34, 35, 36, 39 have no internal pullups. Always use external 10kΩ resistors on these pins.
- **NVS via Preferences**: Use for all persistent settings — survives power cycles and firmware updates (unless flash is erased).
