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
#include <esp_sleep.h>
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

// WiFi TX power, in quarter-dBm (esp_wifi_set_max_tx_power's unit). 44 = 11 dBm
// against a 20 dBm default. The puck sits a metre or two from the pad, so the
// extra 9 dB buys nothing but current: full power peaks around 335 mA, which
// overruns a 250-300 mA LDO and shows up as random brownout resets during
// transmission — a fault that reads like a firmware bug and wastes a day.
// Raise it if the link ever gets unreliable at range.
#define PUCK_TX_POWER_QDBM 44

// Which way the knob counts. Depends purely on how the magnet ends up facing
// once the dial is assembled, so it is a build-time property of the physical
// puck, not a preference. 1 = reverse the sense of rotation.
//
// The AS5600 also has a hardware DIR pin for this (tie to GND or VCC), but the
// SuperMini wiring leaves it grounded, so flipping it in firmware avoids
// touching the board.
// Verified on the assembled dial 2026-08-02: 0 is the correct sense.
#define PUCK_DIR_INVERT 0

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
// A dozing puck has its radio off and will miss a one-shot PK_OTA entirely, so
// the pad also latches the request into every PK_STATE. The puck picks it up on
// the next keepalive burst — worst case DOZE_HELLO_MS away.
#define PF_OTA_PEND   0x20

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

// ── Power / sleep ────────────────────────────────────────────────────
// The radio is by far the biggest load: an unassociated STA sits at ~80-100 mA
// continuously, against ~1 mA for a dozing CPU. Since the OLED was dropped the
// puck has nothing to render, so it no longer needs to be listening all the
// time — it only needs the pad's reply to the packets it sends. That makes it
// safe to keep the radio off until the knob actually moves.
//
// DOZE: radio stopped, CPU in light sleep, waking every SLEEP_POLL_MS to read
//       the encoder over I2C. Movement past one detent promotes to ACTIVE.
// ACTIVE: radio up, the original 10 ms / 40 ms polling and coalescing.
//
// Unassociated STA cannot use WiFi modem sleep (no AP beacon to sync against),
// so stopping the driver outright is the only way to put the radio down.
#define PUCK_SLEEP 1

const unsigned long SLEEP_AFTER_MS   = 4000;   // idle before dropping to DOZE
const unsigned long SLEEP_POLL_MS    = 50;     // light-sleep slice while dozing
// Upper bound on how long to hold the radio up waiting for the pad's reply.
// The wait exits as soon as a PK_STATE actually lands, so the usual cost is a
// few tens of ms — this is only the ceiling for when the pad is slow. It has to
// be generous: the pad answers from its main loop, and a full TFT redraw there
// can take well over 100 ms. At 80 ms the reply was routinely missed, which
// silently dropped mode, speed, accel and the OTA request.
const unsigned long RADIO_LISTEN_MS  = 400;

// ── Link recovery ────────────────────────────────────────────────────
// The puck must never need a power cycle to come back. If the pad is rebooted,
// or was off long enough for its own sleep to drop ESP-NOW, the puck can end up
// talking into a void — and a wedged WiFi driver looks identical to a pad that
// is simply switched off. So escalate: rebuild the radio stack first, and if
// that still yields nothing, restart outright. A puck with no UI can reboot
// itself whenever it likes; nobody sees it.
const unsigned long LINK_REBUILD_MS = 300000UL;   // 5 min silent -> rebuild ESP-NOW
const unsigned long LINK_DEAD_MS    = 1800000UL;  // 30 min silent -> self-restart
const unsigned long LINK_SLEEP_MS   = 3600000UL;  // 1 hour silent -> deep sleep

