/*********************************************************************
 * Encoder Puck — AS5600 + SSD1306 OLED, ESP-NOW sender for MacroPad v5
 *
 * A standalone wireless knob. Rotation and button gestures go to the pad
 * over ESP-NOW; the pad turns them into BLE HID. The puck never pairs
 * with a computer itself.
 *
 * Hardware : ESP32-C3 SuperMini  (or ESP32-S3 mini — see BOARD below)
 *            AS5600 magnetic encoder on I2C, addr 0x36
 *            SSD1306 128x64 OLED  on I2C, addr 0x3C  (same bus)
 *            Momentary button to GND
 *
 * Wiring (C3 SuperMini defaults — change the PIN block for other boards):
 *   AS5600  SDA -> GPIO5    SCL -> GPIO6    VCC -> 3V3   GND -> GND
 *   OLED    SDA -> GPIO5    SCL -> GPIO6    VCC -> 3V3   GND -> GND
 *   Button  one leg -> GPIO4, other leg -> GND   (internal pullup, active LOW)
 *
 *   Both I2C devices share one bus — different addresses, no conflict.
 *   The AS5600 needs a diametrically magnetised magnet 0.5-3mm above the
 *   chip, centred on it. A radially magnetised magnet reads as noise.
 *
 * Controls:
 *   Rotate         mode-dependent (volume / scroll / zoom)
 *   Short click    play / pause
 *   Double click   next track
 *   Long press     cycle mode
 *
 * Protocol : docs/PUCK_PROTOCOL.md  — read that before changing PuckMsg
 *
 * Libraries: Adafruit_SSD1306, Adafruit_GFX, Wire, esp_now, WiFi (all stock)
 *
 * Build:
 *   arduino-cli compile --fqbn esp32:esp32:esp32c3 \
 *     --libraries C:/Users/gsaip/Documents/Arduino/libraries \
 *     firmware/v5/puck_encoder
 *********************************************************************/

#include <Wire.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <WebServer.h>
#include <Update.h>

// The OLED was removed from this build (the design is dial-only now). Set to 1
// and rewire SDA/SCL to bring it back — all the drawing code is still here,
// just compiled out. Leaving it off also buys ~35 KB of flash, which is what
// makes room for the OTA web server below.
#define PUCK_OLED 0

#if PUCK_OLED
  #include <Adafruit_GFX.h>
  #include <Adafruit_SSD1306.h>
#endif

// ── Board select ─────────────────────────────────────────────────────
// 0 = ESP32-C3 SuperMini, 1 = ESP32-S3 mini (Lolin S3 Mini pin numbering)
#define BOARD_C3   0
#define BOARD_S3   1
#define BOARD      BOARD_C3

#if BOARD == BOARD_C3
  // NOT the Arduino default 8/9: on the C3, GPIO2/8/9 are strapping pins, and
  // on the SuperMini GPIO8 also drives the onboard LED (which sits across the
  // rail as a stray pullup). 5/6/4 are plain GPIOs with none of that.
  #define PIN_SDA   5
  #define PIN_SCL   6
  #define PIN_BTN   4
#else
  // Lolin S3 Mini breaks out I2C on 35/36. A generic S3 devkit uses 8/9 —
  // if the OLED never appears, this block is the first thing to check.
  #define PIN_SDA   35
  #define PIN_SCL   36
  #define PIN_BTN   2
#endif

// ── Pad's station MAC ────────────────────────────────────────────────
// The v5 bench board. NOT the BLE address (which is this + 2) — see the
// pairing section of docs/PUCK_PROTOCOL.md.
static uint8_t PAD_MAC[6] = { 0x84, 0x1F, 0xE8, 0x2B, 0x33, 0x48 };

#define PUCK_WIFI_CHANNEL 1

