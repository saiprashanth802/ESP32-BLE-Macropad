/*********************************************************************
 * Encoder Puck (ESP8266) — AS5600 + SSD1306, ESP-NOW sender for MacroPad v5
 *
 * ESP8266 port of firmware/v5/puck_encoder/puck_encoder.ino. Same wire
 * protocol (v2), same OLED layout, same tuning — only the radio API and the
 * pins differ. ESP-NOW interoperates between ESP8266 and ESP32 over the air,
 * so this talks to the unmodified ESP32 pad firmware.
 *
 * Hardware : ESP-12E / ESP-12F (bare module or NodeMCU v1.0)
 *            AS5600 magnetic encoder on I2C, addr 0x36
 *            SSD1306 128x32 OLED  on I2C, addr 0x3C  (same bus)
 *            Optional momentary button to GND
 *
 * Wiring (ESP-12 GPIO numbers; NodeMCU silkscreen in brackets):
 *   AS5600  SDA -> GPIO4 [D2]   SCL -> GPIO5 [D1]   VCC -> 3V3   GND -> GND
 *   OLED    SDA -> GPIO4 [D2]   SCL -> GPIO5 [D1]   VCC -> 3V3   GND -> GND
 *   Button  GPIO13 [D7] -> switch -> GND   (internal pullup, active LOW)
 *   AS5600  DIR -> GND  (fixes rotation direction; floating is undefined)
 *
 *   GPIO 0 / 2 / 15 are strapping pins and are deliberately unused — the
 *   module will not boot if a peripheral holds them at the wrong level.
 *
 * ⚠ POWER: the ESP8266 is 3.3V ONLY and has no onboard regulator on a bare
 *   ESP-12. It also pulls 300mA+ peaks on WiFi transmit, which is far more
 *   than the 3V3 pin of a typical USB-TTL dongle can supply — brownouts there
 *   look like random resets or failed uploads. Use a real 3.3V supply.
 *
 * Boot/flash (bare ESP-12 on a passive adapter):
 *   EN/CH_PD -> 10k -> 3V3   ·   GPIO15 -> 10k -> GND   ·   RST -> 10k -> 3V3
 *   GPIO0 -> GND to flash, floating to run
 *
 * Build (bare module):
 *   arduino-cli compile --fqbn esp8266:esp8266:generic \
 *     --libraries C:/Users/gsaip/Documents/Arduino/libraries \
 *     firmware/v5/puck_encoder_8266
 * Build (NodeMCU v1.0 devkit):  --fqbn esp8266:esp8266:nodemcuv2
 *
 * Protocol : docs/PUCK_PROTOCOL.md
 *********************************************************************/

#include <Wire.h>
#include <ESP8266WiFi.h>
#include <espnow.h>                    // note: espnow.h, not esp_now.h
extern "C" {
  #include "user_interface.h"          // wifi_set_channel
}
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

// ── Pins ─────────────────────────────────────────────────────────────
#define PIN_SDA   4     // D2
#define PIN_SCL   5     // D1
#define PIN_BTN   13    // D7 — optional, safe to leave unwired

// ── Pad's station MAC ────────────────────────────────────────────────
// The v5 bench board. NOT the BLE address (which is this + 2).
// esp_now_send/add_peer on ESP8266 take non-const pointers, so this cannot
// be declared const.
static uint8_t PAD_MAC[6] = { 0x84, 0x1F, 0xE8, 0x2B, 0x33, 0x48 };

#define PUCK_WIFI_CHANNEL 1

// ════════════════════════════════════════════════════════════════════
//  WIRE PROTOCOL — KEEP IN SYNC with puck_encoder.ino, macropad_v5.ino
//  and docs/PUCK_PROTOCOL.md
// ════════════════════════════════════════════════════════════════════
// v3 added the sensitivity bytes and PK_OTA. NOT wire-compatible with v2 —
// the struct grew, and the pad length-checks every packet.
#define PUCK_PROTO_VER 3

#define PK_HELLO  0x01   // puck -> pad, boot + idle heartbeat
#define PK_INPUT  0x02   // puck -> pad, rotation and/or button
#define PK_STATE  0x03   // pad  -> puck, authoritative mode + volume + flags
#define PK_OTA    0x04   // pad  -> puck, OTA request (C3 only — see onRecv)