// ── Deep sleep / spin-to-wake ────────────────────────────────────────
// After an hour with no pad there is nothing worth staying up for: the pad is
// off, and keepalives are just burning the cell. Deep sleep, and require a
// deliberate gesture to come back.
//
// The AS5600 cannot raise an interrupt on movement — it is a plain I2C angle
// sensor with no "something moved" output — so waking on rotation means waking
// on a timer and looking. Each wake reads the angle, compares it with the one
// stored in RTC memory, and counts it as movement if it shifted meaningfully.
// WAKE_MOVES_NEEDED consecutive moving samples means a human is spinning it,
// not a knock or a temperature drift, and only then does the puck boot for real.
const uint64_t DEEP_POLL_US        = 3000000ULL;  // look every 3 s
const int      WAKE_MOVE_THRESHOLD = 200;         // counts (~18 deg) = real movement
const uint32_t WAKE_MOVES_NEEDED   = 3;           // consecutive samples before waking

// ── Spin to reboot ───────────────────────────────────────────────────
// The puck is sealed inside a printed enclosure — there is no reset button to
// reach without taking it apart. So while it is NOT linked, a hard spin becomes
// the reset button: enough detents inside a short window and it restarts into a
// clean link attempt, radio and all.
//
// Gated strictly on being unlinked, so a fast volume sweep during normal use
// can never trigger it. That gate is the whole reason this is safe: spinning
// hard is completely ordinary when the dial is working.
const uint16_t      RELINK_SPIN_TICKS     = 120;    // ~2-3 turns at 50/turn
const unsigned long RELINK_SPIN_WINDOW_MS = 6000;   // ...within this long
const unsigned long RELINK_UNLINKED_MS    = 30000;  // "not linked" means this quiet

// OTA is otherwise a one-way door: ESP-NOW is gone and only a flash or a power
// cycle brings the puck back. If nobody uploads, come back on our own.
const unsigned long OTA_IDLE_TIMEOUT_MS = 300000UL;   // 5 min

// ── Doze keepalive ───────────────────────────────────────────────────
// Its only job now is feeding the pad's link indicator — the pad must not
// decide the puck is gone while it is merely asleep, so PUCK_LINK_TIMEOUT_MS
// on the pad has to stay comfortably longer than this.
//
// History worth keeping, because the symptom is baffling if you meet it cold:
// the puck was originally powered LiPo -> 5 V power-bank boost -> AMS1117, and
// those boost modules cut their output when they see too little load for too
// long (typically under 40-70 mA). A dozing puck draws single-digit mA, so the
// module decided nothing was plugged in and shut down, killing the puck. There
// is NO firmware recovery from that — with the output off the C3 has no power
// at all, so it cannot pulse the module's button pad or signal anything.
//
// The workaround was to make this keepalive double as a load pulse: hold the
// radio up (~100 mA) long enough and often enough to keep resetting the
// module's low-load timer. It cost real runtime and was only ever a bridge.
//
// Fixed properly in hardware instead: the C3 is now fed straight from the cell
// into the SuperMini's own low-dropout regulator, so the boost module is out of
// the discharge path and does charging only. No minimum load to satisfy, and
// the ~43% conversion loss of boost-plus-LDO went with it. BMS_PULSE_MS is kept
// short for that reason — restore it to ~250 ms only if a min-load supply ever
// comes back.
const unsigned long DOZE_HELLO_MS    = 60000;  // 5000 -> 60000 after the rewire
const unsigned long BMS_PULSE_MS     = RADIO_LISTEN_MS;

// A serial monitor holds the native USB CDC link open, and light sleep powers
// down the USB Serial/JTAG peripheral — the port stays enumerated but stops
// responding, so opening it fails with "device is not functioning". Skip
// sleeping while a host has the port open. On battery there is no host, so the
// real device sleeps normally; this only affects bench debugging.
#define PUCK_SLEEP_SKIP_ON_USB 1

