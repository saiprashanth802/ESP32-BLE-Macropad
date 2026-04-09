# Pin Reference — v2 Hardware

All 8 buttons are wired directly (one GPIO per button).
No matrix scanning on this revision.

## Buttons

| Button | GPIO | Mode         | Note              |
|--------|------|--------------|-------------------|
| B1     | 14   | INPUT_PULLUP | Active LOW        |
| B2     | 13   | INPUT_PULLUP |                   |
| B3     | 26   | INPUT_PULLUP |                   |
| B4     | 25   | INPUT_PULLUP |                   |
| B5     | 22   | INPUT_PULLUP |                   |
| B6     | 21   | INPUT_PULLUP |                   |
| B7     | 35   | INPUT_PULLUP | Input-only pin    |
| B8     | 19   | INPUT_PULLUP |                   |

## Rotary Encoder (EC11)

| Signal | GPIO | Mode         |
|--------|------|--------------|
| CLK    | 2    | INPUT_PULLUP |
| DT     | 4    | INPUT_PULLUP |
| SW     | 15   | INPUT_PULLUP — LOW = pressed |

Encoder direction is reversed in firmware (`ENC_DIR = -1`).

## TFT Display (ST7735, 1.8", 128×160)

| Signal     | GPIO | Note                        |
|------------|------|-----------------------------|
| CS         | 5    |                             |
| DC         | 17   |                             |
| RST        | 16   |                             |
| BL (PWM)   | 12   | ledcAttach, 5kHz, 8-bit     |
| MOSI/SDA   | —    | SPI default (VSPI)          |
| SCK/CLK    | —    | SPI default (VSPI)          |

Configure CS/DC/RST in TFT_eSPI `User_Setup.h`.

## Power

| Component  | Detail                                      |
|------------|---------------------------------------------|
| Cell       | 18650, 2600mAh                              |
| Charger    | TP4056 / HW-107 with protection circuit     |
| Connection | JST-PH 2-pin from TP4056 OUT to ESP32 VIN  |
| Charging   | USB-C on ESP32 dev board                   |