#define PM_VOLUME 0
#define PM_SCROLL 1
#define PM_ZOOM   2
#define PM_COUNT  3

#define BTN_NONE   0
#define BTN_SHORT  1
#define BTN_DOUBLE 2
#define BTN_LONG   3

#define PF_BLE_UP     0x01
#define PF_SCROLL_OK  0x02
#define PF_DISABLED   0x04
#define PF_VOL_KNOWN  0x08
#define PF_VOL_MUTED  0x10

#define PUCK_VOL_UNKNOWN 0xFF

struct PuckMsg {
  uint8_t ver;
  uint8_t type;
  uint8_t mode;    // puck->pad: ignored. pad->puck: authoritative mode.
  int8_t  ticks;
  uint8_t btn;
  uint8_t flags;
  uint8_t vol;     // pad->puck: 0-100, or PUCK_VOL_UNKNOWN
  uint8_t speed;   // v3, pad->puck: detents per revolution
  uint8_t accel;   // v3, pad->puck: acceleration cap, 1 = off
};
// ════════════════════════════════════════════════════════════════════

#define PUCK_CPR         4096
#define PUCK_SPEED_MIN   8
#define PUCK_SPEED_MAX   128
#define PUCK_SPEED_DEF   50
#define PUCK_ACCEL_MIN   1
#define PUCK_ACCEL_MAX   8
#define PUCK_ACCEL_DEF   4

// ── AS5600 ───────────────────────────────────────────────────────────
#define AS5600_ADDR  0x36
#define REG_ANGLE_H  0x0E
#define REG_STATUS   0x0B

// ── OLED ─────────────────────────────────────────────────────────────
// Must match the physical panel: 32 for the 0.91", 64 for the 0.96".
// The SSD1306 cannot report its own size, so a mismatch renders cropped.
#define OLED_W    128
#define OLED_H    32
#define OLED_ADDR 0x3C
Adafruit_SSD1306 oled(OLED_W, OLED_H, &Wire, -1);
bool oledOK = false;

// ── Tuning (identical to the C3 build) ───────────────────────────────
// See puck_encoder.ino for the sensitivity maths. Short version: 4096 counts
// per revolution, Windows moves volume 2% per tick, so 50 detents per turn
// makes one unhurried revolution about one full volume sweep. Both values are
// owned by the pad and arrive in PK_STATE; these are the pre-link defaults.
// ACCEL_FACTOR is the multiplier at 1 count/ms — the *80.0f that used to be in
// the mult expression pinned it at the cap for every real turn.
volatile uint8_t puckSpeed = PUCK_SPEED_DEF;
volatile uint8_t puckAccel = PUCK_ACCEL_DEF;
const float ACCEL_FACTOR   = 1.0f;

static inline int threshold() {
  uint8_t s = constrain((int)puckSpeed, PUCK_SPEED_MIN, PUCK_SPEED_MAX);
  return PUCK_CPR / s;
}
const unsigned long READ_INTERVAL_MS  = 10;
const unsigned long SEND_INTERVAL_MS  = 40;
const unsigned long HELLO_INTERVAL_MS = 5000;
const unsigned long LONG_PRESS_MS     = 600;
const unsigned long DOUBLE_GAP_MS     = 280;
const unsigned long DEBOUNCE_MS       = 40;
const unsigned long OLED_DIM_MS       = 20000;
const unsigned long BAR_HOLD_MS       = 1500;

// ── State ────────────────────────────────────────────────────────────
int16_t  lastAngle    = 0;
int32_t  accumulator  = 0;
int16_t  pendingTicks = 0;
volatile uint8_t mode = PM_VOLUME;      // written by the recv callback
bool     magnetPresent = false;

unsigned long lastReadMs   = 0;
unsigned long lastSendMs   = 0;
unsigned long lastHelloMs  = 0;
unsigned long lastActivity = 0;
unsigned long lastOledMs   = 0;

// Button
bool          btnStable    = false;
bool          btnLastRaw   = false;
unsigned long btnEdgeMs    = 0;
unsigned long btnDownMs    = 0;
bool          longFired    = false;
unsigned long pendingClickMs = 0;
bool          clickPending  = false;