// ...but that check alone is not enough. Straight after a flash or power-up
// nothing has opened the port yet, so `Serial` is false, the puck dozes at
// SLEEP_AFTER_MS and takes USB down before anyone can attach — leaving a board
// you cannot open a monitor on at all. This grace period keeps it awake long
// enough to connect; after that the skip-on-USB check takes over.
// Set to 0 on a software restart — see setup(). A watchdog reboot that then sat
// awake for 30 s would burn ~1 mAh of radio each time, which on a pad that is
// simply switched off overnight adds up to more than the doze ever saved.
// Only a real power-on or button reset means a human is present to attach.
unsigned long sleepGraceMs = 30000;

// AS5600 power modes (CONF register 0x08, bits 1:0). The sensor is always-on
// and, once the radio is down, becomes the largest single draw on the 3V3 rail.
// LPM2 polls internally every 20 ms, which is still quicker than our own
// SLEEP_POLL_MS, so dozing in LPM2 costs no responsiveness.
#define AS_PM_NOM   0x00   // 6.5 mA — continuous
#define AS_PM_LPM1  0x01   // 3.4 mA — 5 ms internal polling
#define AS_PM_LPM2  0x02   // 1.8 mA — 20 ms
#define AS_PM_LPM3  0x03   // 1.5 mA — 100 ms, too slow to catch a fast turn
#define REG_CONF_LO 0x08

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

// Set while returning from an OTA session. The pad latches its OTA request for
// a while, so without this the puck comes back, immediately hears the same
// request, and reboots again — an endless loop that only ends when the pad's
// latch expires. Declared here because onRecv() below reads it, and Arduino
// hoists function prototypes but not variables.
unsigned long otaIgnoreUntilMs = 0;

// RTC_NOINIT, not RTC_DATA. RTC_DATA_ATTR only survives *deep sleep* — the
// bootloader reinitialises .rtc.data from flash on an ordinary software reset,
// so a flag stored there is already back to zero by the time setup() looks at
// it. .rtc.noinit is left alone, which is the lifetime we actually want:
// survives both a software reset and deep sleep, garbage after a power cycle
// (handled in setup).
RTC_NOINIT_ATTR uint32_t otaBootFlag;
RTC_NOINIT_ATTR uint32_t otaCooldownFlag;
#define OTA_BOOT_MAGIC 0xC3040A5Au

RTC_NOINIT_ATTR uint32_t deepSleepFlag;
RTC_NOINIT_ATTR int32_t  deepLastAngle;
RTC_NOINIT_ATTR uint32_t deepMoveCount;
#define DEEP_SLEEP_MAGIC 0xD3EF5117u

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

// Read-modify-write: CONF's low byte also holds hysteresis, output stage, PWM
// frequency and the slow filter. Clobbering it with a bare write would silently
// change the filtering along with the power mode.
void as5600SetPower(uint8_t pm) {
  Wire.beginTransmission(AS5600_ADDR);
  Wire.write(REG_CONF_LO);
  if (Wire.endTransmission(false) != 0) return;
  Wire.requestFrom((uint8_t)AS5600_ADDR, (uint8_t)1);
  if (!Wire.available()) return;
  uint8_t v = Wire.read();
  uint8_t want = (uint8_t)((v & ~0x03) | (pm & 0x03));
  if (want == v) return;                     // already there — skip the write
  Wire.beginTransmission(AS5600_ADDR);
  Wire.write(REG_CONF_LO);
  Wire.write(want);
  Wire.endTransmission();
}