// ════════════════════════════════════════════════════════════════════
//  WIRE PROTOCOL — KEEP IN SYNC with macropad_v5.ino and PUCK_PROTOCOL.md
// ════════════════════════════════════════════════════════════════════
// v2 added the volume byte and moved mode ownership to the pad.
// v3 added the sensitivity bytes and PK_OTA, so the pad owns dial feel too.
// v3 is NOT wire-compatible with v2 — the struct grew. Flash both ends.
#define PUCK_PROTO_VER 3

#define PK_HELLO  0x01   // puck -> pad, boot + idle heartbeat
#define PK_INPUT  0x02   // puck -> pad, rotation and/or button
#define PK_STATE  0x03   // pad  -> puck, authoritative mode + volume + flags
#define PK_OTA    0x04   // pad  -> puck, drop ESP-NOW and raise the OTA SoftAP

#define PM_VOLUME 0
#define PM_SCROLL 1
#define PM_ZOOM   2
#define PM_COUNT  3

#define BTN_NONE   0
#define BTN_SHORT  1
#define BTN_DOUBLE 2
#define BTN_LONG   3

#define PF_BLE_UP     0x01   // pad's BLE link is connected
#define PF_SCROLL_OK  0x02   // pad has mouse HID compiled in
#define PF_DISABLED   0x04   // pad has the puck switched off in Settings
#define PF_VOL_KNOWN  0x08   // vol byte is real (companion app is feeding it)
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
  uint8_t speed;   // v3, pad->puck: detents per revolution, PUCK_SPEED_MIN..MAX
  uint8_t accel;   // v3, pad->puck: acceleration cap, 1 = off
};
// ════════════════════════════════════════════════════════════════════

// Sensitivity is expressed on the wire as **detents per revolution**, not as a
// raw threshold. That is the number a human can reason about ("one turn = 50
// clicks"), it fits a byte across the whole useful range, and it keeps the
// AS5600's 4096-count resolution an implementation detail of this file.
#define PUCK_CPR         4096          // AS5600 counts per revolution
#define PUCK_SPEED_MIN   8             // very slow — 8 clicks per full turn
#define PUCK_SPEED_MAX   128           // very fast — a click every 2.8 degrees
#define PUCK_SPEED_DEF   50            // ~one full volume sweep per revolution
#define PUCK_ACCEL_MIN   1             // 1 = acceleration off
#define PUCK_ACCEL_MAX   8
#define PUCK_ACCEL_DEF   4

// ── AS5600 ───────────────────────────────────────────────────────────
#define AS5600_ADDR  0x36
#define REG_ANGLE_H  0x0E
#define REG_STATUS   0x0B

// ── OLED ─────────────────────────────────────────────────────────────
// Panel height MUST match the physical module: 32 for the 0.91" slim one,
// 64 for the 0.96". The SSD1306 cannot report its own size over I2C, so
// getting this wrong is not detectable in software — the controller just
// scans rows the panel doesn't have and you see a cropped or doubled image.
#define OLED_W    128
#define OLED_H    32
#define OLED_ADDR 0x3C
#if PUCK_OLED
Adafruit_SSD1306 oled(OLED_W, OLED_H, &Wire, -1);
#endif
bool oledOK = false;      // stays false when PUCK_OLED is 0, so drawOled() no-ops

// ── Tuning ───────────────────────────────────────────────────────────
// Sensitivity maths — the AS5600 is 4096 counts/rev, and Windows moves volume
// 2% per CONSUMER_VOL_UP, so 50 ticks is a full 0->100% sweep. That makes
// "50 detents per revolution" the natural default: one turn, one sweep.
//   threshold   = 4096 / detentsPerRev
//   ticks/rev   = detentsPerRev * mult
//
// Both live values are owned by the pad and arrive in PK_STATE, so the dial can
// be retuned from the pad's Settings without reflashing the puck. These are the
// pre-link defaults, used until the first PK_STATE lands.
volatile uint8_t puckSpeed = PUCK_SPEED_DEF;   // detents per revolution
volatile uint8_t puckAccel = PUCK_ACCEL_DEF;   // acceleration cap