// Link — pushed down by the pad in PK_STATE
volatile uint8_t padFlags   = 0;
volatile uint8_t padVolume  = PUCK_VOL_UNKNOWN;
volatile bool    padSeen    = false;
volatile unsigned long lastPadMs = 0;
volatile bool    otaAsked   = false;   // pad asked for OTA; C3-only, see loop()
bool          peerReady    = false;
const char*   lastAction   = "";
unsigned long lastActionMs = 0;

const char* MODE_NAMES[PM_COUNT] = { "VOLUME", "SCROLL", "ZOOM" };

// ── AS5600 helpers ───────────────────────────────────────────────────
uint16_t readAngle() {
  Wire.beginTransmission(AS5600_ADDR);
  Wire.write(REG_ANGLE_H);
  if (Wire.endTransmission(false) != 0) return lastAngle;
  Wire.requestFrom((uint8_t)AS5600_ADDR, (uint8_t)2);
  if (Wire.available() >= 2) {
    uint16_t v = (Wire.read() & 0x0F) << 8;
    v |= Wire.read();
    return v;
  }
  return lastAngle;
}

bool magnetOK() {
  Wire.beginTransmission(AS5600_ADDR);
  Wire.write(REG_STATUS);
  if (Wire.endTransmission(false) != 0) return false;
  Wire.requestFrom((uint8_t)AS5600_ADDR, (uint8_t)1);
  if (Wire.available()) return (Wire.read() & (1 << 5));   // MD bit
  return false;
}

// ── I2C probe ────────────────────────────────────────────────────────
// Adafruit_SSD1306::begin() only fails on a malloc error — it never checks
// that the panel ACKs, so a missing OLED looks like a healthy one.
bool i2cPresent(uint8_t addr) {
  Wire.beginTransmission(addr);
  return Wire.endTransmission() == 0;
}

void i2cScan() {
  Serial.println(F("[I2C] scanning..."));
  uint8_t found = 0;
  for (uint8_t a = 1; a < 127; a++) {
    if (!i2cPresent(a)) continue;
    found++;
    Serial.printf("[I2C]   0x%02X", a);
    if      (a == AS5600_ADDR)        Serial.print(F("  <- AS5600"));
    else if (a == 0x3C || a == 0x3D)  Serial.print(F("  <- SSD1306 OLED"));
    Serial.println();
  }
  if (!found) Serial.println(F("[I2C]   nothing responded — check SDA/SCL/power"));
}

// ── ESP-NOW ──────────────────────────────────────────────────────────
// ESP8266 callback signature differs from ESP32's: plain (mac, data, len)
// with no esp_now_recv_info_t, and len is a uint8_t.
void onRecv(uint8_t* mac, uint8_t* data, uint8_t len) {
  if (len != (uint8_t)sizeof(PuckMsg)) return;
  const PuckMsg* m = (const PuckMsg*)data;
  if (m->ver != PUCK_PROTO_VER) return;

  // OTA is implemented on the C3 only — it has native USB and a partition table
  // with dual OTA slots. Say so rather than ignoring the packet, otherwise the
  // pad's OTA key looks broken when this board is the one that's linked.
  if (m->type == PK_OTA) { otaAsked = true; return; }

  if (m->type != PK_STATE) return;
  padFlags  = m->flags;
  padVolume = m->vol;
  // The pad owns the mode — our button only ever asks it to cycle.
  if (m->mode < PM_COUNT) mode = m->mode;
  // ...and dial feel too, so Settings can retune without a reflash.
  if (m->speed >= PUCK_SPEED_MIN && m->speed <= PUCK_SPEED_MAX) puckSpeed = m->speed;
  if (m->accel >= PUCK_ACCEL_MIN && m->accel <= PUCK_ACCEL_MAX) puckAccel = m->accel;
  padSeen   = true;
  lastPadMs = millis();
}

void onSent(uint8_t* mac, uint8_t status) {
  // silent — the link indicator comes from PK_STATE replies, not send status
  (void)mac; (void)status;
}

