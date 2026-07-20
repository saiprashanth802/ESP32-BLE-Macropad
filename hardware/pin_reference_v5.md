# MacroPad v5 — Pin Reference (multihost wiring)

Board: **NodeMCU ESP32-S V1.1** (WROOM-32, no PSRAM). P-label = GPIO number (P18 = GPIO18).
13 pins used: TFT SPI (6) + 4×3 matrix (7). The AS5600 encoder is now a
**separate wireless puck** (ESP12-E + ESP-NOW) — no I2C or encoder switch on
the main board anymore.

## ESP32 GPIO Allocation

| GPIO | Board Label | Function | Direction | Notes |
|------|------------|-----------|-----------|-------|
| 18 | P18 | TFT SCK  | OUTPUT | SPI clock |
| 23 | P23 | TFT MOSI | OUTPUT | SPI data (ST7789 SDA) |
| 5  | P5  | TFT CS   | OUTPUT | HIGH at boot — fine |
| 21 | P21 | TFT DC   | OUTPUT | |
| 22 | P22 | TFT RST  | OUTPUT | |
| 19 | P19 | TFT BL   | OUTPUT | PWM via LEDC (sketch-driven) |
| 25 | P25 | Matrix Row 1 (top) | OUTPUT | driven LOW one at a time |
| 32 | P32 | Matrix Row 2 (mid) | OUTPUT | driven LOW one at a time |
| 33 | P33 | Matrix Row 3 (bot) | OUTPUT | driven LOW one at a time |
| 26 | P26 | Matrix Col 1 | INPUT | INPUT_PULLUP |
| 14 | P14 | Matrix Col 2 | INPUT | INPUT_PULLUP, JTAG-shared (fine) |
| 27 | P27 | Matrix Col 3 | INPUT | INPUT_PULLUP |
| 13 | P13 | Matrix Col 4 | INPUT | INPUT_PULLUP, JTAG-shared (fine) |
| VIN | 5V | Power in  | POWER | From CKCS 5V out |
| 3V3 | 3V3 | 3.3V out | POWER | To TFT VCC |

All matrix inputs use internal pullups — no external resistors needed
(GPIO 34/36/39 input-only pins are no longer used).

## Key Matrix (3 rows × 4 cols = 12 keys) — AS BUILT

**Verified on hardware 2026-07-20 with a pairwise conduction probe** — this
supersedes every earlier pin grouping. The 3 physical rows are the driven
lines; the 4 physical columns are read with `INPUT_PULLUP`.

```
           Col1(26)  Col2(14)  Col3(27)  Col4(13)
Row1(25)     K1        K2        K3        K4
Row2(32)     K5        K6        K7        K8
Row3(33)     K9(FN)    K10       K11       K12
```

Logical key index in firmware: `idx = row*4 + col` — identical to the
landscape grid the display shows. K9 (bottom-left) = FN.

### Diode orientation (verified)

Each switch has a 1N4148 with the **cathode (band) toward the ROW line**
(P25/P32/P33); current flows column → switch → row. The firmware's
row-driven scan matches this as-built orientation.

## Strapping Pins — Avoided

| GPIO | Issue | Status |
|------|-------|--------|
| 0, 2, 12, 15 | Boot mode / flash voltage | **Not used at all** |

If your board has PSRAM, GPIO 16/17 are reserved — this pinout avoids them
too (standard NodeMCU-32S WROOM-32 has no PSRAM).

## TFT_eSPI configuration

Copy `firmware/v5/User_Setup.h` over
`<Arduino sketchbook>/libraries/TFT_eSPI/User_Setup.h`. It contains the pins
above, `ST7789_DRIVER`, 40MHz SPI and the font loads (`LOAD_GLCD` is
mandatory — text renders blank without it).

## Flashing this specific board (bench NodeMCU-32S, MAC 84:1F:E8:2B:33:48)

Its Boya flash chip (mfr 0x68) **cannot run at the default 80MHz flash clock**
— the 2nd-stage bootloader crash-loops printing only `entry 0x4008059c`.
In Arduino IDE set:

- **Tools → Flash Frequency → 40MHz**  (mandatory)
- **Tools → Upload Speed → 115200**  (921600 aborts with
  "Packet content transfer stopped" on this board/cable)

Also disconnect the display while flashing, and power it from **3V3, never
5V** — an overdriven backlight sags the rail enough to kill flash writes.

## Power Chain

```
USB-C socket → CKCS module USB-C pads (wired)
CKCS B+ / B- → 18650 cell
CKCS 5V out  → ESP32 VIN
ESP32 3V3    → TFT VCC
```
