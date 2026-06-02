/*********************************************************************
 * AS5600 Encoder — ESP-NOW Sender
 * 
 * Hardware (NodeMCU ESP32S or any ESP32):
 *   AS5600 SDA → GPIO21
 *   AS5600 SCL → GPIO22
 *   Button     → GPIO39 (SVN) + 10kΩ pullup to 3.3V
 *
 * Sends scroll ticks + button events to MacroPad v4 via ESP-NOW
 * 
 * SETUP:
 *   1. Flash this to the encoder ESP32
 *   2. Open Serial Monitor at 115200
 *   3. Note the MAC address printed on boot
 *   4. Paste that MAC into RECEIVER_MAC in the v4 firmware patch
 *
 * Libraries needed:
 *   - Wire (built-in)
 *   - esp_now (built-in ESP32)
 *   - WiFi (built-in ESP32)
 *********************************************************************/

#include <Wire.h>
#include <esp_now.h>
#include <WiFi.h>

// ── Pins ─────────────────────────────────────────────
#define AS5600_SDA   21
#define AS5600_SCL   22
#define BTN_PIN      39   // active LOW with pullup

// ── AS5600 ───────────────────────────────────────────
#define AS5600_ADDR  0x36
#define REG_ANGLE_H  0x0E
#define REG_ANGLE_L  0x0F
#define REG_STATUS   0x0B

// ── Tuning ───────────────────────────────────────────
const int   THRESHOLD     = 20;    // raw steps per scroll tick — lower = more sensitive
const float ACCEL_FACTOR  = 2.0;   // velocity multiplier — higher = more acceleration
const int   MAX_MULT      = 10;    // max acceleration multiplier

// ── ESP-NOW ───────────────────────────────────────────
// PASTE YOUR V4 MACROPAD MAC ADDRESS HERE
// Get it by adding Serial.println(WiFi.macAddress()) to v4 setup()
uint8_t receiverMAC[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}; // replace with real MAC

// Message structure sent to v4
typedef struct {
  int8_t  scrollTicks;   // positive = scroll down, negative = scroll up
  bool    btnPressed;    // true = button just pressed
  uint8_t mode;          // current scroll mode (cycles on button press)
} EncoderMsg;

EncoderMsg msg;
esp_now_peer_info_t peerInfo;

// ── State ─────────────────────────────────────────────
int16_t  lastAngle    = 0;
int32_t  accumulator  = 0;
unsigned long lastReadMs  = 0;
unsigned long lastBtnMs   = 0;
bool     lastBtnState = false;
uint8_t  scrollMode   = 0;  // 0=scroll 1=volume 2=zoom

const char* modeNames[] = {"SCROLL", "VOLUME", "ZOOM"};

// ── ESP-NOW send callback ─────────────────────────────
void onSent(const uint8_t* mac, esp_now_send_status_t status) {
  // silent — don't spam serial
}

// ── Read AS5600 angle ─────────────────────────────────
uint16_t readAngle() {
  Wire.beginTransmission(AS5600_ADDR);
  Wire.write(REG_ANGLE_H);
  Wire.endTransmission(false);
  Wire.requestFrom(AS5600_ADDR, (uint8_t)2);
  if (Wire.available() >= 2) {
    uint16_t val = (Wire.read() & 0x0F) << 8;
    val |= Wire.read();
    return val;
  }
  return 0;
}

// ── Check magnet ──────────────────────────────────────
bool magnetOK() {
  Wire.beginTransmission(AS5600_ADDR);
  Wire.write(REG_STATUS);
  Wire.endTransmission(false);
  Wire.requestFrom(AS5600_ADDR, (uint8_t)1);
  if (Wire.available()) {
    uint8_t s = Wire.read();
    return (s & (1 << 5)); // MD bit = magnet detected
  }
  return false;
}

void setup() {
  Serial.begin(115200);
  delay(500);

  // I2C
  Wire.begin(AS5600_SDA, AS5600_SCL);
  Wire.setClock(400000);

  // Button
  pinMode(BTN_PIN, INPUT_PULLUP);

  // WiFi in station mode for ESP-NOW
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();

  Serial.println("\n\nAS5600 ESP-NOW Sender");
  Serial.println("=====================");
  Serial.print("This device MAC: ");
  Serial.println(WiFi.macAddress());
  Serial.println("Paste this MAC into RECEIVER_MAC in v4 firmware\n");

  // Check magnet
  delay(100);
  if (magnetOK()) {
    Serial.println("✓ Magnet detected");
  } else {
    Serial.println("✗ No magnet — check AS5600 placement");
  }

  // Init ESP-NOW
  if (esp_now_init() != ESP_OK) {
    Serial.println("ESP-NOW init failed");
    return;
  }
  esp_now_register_send_cb(onSent);

  // Add receiver peer
  memcpy(peerInfo.peer_addr, receiverMAC, 6);
  peerInfo.channel = 0;
  peerInfo.encrypt = false;
  if (esp_now_add_peer(&peerInfo) != ESP_OK) {
    Serial.println("Failed to add peer — check MAC address");
  } else {
    Serial.println("✓ ESP-NOW peer added");
    Serial.println("Sending scroll data...\n");
  }

  lastAngle = readAngle();
  lastReadMs = millis();
}

void loop() {
  unsigned long now = millis();

  // ── Read encoder every 10ms ──────────────────────────
  if (now - lastReadMs >= 10) {
    unsigned long dt = now - lastReadMs;
    lastReadMs = now;

    int16_t angle = (int16_t)readAngle();
    int16_t delta = angle - lastAngle;

    // Handle wraparound
    if (delta >  2048) delta -= 4096;
    if (delta < -2048) delta += 4096;
    lastAngle = angle;

    if (delta != 0) {
      // Velocity based acceleration
      float velocity  = (float)abs(delta) / (float)dt;
      int   mult      = (int)constrain(velocity * ACCEL_FACTOR * 80.0f, 1.0f, (float)MAX_MULT);

      accumulator += delta;

      int ticks = 0;
      while (accumulator >=  THRESHOLD) { accumulator -= THRESHOLD; ticks++;  }
      while (accumulator <= -THRESHOLD) { accumulator += THRESHOLD; ticks--;  }

      if (ticks != 0) {
        // Apply multiplier
        int finalTicks = ticks * mult;
        // Clamp to int8 range
        finalTicks = constrain(finalTicks, -127, 127);

        msg.scrollTicks = (int8_t)finalTicks;
        msg.btnPressed  = false;
        msg.mode        = scrollMode;

        esp_now_send(receiverMAC, (uint8_t*)&msg, sizeof(msg));

        Serial.print("Ticks: "); Serial.print(finalTicks);
        Serial.print("  mult: "); Serial.print(mult);
        Serial.print("  mode: "); Serial.println(modeNames[scrollMode]);
      }
    }
  }

  // ── Button debounce ──────────────────────────────────
  bool btnNow = (digitalRead(BTN_PIN) == LOW);
  if (btnNow != lastBtnState && (now - lastBtnMs) > 50) {
    lastBtnMs   = now;
    lastBtnState = btnNow;

    if (btnNow) {
      // Button pressed — cycle mode
      scrollMode = (scrollMode + 1) % 3;

      msg.scrollTicks = 0;
      msg.btnPressed  = true;
      msg.mode        = scrollMode;

      esp_now_send(receiverMAC, (uint8_t*)&msg, sizeof(msg));

      Serial.print("Mode changed to: ");
      Serial.println(modeNames[scrollMode]);
    }
  }
}