static inline int threshold() {
  uint8_t s = constrain((int)puckSpeed, PUCK_SPEED_MIN, PUCK_SPEED_MAX);
  return PUCK_CPR / s;                         // 32..512 raw counts per detent
}

// ACCEL_FACTOR scales counts-per-ms straight into the multiplier, so the number
// reads as "multiplier at 1 count/ms" (~90 deg/sec, a slow deliberate turn = 1x).
// A brisk 360 deg/sec turn lands at 4x. There used to be an extra *80.0f here,
// which pinned mult at the cap for every human-speed turn and made the dial
// sweep the whole volume range in about 11 degrees.
const float ACCEL_FACTOR     = 1.0f; // multiplier per (count/ms) of angular speed
const unsigned long READ_INTERVAL_MS  = 10;
const unsigned long SEND_INTERVAL_MS  = 40;   // rotation coalescing — see PROTOCOL.md
const unsigned long HELLO_INTERVAL_MS = 5000;
const unsigned long LONG_PRESS_MS     = 600;
const unsigned long DOUBLE_GAP_MS     = 280;
const unsigned long DEBOUNCE_MS       = 40;
const unsigned long OLED_DIM_MS       = 20000; // blank the screen after this idle

// ── State ────────────────────────────────────────────────────────────
int16_t  lastAngle    = 0;
int32_t  accumulator  = 0;
int16_t  pendingTicks = 0;      // coalesced between sends
volatile uint8_t mode = PM_VOLUME;   // written by the recv callback
bool     magnetPresent = false;

unsigned long lastReadMs   = 0;
unsigned long lastSendMs   = 0;
unsigned long lastHelloMs  = 0;
unsigned long lastActivity = 0;
unsigned long lastOledMs   = 0;

// Button
bool          btnStable    = false;   // true = pressed
bool          btnLastRaw   = false;
unsigned long btnEdgeMs    = 0;
unsigned long btnDownMs    = 0;
bool          longFired    = false;
unsigned long pendingClickMs = 0;     // a single click waiting to see if it doubles
bool          clickPending  = false;

// Link — all pushed down by the pad in PK_STATE
volatile uint8_t padFlags   = 0;
volatile uint8_t padVolume  = PUCK_VOL_UNKNOWN;
volatile bool    padSeen    = false;
volatile unsigned long lastPadMs = 0;

// Set by the recv callback, acted on in loop(). Tearing down the radio from
// inside the WiFi task's own callback is a deadlock, same as esp_now_send.
volatile bool    otaRequested = false;

// How long the volume bar stays up after the last detent before the screen
// reverts to showing the mode name.
const unsigned long BAR_HOLD_MS = 1500;
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
// Adafruit_SSD1306::begin() only returns false if its buffer malloc fails —
// it never checks that the panel actually ACKs. So a missing OLED looks like
// a healthy one. Probe the bus ourselves instead of trusting the return.
bool i2cPresent(uint8_t addr) {
  Wire.beginTransmission(addr);
  return Wire.endTransmission() == 0;
}

void i2cScan() {
  Serial.println("[I2C] scanning...");
  uint8_t found = 0;
  for (uint8_t a = 1; a < 127; a++) {
    if (!i2cPresent(a)) continue;
    found++;
    Serial.printf("[I2C]   0x%02X", a);
    if      (a == AS5600_ADDR) Serial.print("  <- AS5600");
    else if (a == 0x3C || a == 0x3D) Serial.print("  <- SSD1306 OLED");
    Serial.println();
  }
  if (!found) Serial.println("[I2C]   nothing responded — check SDA/SCL/power");
}

