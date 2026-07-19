// TFT_eSPI User_Setup.h — MacroPad v5 (NodeMCU ESP32-S V1.1 + ST7789 240×320)
//
// Copy this file over  <Arduino sketchbook>/libraries/TFT_eSPI/User_Setup.h
// Pins match hardware/pin_reference_v5.md (P-label = GPIO number).

#define ST7789_DRIVER

#define TFT_WIDTH  240
#define TFT_HEIGHT 320

#define TFT_MOSI  23   // P23 — ST7789 SDA
#define TFT_SCLK  18   // P18 — ST7789 SCL
#define TFT_CS     5   // P5
#define TFT_DC    21   // P21
#define TFT_RST   22   // P22

// Backlight: the SKETCH drives P19 with LEDC PWM (brightness setting,
// re-attach after light-sleep). Defining TFT_BL here only makes tft.init()
// switch it on early; ledcAttach() in setup() takes the pin over afterwards.
#define TFT_BL    19   // P19
#define TFT_BL_ON HIGH

// Fonts — REQUIRED: text renders blank without LOAD_GLCD
#define LOAD_GLCD      // default 6×8 font, the only one the firmware uses
#define LOAD_FONT2
#define SMOOTH_FONT

#define SPI_FREQUENCY  40000000

// If colours look inverted on your IPS panel (white↔black), uncomment:
// #define TFT_INVERSION_ON
