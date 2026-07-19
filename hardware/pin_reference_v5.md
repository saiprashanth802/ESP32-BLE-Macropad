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
| 25 | P25 | Matrix Row 1 | OUTPUT | driven LOW one at a time |
| 26 | P26 | Matrix Row 2 | OUTPUT | driven LOW one at a time |
| 27 | P27 | Matrix Row 3 | OUTPUT | driven LOW one at a time |
| 32 | P32 | Matrix Row 4 | OUTPUT | driven LOW one at a time |
| 33 | P33 | Matrix Col 1 | INPUT | INPUT_PULLUP |
| 13 | P13 | Matrix Col 2 | INPUT | INPUT_PULLUP, JTAG-shared (fine) |
| 14 | P14 | Matrix Col 3 | INPUT | INPUT_PULLUP, JTAG-shared (fine) |
| VIN | 5V | Power in  | POWER | From CKCS 5V out |
| 3V3 | 3V3 | 3.3V out | POWER | To TFT VCC |

All matrix inputs use internal pullups — no external resistors needed
(GPIO 34/36/39 input-only pins are no longer used).

## Key Matrix (4 rows × 3 cols electrical = 12 keys)

Rows are scanned (driven LOW one at a time, hi-Z otherwise); columns are read
with `INPUT_PULLUP`.

```
          Col1(33)  Col2(13)  Col3(14)
Row1(25)    K1        K5        K9(FN)
Row2(26)    K2        K6        K10
Row3(27)    K3        K7        K11
Row4(32)    K4        K8        K12
```

Logical key index in firmware: `idx = col*4 + row`, giving the landscape
4-wide × 3-tall grid the display shows:

```
K1  K2  K3  K4
K5  K6  K7  K8
K9  K10 K11 K12      K9 = FN (bottom-left)
```

So electrical **rows = physical columns** (left→right) and electrical
**cols = physical rows** (top→bottom) on the landscape board.

### Diode orientation — CHANGED from the old col-driven scan

Each switch gets a 1N4148. Current must flow **column → switch → row**
(pullup source → driven-LOW sink):

- **Cathode (band) → row line** (P25/P26/P27/P32)
- Anode → switch pin on the column side

> ⚠️ Older revisions of this doc said cathode→column — that was for the old
> column-driven scan. If diodes are soldered the old way, either flip them or
> swap the row/column pin groups in `macropad_v5.ino`.

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