// ── ESP-NOW ──────────────────────────────────────────────────────────
void onRecv(const esp_now_recv_info_t* info, const uint8_t* data, int len) {
  if (len != (int)sizeof(PuckMsg)) return;
  const PuckMsg* m = (const PuckMsg*)data;
  if (m->ver != PUCK_PROTO_VER) return;

  // OTA is a mode switch that tears down the radio, so it cannot run here on
  // the WiFi task — same rule as esp_now_send. Latch it for puckTick().
  if (m->type == PK_OTA) { otaRequested = true; return; }

  if (m->type != PK_STATE) return;
  padFlags  = m->flags;
  padVolume = m->vol;
  // The pad owns the mode — a key on the pad (or our own button, which only
  // asks the pad to cycle) is the single source of truth.
  if (m->mode < PM_COUNT) mode = m->mode;
  // ...and it owns dial feel too, so Settings can retune without a reflash.
  // Guard the range: a zero here would divide by zero in threshold().
  if (m->speed >= PUCK_SPEED_MIN && m->speed <= PUCK_SPEED_MAX) puckSpeed = m->speed;
  if (m->accel >= PUCK_ACCEL_MIN && m->accel <= PUCK_ACCEL_MAX) puckAccel = m->accel;
  padSeen   = true;
  lastPadMs = millis();
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
  m.vol   = PUCK_VOL_UNKNOWN;   // puck->pad these three are unused, but the
  m.speed = puckSpeed;          // struct is fixed-size and the pad length-checks
  m.accel = puckAccel;          // it, so leaving them uninitialised is garbage
  esp_now_send(PAD_MAC, (uint8_t*)&m, sizeof(m));
}

void setupEspNow() {
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  esp_wifi_set_channel(PUCK_WIFI_CHANNEL, WIFI_SECOND_CHAN_NONE);

  if (esp_now_init() != ESP_OK) {
    Serial.println("[NOW] init failed");
    return;
  }
  esp_now_register_recv_cb(onRecv);

  esp_now_peer_info_t peer;
  memset(&peer, 0, sizeof(peer));
  memcpy(peer.peer_addr, PAD_MAC, 6);
  peer.channel = PUCK_WIFI_CHANNEL;
  peer.encrypt = false;
  if (esp_now_add_peer(&peer) != ESP_OK) {
    Serial.println("[NOW] add_peer failed");
    return;
  }
  peerReady = true;
  Serial.println("[NOW] ready");
}

// ── OTA ──────────────────────────────────────────────────────────────
// Triggered from the pad (Settings -> PUCK -> OTA), which sends PK_OTA. The
// puck has no button and no screen, so the pad is the only sane trigger.
//
// ESP-NOW and an access point cannot share the radio: ESP-NOW runs as an
// unassociated STA pinned to channel 1, and softAP() will not come up cleanly
// underneath it. So this is a one-way door — the radio is torn down and the
// only way back to normal operation is the reboot at the end of the flash (or
// a power cycle if the user changes their mind).
#define OTA_AP_SSID "Puck-Setup"
#define OTA_AP_PASS "puck12345"

WebServer otaServer(80);
bool otaMode = false;