void sendMsg(uint8_t type, int8_t ticks, uint8_t btn) {
  if (!peerReady) return;
  PuckMsg m;
  m.ver   = PUCK_PROTO_VER;
  m.type  = type;
  m.mode  = mode;
  m.ticks = ticks;
  m.btn   = btn;
  m.flags = 0;
  m.vol   = PUCK_VOL_UNKNOWN;
  m.speed = puckSpeed;
  m.accel = puckAccel;
  esp_now_send(PAD_MAC, (uint8_t*)&m, sizeof(m));
}

void setupEspNow() {
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  wifi_set_channel(PUCK_WIFI_CHANNEL);

  // ESP8266 returns 0 on success here, not ESP_OK.
  if (esp_now_init() != 0) {
    Serial.println(F("[NOW] init failed"));
    return;
  }
  // No ESP32 equivalent: the 8266 stack needs an explicit role. COMBO =
  // send and receive, which we need for the PK_STATE replies.
  esp_now_set_self_role(ESP_NOW_ROLE_COMBO);
  esp_now_register_recv_cb(onRecv);
  esp_now_register_send_cb(onSent);

  if (esp_now_add_peer(PAD_MAC, ESP_NOW_ROLE_COMBO, PUCK_WIFI_CHANNEL, NULL, 0) != 0) {
    Serial.println(F("[NOW] add_peer failed"));
    return;
  }
  peerReady = true;
  Serial.println(F("[NOW] ready"));
}

// ── OLED ─────────────────────────────────────────────────────────────
void drawOled() {
  if (!oledOK) return;
  lastOledMs = millis();

  unsigned long now = millis();
  bool linkUp = padSeen && (now - lastPadMs < 3000);

  // Idle blank, latched so it isn't re-pushed over I2C every 100ms
  static bool blanked = false;
  if (now - lastActivity > OLED_DIM_MS) {
    if (!blanked) { blanked = true; oled.clearDisplay(); oled.display(); }
    return;
  }
  blanked = false;

  oled.clearDisplay();
  oled.setTextColor(SSD1306_WHITE);
  oled.setTextSize(1);

  // Volume bar while the dial is in use, then revert to the mode name.
  // Needs a real level from the companion app — BLE HID volume is relative,
  // so without it there is no honest number to draw.
  bool muted    = (padFlags & PF_VOL_MUTED) != 0;
  bool volKnown = (padFlags & PF_VOL_KNOWN) && padVolume != PUCK_VOL_UNKNOWN;
  bool showBar  = (mode == PM_VOLUME) && volKnown && magnetPresent
                  && (now - lastActivity < BAR_HOLD_MS);

  if (showBar) {
    uint8_t v = padVolume > 100 ? 100 : padVolume;
    oled.setCursor(0, 0);
    oled.print(muted ? F("MUTED") : F("VOL"));

    char pct[6];
    snprintf(pct, sizeof(pct), "%u%%", v);
    oled.setCursor(OLED_W - (int)strlen(pct) * 6, 0);
    oled.print(pct);

    const int by = (OLED_H >= 64) ? 30 : 14;
    const int bh = (OLED_H >= 64) ? 20 : 16;
    oled.drawRect(0, by, OLED_W, bh, SSD1306_WHITE);
    if (!muted && v > 0)
      oled.fillRect(2, by + 2, ((OLED_W - 4) * v) / 100, bh - 4, SSD1306_WHITE);

    oled.display();
    return;
  }

  // Idle: link state + mode name
  oled.setCursor(0, 0);
  if (!linkUp)                          oled.print(F("NO PAD"));
  else if (padFlags & PF_DISABLED)      oled.print(F("PAD: OFF"));
  else if (!(padFlags & PF_BLE_UP))     oled.print(F("PAD: NO BLE"));
  else                                  oled.print(F("LINKED"));

  if (linkUp) oled.fillCircle(122, 3, 3, SSD1306_WHITE);
  else        oled.drawCircle(122, 3, 3, SSD1306_WHITE);

  oled.drawFastHLine(0, 10, 128, SSD1306_WHITE);

  const char* big = MODE_NAMES[mode];
  bool small = false;
  if (!magnetPresent)                                  { big = "NO MAGNET"; small = true; }
  else if (now - lastActionMs < 1000 && lastAction[0])  { big = lastAction; }

#if OLED_H >= 64
  oled.setTextSize(2);
  oled.setCursor((OLED_W - (int)strlen(big) * 12) / 2, 24);
  oled.print(big);
  oled.drawFastHLine(0, 55, 128, SSD1306_WHITE);
  oled.setTextSize(1);
  oled.setCursor(0, 57);
  oled.print(F("hold=mode  click=play"));
#else
  if (small) {
    oled.setTextSize(1);
    oled.setCursor((OLED_W - (int)strlen(big) * 6) / 2, 18);
  } else {
    oled.setTextSize(2);
    oled.setCursor((OLED_W - (int)strlen(big) * 12) / 2, 14);
  }
  oled.print(big);
#endif

  oled.display();
}

