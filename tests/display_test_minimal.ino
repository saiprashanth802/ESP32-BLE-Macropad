/*
 * Minimal display test — no BLE, no ESP-NOW
 * Just TFT to confirm hardware is working
 * 
 * If this works, the issue is BLE init order
 * If this doesn't work, it's a wiring/config issue
 */

#include <TFT_eSPI.h>
#include <SPI.h>

#define TFT_BL 12

TFT_eSPI    tft = TFT_eSPI();
TFT_eSprite spr = TFT_eSprite(&tft);

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("1 - Serial OK");

  // Backlight
  ledcAttach(TFT_BL, 5000, 8);
  ledcWrite(TFT_BL, 200);
  Serial.println("2 - Backlight set");

  // TFT
  tft.init();
  tft.setRotation(2);
  Serial.println("3 - TFT init done");

  spr.setColorDepth(16);
  spr.createSprite(128, 160);
  Serial.println("4 - Sprite created");

  // Draw
  spr.fillSprite(0x0841);
  spr.setTextColor(0x07FF, 0x0841);
  spr.setTextSize(2);
  spr.setCursor(10, 40);
  spr.print("HELLO");
  spr.setCursor(10, 65);
  spr.print("WORLD");
  spr.setTextSize(1);
  spr.setTextColor(0xFD20, 0x0841);
  spr.setCursor(10, 100);
  spr.print("Display working!");
  spr.pushSprite(0, 0);
  Serial.println("5 - Sprite pushed");
}

void loop() {
  delay(1000);
}
