/*********************************************************************
 * AS5600 Magnetic Encoder — Test Sketch
 * Board : NodeMCU v1.1 (ESP8266)
 * SDA   : D2 (GPIO4)
 * SCL   : D1 (GPIO5)
 * 
 * Wiring:
 *   AS5600 VCC → 3.3V
 *   AS5600 GND → GND
 *   AS5600 SDA → D2
 *   AS5600 SCL → D1
 *   AS5600 DIR → GND  (CW = increasing angle)
 *   4.7kΩ pullup on SDA → 3.3V
 *   4.7kΩ pullup on SCL → 3.3V
 * 
 * Open Serial Monitor at 115200 baud
 *********************************************************************/

#include <Wire.h>

// AS5600 I2C address (fixed, cannot be changed)
#define AS5600_ADDR     0x36

// AS5600 register addresses
#define REG_RAW_ANGLE_H 0x0C  // raw angle high byte
#define REG_RAW_ANGLE_L 0x0D  // raw angle low byte
#define REG_ANGLE_H     0x0E  // filtered angle high byte (use this)
#define REG_ANGLE_L     0x0F  // filtered angle low byte
#define REG_STATUS      0x0B  // magnet status
#define REG_AGC         0x1A  // automatic gain control
#define REG_MAGNITUDE_H 0x1B  // magnitude high byte
#define REG_MAGNITUDE_L 0x1C  // magnitude low byte

// ── Tuning ──────────────────────────────────────────
// How many raw steps (0–4095) before firing one scroll tick
// Lower = more sensitive, Higher = less sensitive
const int  THRESHOLD        = 20;

// Velocity acceleration — how aggressively fast spins multiply steps
// 1.0 = linear (no acceleration), 3.0 = strong acceleration
const float ACCEL_FACTOR    = 2.0;

// Velocity window in ms — how recent the speed measurement is
const unsigned long VEL_WINDOW_MS = 80;
// ────────────────────────────────────────────────────

int16_t  lastAngle    = 0;
int16_t  accumulator  = 0;
int      totalTicks   = 0;        // running total for display
unsigned long lastReadMs = 0;
unsigned long lastPrintMs = 0;

// ── Read a 16-bit register pair from AS5600 ─────────
uint16_t readReg16(uint8_t regH) {
  Wire.beginTransmission(AS5600_ADDR);
  Wire.write(regH);
  Wire.endTransmission(false);
  Wire.requestFrom(AS5600_ADDR, (uint8_t)2);
  uint16_t val = 0;
  if (Wire.available() >= 2) {
    val  = (Wire.read() & 0x0F) << 8;  // high nibble (12-bit)
    val |=  Wire.read();                 // low byte
  }
  return val;
}

// ── Read single byte register ────────────────────────
uint8_t readReg8(uint8_t reg) {
  Wire.beginTransmission(AS5600_ADDR);
  Wire.write(reg);
  Wire.endTransmission(false);
  Wire.requestFrom(AS5600_ADDR, (uint8_t)1);
  return Wire.available() ? Wire.read() : 0;
}

// ── Check magnet status ──────────────────────────────
void printMagnetStatus() {
  uint8_t status = readReg8(REG_STATUS);
  uint8_t agc    = readReg8(REG_AGC);
  uint16_t mag   = ((readReg8(REG_MAGNITUDE_H) & 0x0F) << 8)
                 |   readReg8(REG_MAGNITUDE_L);

  Serial.println(F("──────────────────────────────"));
  Serial.println(F("AS5600 Magnet Status:"));

  bool magDetected = status & (1 << 5);
  bool tooWeak     = status & (1 << 4);
  bool tooStrong   = status & (1 << 3);

  if (magDetected)  Serial.println(F("  ✓ Magnet detected"));
  else              Serial.println(F("  ✗ NO MAGNET DETECTED — check placement"));

  if (tooWeak)      Serial.println(F("  ⚠ Magnet too weak — move closer"));
  if (tooStrong)    Serial.println(F("  ⚠ Magnet too strong — move further away"));
  if (!tooWeak && !tooStrong && magDetected)
                    Serial.println(F("  ✓ Magnet strength OK"));

  Serial.print(F("  AGC value : ")); Serial.println(agc);
  Serial.print(F("  Magnitude : ")); Serial.println(mag);
  Serial.println(F("──────────────────────────────"));
}

void setup() {
  Serial.begin(115200);
  delay(500);

  Wire.begin(4, 5);  // SDA=GPIO4 (D2), SCL=GPIO5 (D1)
  Wire.setClock(400000);  // 400kHz fast mode

  Serial.println(F("\n\nAS5600 Encoder Test — NodeMCU v1.1"));
  Serial.println(F("===================================="));

  // Check if AS5600 is reachable on I2C
  Wire.beginTransmission(AS5600_ADDR);
  uint8_t err = Wire.endTransmission();
  if (err == 0) {
    Serial.println(F("✓ AS5600 found on I2C bus (0x36)"));
  } else {
    Serial.print(F("✗ AS5600 NOT found — I2C error: "));
    Serial.println(err);
    Serial.println(F("Check wiring and pullup resistors"));
  }

  // Print magnet status on boot
  printMagnetStatus();

  // Seed lastAngle
  lastAngle = (int16_t)readReg16(REG_ANGLE_H);
  lastReadMs = millis();

  Serial.println(F("Rotate the encoder — watching for ticks..."));
  Serial.println(F("Format: [angle 0-4095] | delta | ticks | velocity"));
  Serial.println();
}

void loop() {
  unsigned long now = millis();

  // Read angle every ~10ms
  if (now - lastReadMs < 10) return;
  unsigned long dt = now - lastReadMs;
  lastReadMs = now;

  int16_t angle = (int16_t)readReg16(REG_ANGLE_H);

  // ── Compute signed delta with wraparound ────────────
  int16_t delta = angle - lastAngle;
  if (delta >  2048) delta -= 4096;   // crossed 4095→0 boundary
  if (delta < -2048) delta += 4096;   // crossed 0→4095 boundary
  lastAngle = angle;

  if (delta == 0) return;  // no movement, skip

  // ── Velocity (steps per ms) ──────────────────────────
  float velocity = (float)abs(delta) / (float)dt;

  // ── Acceleration multiplier ──────────────────────────
  // velocity * ACCEL_FACTOR gives a smooth curve
  // constrained to 1–10x so it never goes wild
  int multiplier = (int)constrain(velocity * ACCEL_FACTOR * 80.0f, 1.0f, 10.0f);

  // ── Accumulate and fire ticks ────────────────────────
  accumulator += delta;
  int fired = 0;

  while (accumulator >=  THRESHOLD) { accumulator -= THRESHOLD; totalTicks++;  fired++; }
  while (accumulator <= -THRESHOLD) { accumulator += THRESHOLD; totalTicks--;  fired--; }

  // ── Serial output ────────────────────────────────────
  // Print every tick fired, not every loop iteration
  if (fired != 0) {
    Serial.print(F("angle="));
    Serial.print(angle);
    Serial.print(F("\tdelta="));
    Serial.print(delta);
    Serial.print(F("\tvel="));
    Serial.print(velocity, 3);
    Serial.print(F("\tmult="));
    Serial.print(multiplier);
    Serial.print(F("\tticks_fired="));
    Serial.print(fired);
    Serial.print(F("\ttotal="));
    Serial.println(totalTicks);
  }

  // ── Print magnet status every 5 seconds ─────────────
  if (now - lastPrintMs > 5000) {
    lastPrintMs = now;
    printMagnetStatus();
  }
}