void note(const char* s) { lastAction = s; lastActionMs = millis(); }

// ── Button gestures (optional hardware) ──────────────────────────────
void handleButton(unsigned long now) {
  bool raw = (digitalRead(PIN_BTN) == LOW);
  if (raw != btnLastRaw) { btnLastRaw = raw; btnEdgeMs = now; }

  if (raw != btnStable && (now - btnEdgeMs) > DEBOUNCE_MS) {
    btnStable = raw;
    if (btnStable) { btnDownMs = now; longFired = false; }
    else if (!longFired) {
      if (clickPending && (now - pendingClickMs) < DOUBLE_GAP_MS) {
        clickPending = false;
        sendMsg(PK_INPUT, 0, BTN_DOUBLE);
        note("NEXT");
        lastActivity = now;
      } else {
        clickPending   = true;
        pendingClickMs = now;
      }
    }
  }

  // Only *asks* the pad to cycle — the pad owns the mode.
  if (btnStable && !longFired && (now - btnDownMs) > LONG_PRESS_MS) {
    longFired    = true;
    clickPending = false;
    sendMsg(PK_INPUT, 0, BTN_LONG);
    note("MODE");
    lastActivity = now;
  }

  if (clickPending && (now - pendingClickMs) >= DOUBLE_GAP_MS) {
    clickPending = false;
    sendMsg(PK_INPUT, 0, BTN_SHORT);
    note("PLAY/PAUSE");
    lastActivity = now;
  }
}

// ── Setup ────────────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println(F("\n\n=== MacroPad Encoder Puck (ESP8266) ==="));

  pinMode(PIN_BTN, INPUT_PULLUP);

  Wire.begin(PIN_SDA, PIN_SCL);
  Wire.setClock(400000);
  Serial.printf("[I2C] SDA=GPIO%d  SCL=GPIO%d   BTN=GPIO%d\n",
                PIN_SDA, PIN_SCL, PIN_BTN);
  i2cScan();

  uint8_t oledAddr = i2cPresent(0x3C) ? 0x3C : (i2cPresent(0x3D) ? 0x3D : 0);
  if (oledAddr && oled.begin(SSD1306_SWITCHCAPVCC, oledAddr)) {
    oledOK = true;
    Serial.printf("[OLED] found at 0x%02X\n", oledAddr);
    oled.clearDisplay();
    oled.setTextSize(1);
    oled.setTextColor(SSD1306_WHITE);
    oled.setCursor(0, 0);
    oled.println(F("MacroPad Puck"));
    oled.println(F("starting..."));
    oled.display();
  } else {
    Serial.println(F("[OLED] NOT WIRED — no ACK at 0x3C or 0x3D"));
  }

  if (!i2cPresent(AS5600_ADDR)) {
    Serial.println(F("[AS5600] NOT WIRED — no ACK at 0x36"));
  } else {
    magnetPresent = magnetOK();
    Serial.println(magnetPresent ? F("[AS5600] ok, magnet detected")
                                 : F("[AS5600] ok, but NO MAGNET in range"));
  }

  Serial.printf("[BTN] GPIO%d reads %s (HIGH when not pressed/unwired)\n",
                PIN_BTN, digitalRead(PIN_BTN) ? "HIGH" : "LOW");

  setupEspNow();

  // macAddress() is only valid once the WiFi driver is up, so this must come
  // after setupEspNow()'s WiFi.mode(WIFI_STA).
  Serial.print(F("[NOW] this puck MAC: "));
  Serial.println(WiFi.macAddress());
  Serial.print(F("[NOW] target pad MAC: "));
  for (int i = 0; i < 6; i++) { Serial.printf("%02X", PAD_MAC[i]); if (i < 5) Serial.print(':'); }
  Serial.println();
  Serial.printf("[SYS] free heap %u\n", ESP.getFreeHeap());

  lastAngle    = (int16_t)readAngle();
  lastReadMs   = millis();
  lastActivity = millis();

  sendMsg(PK_HELLO, 0, BTN_NONE);
  drawOled();
}