static const char OTA_PAGE[] PROGMEM = R"HTML(<!doctype html><meta charset=utf-8>
<meta name=viewport content="width=device-width,initial-scale=1">
<title>Puck OTA</title><style>
body{font-family:system-ui,sans-serif;background:#111;color:#eee;margin:0;padding:32px}
h1{font-size:18px;margin:0 0 4px}p{color:#888;font-size:13px;margin:0 0 24px}
input[type=file]{display:block;margin-bottom:16px;color:#ccc}
button{background:#3abeff;border:0;color:#000;font-weight:600;padding:10px 18px;border-radius:6px;font-size:14px}
#s{margin-top:16px;font-size:13px;color:#3abeff}</style>
<h1>Encoder Puck</h1><p>Upload puck_encoder.ino.bin</p>
<input type=file id=f accept=.bin><button onclick=go()>Flash</button><div id=s></div>
<script>async function go(){const f=document.getElementById('f').files[0];
if(!f){s.textContent='Pick a .bin first';return}
s.textContent='Uploading '+f.name+'...';const d=new FormData();d.append('f',f);
try{const r=await fetch('/api/update',{method:'POST',body:d});
s.textContent=r.ok?'Done - puck is rebooting':'Failed: '+await r.text()}
catch(e){s.textContent='Upload ended (the puck usually reboots before replying)'}}
</script>)HTML";

void handleOtaRoot() { otaServer.send_P(200, "text/html", OTA_PAGE); }

void handleOtaDone() {
  bool ok = !Update.hasError();
  otaServer.sendHeader("Connection", "close");
  otaServer.send(ok ? 200 : 500, "text/plain", ok ? "OK" : "FAIL");
  delay(300);
  if (ok) ESP.restart();
}

void handleOtaUpload() {
  HTTPUpload& up = otaServer.upload();
  if (up.status == UPLOAD_FILE_START) {
    Serial.printf("[OTA] start %s\n", up.filename.c_str());
    if (!Update.begin(UPDATE_SIZE_UNKNOWN)) Update.printError(Serial);
  } else if (up.status == UPLOAD_FILE_WRITE) {
    if (Update.write(up.buf, up.currentSize) != up.currentSize) Update.printError(Serial);
  } else if (up.status == UPLOAD_FILE_END) {
    if (Update.end(true)) Serial.printf("[OTA] success %u bytes\n", up.totalSize);
    else                  Update.printError(Serial);
  }
}

void enterOtaMode() {
  Serial.println("[OTA] entering — ESP-NOW going down");
  esp_now_deinit();
  peerReady = false;
  WiFi.disconnect(true);
  delay(100);

  WiFi.mode(WIFI_AP);
  WiFi.softAP(OTA_AP_SSID, OTA_AP_PASS);
  delay(200);

  otaServer.on("/", HTTP_GET, handleOtaRoot);
  otaServer.on("/api/update", HTTP_POST, handleOtaDone, handleOtaUpload);
  otaServer.begin();

  otaMode = true;
  Serial.printf("[OTA] AP \"%s\" pass \"%s\" -> http://%s/\n",
                OTA_AP_SSID, OTA_AP_PASS, WiFi.softAPIP().toString().c_str());
  Serial.println("[OTA] the AP disappearing is the success signal");
}

// ── OLED ─────────────────────────────────────────────────────────────
void drawOled() {
  if (!oledOK) return;
#if PUCK_OLED
  lastOledMs = millis();

  unsigned long now = millis();
  bool linkUp = padSeen && (now - lastPadMs < 3000);

  // Idle blank — the puck is battery-friendly and the OLED burns in.
  // Latched, so a blanked screen isn't re-pushed over I2C every 100ms.
  static bool blanked = false;
  if (now - lastActivity > OLED_DIM_MS) {
    if (!blanked) {
      blanked = true;
      oled.clearDisplay();
      oled.display();
    }
    return;
  }
  blanked = false;

  oled.clearDisplay();
  oled.setTextColor(SSD1306_WHITE);
  oled.setTextSize(1);

  // ── Volume bar: shown only while the dial is actually being used, then it
  // falls back to the mode name. Needs a real level from the companion app —
  // BLE HID volume is relative, so without it we have no honest number to
  // draw and showing a made-up bar would be worse than showing none.
  bool muted    = (padFlags & PF_VOL_MUTED) != 0;
  bool volKnown = (padFlags & PF_VOL_KNOWN) && padVolume != PUCK_VOL_UNKNOWN;
  bool showBar  = (mode == PM_VOLUME) && volKnown && magnetPresent
                  && (now - lastActivity < BAR_HOLD_MS);

  if (showBar) {
    uint8_t v = padVolume > 100 ? 100 : padVolume;
    oled.setCursor(0, 0);
    oled.print(muted ? "MUTED" : "VOL");

    char pct[6];
    snprintf(pct, sizeof(pct), "%u%%", v);
    oled.setCursor(OLED_W - (int)strlen(pct) * 6, 0);
    oled.print(pct);

    // Track + fill. A muted system draws the outline only, so the level is
    // still readable but obviously not in effect.
    const int by = (OLED_H >= 64) ? 30 : 14;
    const int bh = (OLED_H >= 64) ? 20 : 16;
    oled.drawRect(0, by, OLED_W, bh, SSD1306_WHITE);
    if (!muted && v > 0)
      oled.fillRect(2, by + 2, ((OLED_W - 4) * v) / 100, bh - 4, SSD1306_WHITE);

    oled.display();
    return;
  }

  // ── Idle: link state + mode name
  oled.setCursor(0, 0);
  if (!linkUp)                          oled.print("NO PAD");
  else if (padFlags & PF_DISABLED)      oled.print("PAD: OFF");
  else if (!(padFlags & PF_BLE_UP))     oled.print("PAD: NO BLE");
  else                                  oled.print("LINKED");

  // Link dot, top right
  if (linkUp) oled.fillCircle(122, 3, 3, SSD1306_WHITE);
  else        oled.drawCircle(122, 3, 3, SSD1306_WHITE);

  oled.drawFastHLine(0, 10, 128, SSD1306_WHITE);

  // The centre line is the one thing worth the big font. Priority order:
  // a missing magnet explains a dead knob, so it outranks everything; a
  // just-fired action is worth a brief flash; otherwise show the mode.
  const char* big = MODE_NAMES[mode];
  bool small = false;
  if (!magnetPresent)                                    { big = "NO MAGNET"; small = true; }
  else if (now - lastActionMs < 1000 && lastAction[0])   { big = lastAction; }

#if OLED_H >= 64
  oled.setTextSize(2);
  oled.setCursor((OLED_W - (int)strlen(big) * 12) / 2, 24);
  oled.print(big);

  oled.drawFastHLine(0, 55, 128, SSD1306_WHITE);
  oled.setTextSize(1);
  oled.setCursor(0, 57);
  oled.print("hold=mode  click=play");
#else
  // 128x32: four 8px pages total. Header takes one, the rule sits on the
  // second, and size-2 text (16px) fills the rest exactly. No room for the
  // footer hint, and "NO MAGNET" only fits at size 1.
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
#endif  // PUCK_OLED
}

void note(const char* s) { lastAction = s; lastActionMs = millis(); }

// ── Button gestures ──────────────────────────────────────────────────
void handleButton(unsigned long now) {
  bool raw = (digitalRead(PIN_BTN) == LOW);

  if (raw != btnLastRaw) { btnLastRaw = raw; btnEdgeMs = now; }

  if (raw != btnStable && (now - btnEdgeMs) > DEBOUNCE_MS) {
    btnStable = raw;
    if (btnStable) {                       // ── pressed
      btnDownMs = now;
      longFired = false;
    } else {                               // ── released
      if (!longFired) {
        if (clickPending && (now - pendingClickMs) < DOUBLE_GAP_MS) {
          clickPending = false;
          sendMsg(PK_INPUT, 0, BTN_DOUBLE);
          note("NEXT");
          lastActivity = now;
        } else {
          clickPending   = true;           // wait — might become a double
          pendingClickMs = now;
        }
      }
    }
  }

  // Long press fires while still held, so it feels immediate. We only ask the
  // pad to cycle — it owns the mode and sends the new one back in PK_STATE.
  if (btnStable && !longFired && (now - btnDownMs) > LONG_PRESS_MS) {
    longFired    = true;
    clickPending = false;
    sendMsg(PK_INPUT, 0, BTN_LONG);
    note("MODE");
    lastActivity = now;
  }

  // A click that never doubled resolves as a single
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
  // Native USB CDC only exists once the host enumerates it. Without this the
  // whole boot report is written into the void before the port opens.
  unsigned long t0 = millis();
  while (!Serial && millis() - t0 < 2000) delay(10);
  delay(200);
  Serial.println("\n=== MacroPad Encoder Puck ===");

  pinMode(PIN_BTN, INPUT_PULLUP);

  Wire.begin(PIN_SDA, PIN_SCL);
  Wire.setClock(400000);
  Serial.printf("[I2C] SDA=GPIO%d  SCL=GPIO%d   BTN=GPIO%d\n",
                PIN_SDA, PIN_SCL, PIN_BTN);
  i2cScan();

#if PUCK_OLED
  // Probe first, then init — begin() cannot tell us this itself
  uint8_t oledAddr = i2cPresent(0x3C) ? 0x3C : (i2cPresent(0x3D) ? 0x3D : 0);
  if (oledAddr && oled.begin(SSD1306_SWITCHCAPVCC, oledAddr)) {
    oledOK = true;
    Serial.printf("[OLED] found at 0x%02X\n", oledAddr);
    oled.clearDisplay();
    oled.setTextSize(1);
    oled.setTextColor(SSD1306_WHITE);
    oled.setCursor(0, 0);
    oled.println("MacroPad Puck");
    oled.println("starting...");
    oled.display();
  } else {
    Serial.println("[OLED] NOT WIRED — no ACK at 0x3C or 0x3D");
  }
#else
  Serial.println("[OLED] compiled out (PUCK_OLED 0) — mode shows on the pad");
#endif

  if (!i2cPresent(AS5600_ADDR)) {
    Serial.println("[AS5600] NOT WIRED — no ACK at 0x36");
  } else {
    magnetPresent = magnetOK();
    Serial.println(magnetPresent ? "[AS5600] ok, magnet detected"
                                 : "[AS5600] ok, but NO MAGNET in range");
  }

  Serial.printf("[BTN] GPIO%d reads %s (should be HIGH when not pressed)\n",
                PIN_BTN, digitalRead(PIN_BTN) ? "HIGH" : "LOW");

  setupEspNow();

  // WiFi.macAddress() returns all zeros until the driver is started, so this
  // has to come after setupEspNow()'s WiFi.mode(WIFI_STA), not before it.
  Serial.print("[NOW] this puck MAC: ");
  Serial.println(WiFi.macAddress());
  Serial.print("[NOW] target pad MAC: ");
  for (int i = 0; i < 6; i++) { Serial.printf("%02X", PAD_MAC[i]); if (i < 5) Serial.print(":"); }
  Serial.println();

  lastAngle    = (int16_t)readAngle();
  lastReadMs   = millis();
  lastActivity = millis();

  sendMsg(PK_HELLO, 0, BTN_NONE);
  drawOled();
}

// ── Loop ─────────────────────────────────────────────────────────────
void loop() {
  unsigned long now = millis();

  // ── OTA ───────────────────────────────────────────────────
  // Once we're in OTA mode the radio belongs to the AP and there is no ESP-NOW
  // left to talk to, so nothing below this point can usefully run. Serving the
  // upload is the whole job until the flash reboots us.
  if (otaMode) { otaServer.handleClient(); return; }
  if (otaRequested) { otaRequested = false; enterOtaMode(); return; }

  // ── Encoder ───────────────────────────────────────────────
  if (now - lastReadMs >= READ_INTERVAL_MS) {
    unsigned long dt = now - lastReadMs;
    lastReadMs = now;

    int16_t angle = (int16_t)readAngle();
    int16_t delta = angle - lastAngle;
    if (delta >  2048) delta -= 4096;      // 12-bit wraparound
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
  // A fast spin becomes a few packets, not hundreds — this is what keeps
  // the pad's BLE link stable while its WiFi radio is up.
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
    // Re-check the magnet occasionally — it can be knocked out of range
    magnetPresent = magnetOK();
  }

  // ── OLED refresh ──────────────────────────────────────────
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

  delay(1);
}