// Wrapped, direction-corrected delta since the last call. Both the active loop
// and the doze poll go through here, so they cannot disagree about sign — they
// share one accumulator across the sleep boundary, and a mismatch there would
// show up as the dial jumping backwards on the first detent after waking.
int16_t readDelta() {
  int16_t angle = (int16_t)readAngle();
  int16_t d = angle - lastAngle;
  if (d >  2048) d -= 4096;              // 12-bit wraparound
  if (d < -2048) d += 4096;
  lastAngle = angle;
#if PUCK_DIR_INVERT
  d = (int16_t)-d;
#endif
  return d;
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
  // Latched request — but not while cooling down from a session we just left.
  if ((m->flags & PF_OTA_PEND) && millis() >= otaIgnoreUntilMs) otaRequested = true;
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
  esp_wifi_set_max_tx_power(PUCK_TX_POWER_QDBM);

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

// ── Radio power ──────────────────────────────────────────────────────
// esp_wifi_stop() takes the RF down without tearing down ESP-NOW's registration.
// The peer table does not reliably survive a stop/start though, so re-add on the
// way up — esp_now_add_peer on an existing peer returns an error we can ignore,
// and the check keeps the log clean.
bool radioUp = true;

void radioOn() {
  if (radioUp) return;
  if (esp_wifi_start() != ESP_OK) { Serial.println("[NOW] wifi start failed"); return; }
  esp_wifi_set_channel(PUCK_WIFI_CHANNEL, WIFI_SECOND_CHAN_NONE);
  esp_wifi_set_max_tx_power(PUCK_TX_POWER_QDBM);   // reset by every wifi start
  if (!esp_now_is_peer_exist(PAD_MAC)) {
    esp_now_peer_info_t peer;
    memset(&peer, 0, sizeof(peer));
    memcpy(peer.peer_addr, PAD_MAC, 6);
    peer.channel = PUCK_WIFI_CHANNEL;
    peer.encrypt = false;
    esp_now_add_peer(&peer);
  }
  radioUp   = true;
  peerReady = true;
}

void radioOff() {
  if (!radioUp) return;
  esp_wifi_stop();
  radioUp   = false;
  peerReady = false;      // sendMsg() is a no-op with the radio down
}

// Full teardown and rebuild. radioOn() only restarts the driver; if ESP-NOW
// itself has wedged — callback lost, peer table gone, interface confused — that
// is not enough, and the symptom is a puck that transmits into nothing forever.
void rebuildEspNow() {
  Serial.println("[NOW] rebuilding — no pad contact");
  esp_now_deinit();
  esp_wifi_stop();
  delay(50);
  esp_wifi_start();
  esp_wifi_set_channel(PUCK_WIFI_CHANNEL, WIFI_SECOND_CHAN_NONE);
  esp_wifi_set_max_tx_power(PUCK_TX_POWER_QDBM);

  if (esp_now_init() != ESP_OK) { Serial.println("[NOW] re-init failed"); return; }
  esp_now_register_recv_cb(onRecv);

  esp_now_peer_info_t peer;
  memset(&peer, 0, sizeof(peer));
  memcpy(peer.peer_addr, PAD_MAC, 6);
  peer.channel = PUCK_WIFI_CHANNEL;
  peer.encrypt = false;
  esp_now_add_peer(&peer);

  radioUp   = true;
  peerReady = true;
  sendMsg(PK_HELLO, 0, BTN_NONE);
}

// Escalating recovery, run from both the active loop and the doze poll.
// Deliberately only arms once the puck has heard from the pad at least once —
// on a bench puck with no pad in the room, restarting every 30 minutes forever
// would be pointless noise.
// Park until somebody spins the dial. Radio down, AS5600 into its deepest low
// power mode, CPU off but for a 3 s tick.
void enterDeepSleep() {
  Serial.println("[SYS] no pad for an hour — deep sleep, spin the dial to wake");
  delay(30);
  radioOff();
  as5600SetPower(AS_PM_LPM3);
  deepSleepFlag = DEEP_SLEEP_MAGIC;
  deepLastAngle = (int32_t)readAngle();
  deepMoveCount = 0;
  esp_sleep_enable_timer_wakeup(DEEP_POLL_US);
  esp_deep_sleep_start();
}

// Counts detents while the link is down; a big enough burst restarts the puck.
void relinkSpinCheck(unsigned long now, int magnitude) {
  static unsigned long winStart = 0;
  static uint16_t      spun     = 0;

  const bool unlinked = !padSeen || (now - lastPadMs > RELINK_UNLINKED_MS);
  if (!unlinked) { spun = 0; return; }          // linked: never arm

  if (now - winStart > RELINK_SPIN_WINDOW_MS) { winStart = now; spun = 0; }
  spun += (uint16_t)magnitude;

  if (spun >= RELINK_SPIN_TICKS) {
    Serial.println("[SYS] spin-to-reboot (unlinked) — restarting");
    delay(20);
    ESP.restart();
  }
}

void linkWatchdog(unsigned long now) {
  static unsigned long lastRebuildMs = 0;
  if (!padSeen) return;
  const unsigned long silent = now - lastPadMs;

  if (silent >= LINK_SLEEP_MS) enterDeepSleep();   // does not return

  if (silent >= LINK_DEAD_MS) {
    Serial.println("[NOW] link dead — restarting");
    delay(20);
    ESP.restart();
  }
  if (silent >= LINK_REBUILD_MS && (now - lastRebuildMs) >= LINK_REBUILD_MS) {
    lastRebuildMs = now;
    rebuildEspNow();
  }
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
unsigned long otaTouchMs = 0;   // last sign of life from an uploader

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
  otaTouchMs = millis();               // keep the idle timeout off our back
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



// Requesting OTA reboots rather than switching mode in place.
//
// The doze cycle drives the radio with raw esp_wifi_stop()/esp_wifi_start(),
// but WiFi.softAP() goes through the Arduino WiFiGeneric wrapper, which keeps
// its own view of whether the driver is started. Changing that state behind the
// wrapper's back desyncs it, and softAP() then silently does nothing — ESP-NOW
// tears down, the pad loses the puck, and no access point ever appears. Which
// is precisely the symptom this fixes.
//
// Rebooting sidesteps the whole problem: the AP comes up from a cold, coherent
// stack on the next boot, with no wrapper state to disagree with.
void enterOtaMode() {
  Serial.println("[OTA] requested — rebooting into AP mode");
  otaBootFlag = OTA_BOOT_MAGIC;
  delay(30);
  ESP.restart();
}

// Runs from setup() on an OTA boot, before any of the ESP-NOW path.
void startOtaAp() {
  WiFi.mode(WIFI_AP);
  WiFi.softAP(OTA_AP_SSID, OTA_AP_PASS);
  delay(200);

  otaServer.on("/", HTTP_GET, handleOtaRoot);
  otaServer.on("/api/update", HTTP_POST, handleOtaDone, handleOtaUpload);
  otaServer.begin();

  otaMode    = true;
  otaTouchMs = millis();
  Serial.printf("[OTA] AP \"%s\" pass \"%s\" -> http://%s/\n",
                OTA_AP_SSID, OTA_AP_PASS, WiFi.softAPIP().toString().c_str());
  Serial.println("[OTA] the AP disappearing is the success signal");
}

// One deep-sleep tick. Called from setup() after Wire is up, on a timer wake.
// Either returns (movement confirmed — carry on booting and try to link) or
// goes straight back to sleep without ever bringing the radio up.
void handleDeepWake() {
  int32_t angle = (int32_t)readAngle();
  int32_t d = angle - deepLastAngle;
  if (d >  2048) d -= 4096;                 // wraparound, same as readDelta()
  if (d < -2048) d += 4096;
  deepLastAngle = angle;

  if (abs((int)d) >= WAKE_MOVE_THRESHOLD) deepMoveCount++;
  else                                    deepMoveCount = 0;   // must be consecutive

  if (deepMoveCount >= WAKE_MOVES_NEEDED) {
    deepSleepFlag = 0;
    deepMoveCount = 0;
    as5600SetPower(AS_PM_NOM);
    Serial.println("[SYS] spin detected — waking up to re-link");
    return;                                  // fall through into a normal boot
  }

  esp_sleep_enable_timer_wakeup(DEEP_POLL_US);
  esp_deep_sleep_start();
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

  // A self-restart (link watchdog, OTA timeout) has no human waiting on it, so
  // skip the grace window and let it doze straight away.
  esp_reset_reason_t rr = esp_reset_reason();
  if (rr == ESP_RST_SW) sleepGraceMs = 0;
  Serial.printf("[SYS] reset reason %d, sleep grace %lu ms\n", (int)rr, sleepGraceMs);

  // .rtc.noinit is uninitialised garbage after a power cycle, so only believe it
  // when we arrived by a route that preserves it — a software reset or a deep
  // sleep wake. Anything else (power-on, brownout, watchdog) starts clean.
  if (rr != ESP_RST_SW && rr != ESP_RST_DEEPSLEEP) {
    otaBootFlag = otaCooldownFlag = 0;
    deepSleepFlag = deepMoveCount = 0;
  }

  // OTA boot: bring the access point up on a clean stack and do nothing else.
  // Cleared first, so a failed OTA boot cannot trap the puck in a reboot loop —
  // worst case it comes back as a normal puck.
  if (otaBootFlag == OTA_BOOT_MAGIC) {
    otaBootFlag     = 0;
    otaCooldownFlag = OTA_BOOT_MAGIC;   // ...so the next normal boot backs off
    startOtaAp();
    return;
  }

  // Coming back from OTA. Ignore the pad's still-latched request for a while,
  // otherwise we bounce straight back into AP mode.
  if (otaCooldownFlag == OTA_BOOT_MAGIC) {
    otaCooldownFlag  = 0;
    otaIgnoreUntilMs = 120000;
    Serial.println("[OTA] back from AP mode — ignoring OTA requests for 120 s");
  }

  pinMode(PIN_BTN, INPUT_PULLUP);

  Wire.begin(PIN_SDA, PIN_SCL);
  Wire.setClock(400000);

  // Parked waiting for a spin: check and go straight back down. Deliberately
  // before the banner and the bus scan — every millisecond awake here is
  // battery, and this path runs every DEEP_POLL_US around the clock.
  if (deepSleepFlag == DEEP_SLEEP_MAGIC) handleDeepWake();

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

// ── Doze ─────────────────────────────────────────────────────────────
#if PUCK_SLEEP
bool dozing = false;

void enterDoze() {
  if (dozing) return;
  // Never leave ticks stranded — they would surface as a jump on the next turn.
  if (pendingTicks != 0) {
    sendMsg(PK_INPUT, (int8_t)pendingTicks, BTN_NONE);
    pendingTicks = 0;
    delay(10);                       // let it actually go out before the radio dies
  }
  radioOff();
  as5600SetPower(AS_PM_LPM2);
  dozing = true;
}

void exitDoze() {
  if (!dozing) return;
  dozing = false;
  as5600SetPower(AS_PM_NOM);
  radioOn();
  lastReadMs   = millis();           // don't let the doze gap look like one huge dt
  lastActivity = millis();
}

// One radio window: up, send, hold briefly for the pad's reply, back down.
// The pad answers any packet it receives, so this is also how mode, speed and
// accel stay in sync without ever idling with the radio on.
void radioBurst(uint8_t type, int8_t ticks, unsigned long holdMs) {
  radioOn();
  const unsigned long wasPadMs = lastPadMs;
  sendMsg(type, ticks, BTN_NONE);
  // Wait for the reply, not for the clock. Leaving early on the common case is
  // what keeps this affordable — holding the full window every beat would cost
  // more than the doze saves.
  unsigned long t0 = millis();
  while (millis() - t0 < holdMs) {
    if (lastPadMs != wasPadMs) break;      // PK_STATE landed
    delay(1);
  }
  if (otaRequested) return;          // caller handles it; leave the radio up
  radioOff();
}

void dozeTick() {
  esp_sleep_enable_timer_wakeup((uint64_t)SLEEP_POLL_MS * 1000ULL);
#if PUCK_SLEEP_SKIP_ON_USB
  if (Serial) delay(SLEEP_POLL_MS);  // host attached — burn the slice awake
  else        esp_light_sleep_start();
#else
  esp_light_sleep_start();
#endif

  accumulator += readDelta();

  // One full detent of real movement is the wake threshold. Using the same
  // accumulator as the active path means sensor noise, which never integrates
  // that far, cannot rattle the puck awake.
  if (abs(accumulator) >= threshold()) { exitDoze(); return; }

  if (digitalRead(PIN_BTN) == LOW) { exitDoze(); return; }

  // Keepalive + BMS load pulse. Held longer than a plain reply window so the
  // boost module reliably sees the current and resets its low-load timer.
  if (millis() - lastHelloMs >= DOZE_HELLO_MS) {
    lastHelloMs = millis();
    magnetPresent = magnetOK();
    radioBurst(PK_HELLO, 0, BMS_PULSE_MS);
    linkWatchdog(millis());
    // A rebuild leaves the radio up; we are still dozing, so put it back down.
    if (radioUp && !otaRequested) radioOff();
  }
}
#endif  // PUCK_SLEEP

// ── Loop ─────────────────────────────────────────────────────────────
void loop() {
  unsigned long now = millis();

  // ── OTA ───────────────────────────────────────────────────
  // Once we're in OTA mode the radio belongs to the AP and there is no ESP-NOW
  // left to talk to, so nothing below this point can usefully run. Serving the
  // upload is the whole job until the flash reboots us.
  if (otaMode) {
    otaServer.handleClient();
    // Come back on our own if nobody uploads. otaTouchMs is pushed forward by
    // every chunk received, so a slow upload can never trip this mid-flash.
    if (millis() - otaTouchMs >= OTA_IDLE_TIMEOUT_MS) {
      Serial.println("[OTA] idle timeout — restarting into normal mode");
      delay(20);
      ESP.restart();
    }
    return;
  }
  if (otaRequested) {
    otaRequested = false;
#if PUCK_SLEEP
    dozing = false;
    radioOn();                       // the AP needs the driver running
#endif
    enterOtaMode();
    return;
  }

#if PUCK_SLEEP
  if (dozing) { dozeTick(); return; }
#endif

  // ── Encoder ───────────────────────────────────────────────
  if (now - lastReadMs >= READ_INTERVAL_MS) {
    unsigned long dt = now - lastReadMs;
    lastReadMs = now;

    int16_t delta = readDelta();

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
        relinkSpinCheck(now, abs(ticks));
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

  linkWatchdog(now);

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
      Serial.printf("[PAD] mode=%s ble=%d scroll=%d off=%d ota=%d vol=",
                    MODE_NAMES[mode < PM_COUNT ? mode : 0],
                    (padFlags & PF_BLE_UP)    ? 1 : 0,
                    (padFlags & PF_SCROLL_OK) ? 1 : 0,
                    (padFlags & PF_DISABLED)  ? 1 : 0,
                    (padFlags & PF_OTA_PEND)  ? 1 : 0);
      if (padFlags & PF_VOL_KNOWN) Serial.printf("%u%%%s\n", padVolume,
                                                 (padFlags & PF_VOL_MUTED) ? " MUTED" : "");
      else                         Serial.println(F("unknown (companion not running?)"));
    }
  }

#if PUCK_SLEEP
  // Idle long enough with nothing queued — drop the radio. The delay before
  // dozing exists so a pause mid-turn doesn't cost a radio restart on the very
  // next detent; restarting costs ~80 ms and would read as lag.
  if (pendingTicks == 0 && (now - lastActivity) >= SLEEP_AFTER_MS
      && now >= sleepGraceMs) enterDoze();
#endif

  delay(1);
}
