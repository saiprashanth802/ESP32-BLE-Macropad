# Testing Log

## AS5600 Magnetic Encoder
- **Board**: NodeMCU ESP32S v1.1
- **Status**: ✓ Working
- **Notes**:
  - Angle reads 0–4095 correctly
  - Velocity calculation working
  - Wraparound at 0/4095 boundary handled
  - AGC value ~50–100 = good air gap
  - Diametric cylinder magnet required
  - Wire: SDA→D2(GPIO4), SCL→D1(GPIO5) on ESP8266 NodeMCU

## ST7789 240×320 Display
- **Board**: NodeMCU ESP32S v1.1
- **Module**: SmartElex 2" IPS (R257748 robu.in), driver ST7789P3
- **Status**: ✓ Working
- **Notes**:
  - 9 pins: VCC GND SDA SCL CS DC RST EN TE
  - EN = backlight enable (not BL PWM) — wire to 3.3V for always-on
  - TE = tearing effect — leave unconnected
  - No BL pin — backlight controlled via EN
  - Full 240×320 confirmed working with 4-band colour test
  - RAM too small for full sprite (153KB) — use direct drawing
  - Small sprites OK: 240×24 status bar, 72×60 key cells
  - Library: TFT_eSPI, ST7789_DRIVER
  - Pins: MOSI=23, SCLK=18, CS=27, DC=17, RST=16
  - EN: PWM via ledcAttach GPIO19 or direct 3.3V

## MX Mechanical Switches
- **Type**: Cherry MX compatible, plate mount 3-pin
- **KiCad footprint**: Button_Switch_Keyboard:SW_Cherry_MX_1.00u_PCB
- **Spacing**: 19.05mm standard MX pitch
- **Status**: PCB ordered, not yet tested on hardware

## Media Remote (ESP-NOW + BLE dual mode)
- **Status**: In development
- **Issues**:
  - BLE connection instability when WiFi (ESP-NOW) active simultaneously
  - Solution: ESP-NOW off by default, toggle in settings
  - Input-only GPIO pins (34,35,39) need external pullups — or use GPIO 25,26,27
  - Reconnected button pins to GPIO 25, 26, 27 (internal pullup capable)