// ── Loop ─────────────────────────────────────────────────────────────
void loop() {
  unsigned long now = millis();

  if (otaAsked) {
    otaAsked = false;
    Serial.println(F("[OTA] not supported on the ESP-12 build — flash over USB"));
  }

  // ── Encoder ───────────────────────────────────────────────
  if (now - lastReadMs >= READ_INTERVAL_MS) {
    unsigned long dt = now - lastReadMs;
    lastReadMs = now;

    int16_t angle = (int16_t)readAngle();
    int16_t delta = angle - lastAngle;
    if (delta >  2048) delta -= 4096;        // 12-bit wraparound
    if (delta < -2048) delta += 4096;
    lastAngle = angle;

    if (delta != 0) {
      const int th   = threshold();
      const int cap  = constrain((int)puckAccel, PUCK_ACCEL_MIN, PUCK_ACCEL_MAX);
      float velocity = (float)abs(delta) / (float)dt;
      int   mult     = (int)constrain(velocity * ACCEL_FACTOR, 1.0f, (float)cap);

      accumulator += delta;
      int ticks = 0;
      while (accumulator >=  th) { accumulator -= th; ticks++; }
      while (accumulator <= -th) { accumulator += th; ticks--; }

      if (ticks != 0) {
        pendingTicks = constrain(pendingTicks + ticks * mult, -127, 127);
        lastActivity = now;
      }
    }
  }

  // ── Coalesced send ────────────────────────────────────────
  // A fast spin becomes a few packets, not hundreds — this is what keeps the
  // pad's BLE link stable while its WiFi radio is up.
  if (pendingTicks != 0 && (now - lastSendMs) >= SEND_INTERVAL_MS) {
    lastSendMs = now;
    sendMsg(PK_INPUT, (int8_t)pendingTicks, BTN_NONE);
    note(pendingTicks > 0 ? "+" : "-");
    pendingTicks = 0;
  }

  handleButton(now);

  // ── Heartbeat, only while idle ────────────────────────────
  if (pendingTicks == 0 && (now - lastHelloMs) >= HELLO_INTERVAL_MS) {
    lastHelloMs = now;
    sendMsg(PK_HELLO, 0, BTN_NONE);
    magnetPresent = magnetOK();
  }

  if (now - lastOledMs >= 100) drawOled();

  // ── Link diagnostic ───────────────────────────────────────
  // Prints whenever anything the pad sends us changes. Exists so the
  // pad→puck direction can be verified over serial before the OLED is
  // wired — "LINKED" on the pad only proves puck→pad.
  {
    static uint8_t lastF = 0xFF, lastM = 0xFF, lastV = 0xFE;
    static bool    everSeen = false;
    if (padSeen && (padFlags != lastF || mode != lastM || padVolume != lastV)) {
      lastF = padFlags; lastM = mode; lastV = padVolume;
      if (!everSeen) { everSeen = true; Serial.println(F("[NOW] first PK_STATE from pad")); }
      Serial.printf("[PAD] mode=%s ble=%d scroll=%d off=%d vol=",
                    MODE_NAMES[mode < PM_COUNT ? mode : 0],
                    (padFlags & PF_BLE_UP)    ? 1 : 0,
                    (padFlags & PF_SCROLL_OK) ? 1 : 0,
                    (padFlags & PF_DISABLED)  ? 1 : 0);
      if (padFlags & PF_VOL_KNOWN) Serial.printf("%u%%%s\n", padVolume,
                                                 (padFlags & PF_VOL_MUTED) ? " MUTED" : "");
      else                         Serial.println(F("unknown (companion not running?)"));
    }
  }

  delay(1);          // feeds the ESP8266 soft-WDT
}
