# MacroPad v5 — Pin Reference

## ESP32 WROOM-32 GPIO Allocation

| GPIO | Board Label | Function | Direction | Notes |
|------|------------|----------|-----------|-------|
| 13 | P13 | Key Col 0 | OUTPUT | Drive LOW to scan |
| 14 | P14 | Key Col 1 | OUTPUT | Drive LOW to scan |
| 25 | P25 | Key Col 2 | OUTPUT | Drive LOW to scan |
| 26 | P26 | Key Row 0 | INPUT | INPUT_PULLUP |
| 32 | P32 | Key Row 1 | INPUT | INPUT_PULLUP |
| 33 | P33 | Key Row 2 | INPUT | INPUT_PULLUP |
| 34 | P34 | Key Row 3 | INPUT | Input-only, external 10kΩ pullup |
| 39 | SVN | EC12 SW   | INPUT | Input-only, external 10kΩ pullup |
| 21 | P21 | I2C SDA   | BIDIR | AS5600, 4.7kΩ pullup on breakout |
| 22 | P22 | I2C SCL   | BIDIR | AS5600, 4.7kΩ pullup on breakout |
| 23 | P23 | SPI MOSI  | OUTPUT | ST7789 SDA |
| 18 | P18 | SPI SCLK  | OUTPUT | ST7789 SCL |
| 27 | P27 | TFT CS    | OUTPUT | 10kΩ pullup to 3.3V |
| 17 | P17 | TFT DC    | OUTPUT | |
| 16 | P16 | TFT RST   | OUTPUT | |
| 19 | P19 | TFT BL    | OUTPUT | PWM via LEDC |
| VIN | 5V | Power in  | POWER | From CKCS 5V out |
| 3V3 | 3V3 | 3.3V out | POWER | To all VCC pins |

## Key Matrix (3×4 = 12 keys)

```
       Col0(13)  Col1(14)  Col2(25)
Row0(26)  SW1      SW2      SW3
Row1(32)  SW4      SW5      SW6
Row2(33)  SW7      SW8      SW9
Row3(34)  SW10     SW11     SW12
```

Each switch has a 1N4148 diode:
- Anode → switch pin 2 (row side)
- Cathode → column line

## Pullup Resistors

| Ref | Value | Pin | To |
|-----|-------|-----|----|
| R1 | 10kΩ | GPIO26 Row0 | 3.3V |
| R2 | 10kΩ | GPIO32 Row1 | 3.3V |
| R3 | 10kΩ | GPIO33 Row2 | 3.3V |
| R4 | 10kΩ | GPIO34 Row3 | 3.3V |
| R5 | 10kΩ | GPIO39 EC12 SW | 3.3V |
| R6 | 10kΩ | GPIO27 TFT CS | 3.3V |

I2C pullups (4.7kΩ) are onboard the AS5600 breakout — no external ones needed.

## Strapping Pins — Avoided

| GPIO | Issue | Solution |
|------|-------|----------|
| 0 | Boot mode | Not used |
| 2 | Boot mode | Not used |
| 12 | Flash voltage | Not used (was TFT_BL in v4, moved to GPIO19 in v5) |
| 15 | Boot log | Not used |

## Power Chain

```
USB-C socket → CKCS module USB-C pads (wired)
CKCS B+ / B- → 18650 cell
CKCS 5V out  → ESP32 VIN
ESP32 3V3    → all VCC pins
```
