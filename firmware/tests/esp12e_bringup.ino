/*********************************************************************
 * ESP-12E bring-up test — bare module, no dev kit
 *
 * Purpose: prove the programming jig works before any dial code exists.
 * Verifies, in order: flashing, serial, power stability, and prints the
 * MAC address (needed if this module is ever used as a paired ESP-NOW peer).
 *
 * See docs/DIAL_SATELLITE.md for where this module fits in the design.
 *
 * ── Jig wiring (matches CONTEXT.md §2 boot-pin biasing) ─────────────
 *   VCC    → 3.3V regulated  (AMS1117-3.3 off the adapter's 5V —
 *                             NOT the adapter's 3.3V pin, it browns out)
 *   GND    → GND (common with the USB-TTL adapter)
 *   EN     → 10k → 3.3V
 *   RST    → 10k → 3.3V, + button to GND
 *   GPIO0  → 10k → 3.3V, + button/jumper to GND   (LOW at boot = flash)
 *   GPIO2  → 10k → 3.3V
 *   GPIO15 → 10k → GND
 *   TX     → adapter RX
 *   RX     → adapter TX        (adapter MUST be 3.3V logic)
 *   Decoupling: 10uF + 100nF across VCC/GND, close to the module.
 *
 * ── Serial adapter: two options ─────────────────────────────────────
 *   NOTE: the ESP8266 flashes over UART ONLY, via its ROM bootloader.
 *   There is no SWD/JTAG upload path — an ST-Link cannot program it, and
 *   the module's MOSI/MISO/SCLK/CS pads are the private flash bus between
 *   the ESP8266 and its own flash chip, not a debug port.
 *
 *   (A) A USB-TTL adapter (CH340 / CP2102 / FT232RL) set to 3.3V logic.
 *
 *   (B) A NodeMCU ESP32S used as the bridge — no extra hardware, and its
 *       onboard regulator powers the ESP-12E properly:
 *         NodeMCU EN  → GND    (holds the ESP32 in reset so it releases
 *                               GPIO1/GPIO3 — without this both chips
 *                               fight over the serial lines)
 *         NodeMCU RX0 → ESP-12E RX
 *         NodeMCU TX0 → ESP-12E TX
 *         NodeMCU GND → ESP-12E GND
 *         NodeMCU 3V3 → ESP-12E VCC
 *       RX→RX and TX→TX is correct, not a typo: the header silkscreen is
 *       named from the ESP32's point of view, and the ESP32 has just been
 *       taken out of the circuit. The pin marked RX0 carries the USB
 *       chip's TX output.
 *
 * ── Flash procedure ─────────────────────────────────────────────────
 *   Hold GPIO0 low → tap RST → release GPIO0 → upload.
 *
 * ── Arduino IDE ─────────────────────────────────────────────────────
 *   Board: Generic ESP8266 Module | Flash Mode: DIO | Crystal: 26MHz
 *   Flash Size: 4MB (FS:2MB OTA:~1019KB) | Reset Method: no dtr (aka ck)
 *   Upload Speed: 115200
 *
 * ── Expected result ─────────────────────────────────────────────────
 *   Serial at 115200 prints a banner and a heartbeat line every second,
 *   with the blue onboard LED (GPIO2) blinking in step.
 *   Reset reason must read "Power on" or "External System" — anything
 *   mentioning WDT or Exception means the supply is sagging, not that
 *   the sketch is wrong. Fix power before believing any other symptom.
 *********************************************************************/

#include <ESP8266WiFi.h>

// GPIO2 carries the onboard blue LED on most ESP-12E modules, active LOW.
// It is also a boot-strapping pin and must be HIGH at reset — driving it
// only after boot is safe, which is why this is set up in setup(), not before.
#define LED_PIN 2

unsigned long lastBeat = 0;
uint32_t      beats    = 0;

void setup() {
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, HIGH);   // active LOW → start off

  Serial.begin(115200);
  delay(200);
  Serial.println();
  Serial.println(F("================================================"));
  Serial.println(F("  ESP-12E bring-up test"));
  Serial.println(F("================================================"));

  // Radio off for the baseline test: this isolates "can I flash and run"
  // from "can my supply handle a 300mA TX spike". Turn it on in step 2.
  WiFi.mode(WIFI_OFF);
  WiFi.forceSleepBegin();
  delay(100);

  Serial.print(F("Chip ID     : 0x"));  Serial.println(ESP.getChipId(), HEX);
  Serial.print(F("Flash size  : "));    Serial.print(ESP.getFlashChipRealSize() / 1024);
                                        Serial.println(F(" KB (real)"));
  Serial.print(F("Sketch space: "));    Serial.print(ESP.getFreeSketchSpace() / 1024);
                                        Serial.println(F(" KB"));
  Serial.print(F("Free heap   : "));    Serial.println(ESP.getFreeHeap());
  Serial.print(F("SDK version : "));    Serial.println(ESP.getSdkVersion());
  Serial.print(F("Reset reason: "));    Serial.println(ESP.getResetReason());

  // STA MAC is what an ESP-NOW peer would be addressed by. Printed here so
  // the number exists on record before it is ever needed.
  WiFi.mode(WIFI_STA);
  Serial.print(F("STA MAC     : "));    Serial.println(WiFi.macAddress());
  WiFi.mode(WIFI_OFF);
  WiFi.forceSleepBegin();

  Serial.println(F("------------------------------------------------"));
  Serial.println(F("Heartbeat follows. Steady 1 Hz = jig is good."));
  Serial.println(F("Gaps, resets or garbage = power, not software."));
  Serial.println();
}

void loop() {
  unsigned long now = millis();
  if (now - lastBeat >= 1000) {
    lastBeat = now;
    beats++;

    digitalWrite(LED_PIN, LOW);    // LED on
    delay(40);
    digitalWrite(LED_PIN, HIGH);   // LED off

    Serial.print(F("beat "));
    Serial.print(beats);
    Serial.print(F("  uptime "));
    Serial.print(now / 1000);
    Serial.print(F("s  heap "));
    Serial.println(ESP.getFreeHeap());
  }
}
