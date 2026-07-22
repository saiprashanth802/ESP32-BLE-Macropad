/*********************************************************************
 * ESP32 BLE HID Macro Pad — v5 MULTIHOST (NimBLE)
 *
 * Hardware : NodeMCU ESP32-S V1.1 (WROOM-32, no PSRAM)
 *            ST7789 240×320 IPS, landscape mount (320×240)
 *            12× MX switches, 4×3 electrical matrix, 1N4148 diodes
 *            (AS5600 encoder puck is a separate ESP-NOW device — NOT
 *             handled by this firmware yet)
 *
 * Wiring (P-label = GPIO) — AS BUILT, verified by pairwise probe:
 *   TFT  : SCK=P18  MOSI=P23  CS=P5  DC=P21  RST=P22  BL=P19 (LEDC PWM)
 *   Matrix rows, driven LOW one at a time (top→bottom): P25 P32 P33
 *   Matrix cols, INPUT_PULLUP readers (left→right)    : P26 P14 P27 P13
 *   Diodes: cathode faces the ROW line (current col → switch → row)
 *
 * Key grid (logical, landscape, 4 wide × 3 tall):
 *   K1  K2  K3  K4        idx 0..3   = drive row d, read col j
 *   K5  K6  K7  K8        idx 4..7     maps as  idx = d*4 + j
 *   K9  K10 K11 K12       idx 8..11    (K9 = FN, bottom-left)
 *
 * ── PEBBLE-KEYS-STYLE MULTI-HOST BLE ─────────────────────────────
 *   3 host slots, each remembers one bonded device (NVS persisted).
 *   Switching re-advertises filtered to that slot's bonded host, so
 *   only the selected device reconnects — like Logitech Easy-Switch.
 *
 * ── CONTROLS ─────────────────────────────────────────────────────
 *   K9 (FN) tap          : fire its macro (like any key)
 *   K9 (FN) hold 1s      : SYSTEM menu, then while it is open:
 *       K1/K2/K3 tap     :   switch to device slot 1/2/3
 *       K1/K2/K3 hold 1.5s:  (re-)pair that slot with a new device
 *       K4               :   preset picker
 *       K8               :   settings  (brightness / sleep / OS /
 *                            devices+bond management)
 *       K12              :   BUILD mode (remap any key live)
 *       release FN       :   cancel
 *
 * Gotchas honoured (from CONTEXT.md §3 / v4 learnings):
 *   ▸ NimBLE init BEFORE ledcAttach — radio init resets LEDC
 *   ▸ BlueZ needs adv flags 0x06 rebuilt on EVERY adv restart
 *   ▸ ledcAttach again after light-sleep — LEDC silently dropped
 *   ▸ strapping pins 0/2/12/15 completely unused
 *********************************************************************/

// Network stack FIRST — TFT_eSPI (SMOOTH_FONT) includes <FS.h> and trips its
// include guard, which would hide the global `FS` alias WebServer.h needs.
#include <WiFi.h>
#include <FS.h>
#include <SPIFFS.h>
#include <WebServer.h>
#include <Update.h>
#include <ArduinoJson.h>
#include <AnimatedGIF.h>
#include <NimBLEDevice.h>
#include <NimBLEHIDDevice.h>
#include <TFT_eSPI.h>
#include <SPI.h>
#include <Preferences.h>
#include <driver/gpio.h>

// ════════════════════════════════════════════════
//  CONFIG / OTA — versioned contract for the companion app
//  Config Mode (Settings → CONFIG) suspends BLE, raises a SoftAP web
//  server exposing a JSON config API + firmware upload. WiFi and BLE
//  never run at once (WROOM-32 coexistence is unstable), so entering
//  and leaving Config Mode both go through a clean reboot.
// ════════════════════════════════════════════════
#define FW_VERSION   "v5-multihost"
#define CONFIG_API   3            // bump when the JSON schema changes
#define AP_SSID      "MacroPad-Setup"
#define AP_PASS      "macropad123" // WPA2 needs >=8 chars; changeable via API
#define AP_IP_STR    "192.168.4.1"

// ════════════════════════════════════════════════
//  PALETTE (RGB565)
// ════════════════════════════════════════════════
#define C_BG       0x0841
#define C_SURF     0x10A3
#define C_SURF2    0x18E6
#define C_BORDER   0x2965
#define C_DIM      0x4A69
#define C_WHITE    0xFFFF
#define C_CYAN     0x07FF
#define C_AMBER    0xFD20
#define C_MAGENTA  0xF81F
#define C_GREEN    0x07E0
#define C_RED      0xF800
#define C_YELLOW   0xFFE0
#define C_PINK     0xFBB7
#define C_LTBLUE   0x3D9F
#define C_ORANGE   0xFC60
#define C_BUILD    0x9F1F

// RAM (not const): the companion app may recolor presets live over the
// host-link (HCMD_COLOR). Not persisted — the app re-pushes every session.
uint16_t PRESET_COLORS[8] = {
  C_CYAN, C_GREEN, C_AMBER, C_MAGENTA,
  C_ORANGE, C_LTBLUE, C_PINK, C_YELLOW,
};

// ════════════════════════════════════════════════
//  PINS  (NodeMCU ESP32-S V1.1 — see header)
// ════════════════════════════════════════════════
#define TFT_BL 19
// AS-BUILT wiring (verified with pairwise diagnostic 2026-07-20):
// 3 physical rows are the DRIVEN lines (diode cathodes face them),
// 4 physical columns are the READ lines. Key idx = row*4 + col.
const uint8_t DRIVE_PINS[3] = {25, 32, 33};     // rows top→bottom, driven LOW
const uint8_t READ_PINS[4]  = {26, 14, 27, 13}; // cols left→right, INPUT_PULLUP

#define NUM_KEYS   12
#define KEY_FN      8          // bottom-left key (K9)

// ════════════════════════════════════════════════
//  KEY MODEL  (defined up here so Arduino's auto-generated prototypes,
//  inserted before the first function, can see these types)
// ════════════════════════════════════════════════
// Rich key model. `type` defaults to KA_BUILTIN (0) so the existing
// aggregate initializers {"Label", A_ID} still compile and behave exactly
// as before — everything past `id` zero-initializes.
enum KAType : uint8_t {
  KA_BUILTIN = 0,   // fire ACTION_LIB entry `id` (OS-layout aware)
  KA_KEY,           // single chord: mod + key
  KA_CONSUMER,      // single media/consumer usage code
  KA_MACRO,         // ordered sequence of chord steps with delays
  KA_TEXT,          // type an ASCII string
  KA_HOST,          // notify the companion app over GATT (mod/key = HID
                    // fallback chord for when no app is listening)
};

#define MACRO_MAX 6
struct MacroStep {
  uint8_t  mod;       // modifier bitmap (0 if this step is a consumer)
  uint8_t  key;       // HID keycode  (0 => use `consumer`)
  uint16_t consumer;  // consumer usage (used when key==0)
  uint8_t  delayCs;   // post-step delay in centiseconds (×10ms)
};

struct KeyAction {
  char      label[9];
  int16_t   id;                 // KA_BUILTIN action id
  uint8_t   type;               // KAType
  uint8_t   mod;                // KA_KEY
  uint8_t   key;                // KA_KEY
  uint16_t  consumer;           // KA_CONSUMER
  uint8_t   nSteps;             // KA_MACRO step count
  MacroStep steps[MACRO_MAX];   // KA_MACRO
  char      text[24];           // KA_TEXT
};
struct Preset    { char name[10]; KeyAction keys[NUM_KEYS]; };

// ════════════════════════════════════════════════
//  TUNING
// ════════════════════════════════════════════════
int           backlightBrightness = 180;
unsigned long sleepTimeoutMs      = 300000UL;   // 0 = no sleep
bool          linuxLayout         = false;      // false=Windows, true=Linux

const uint16_t KEY_DEBOUNCE_MS   = 25;   // raised: bench keys were sticking
const uint16_t KEY_SCAN_INTERVAL_MS = 5; // fixed scan cadence, not every loop

// ── FACE / screensaver ──────────────────────────
// faceMode:  0=OFF  1=IDLE (screensaver after idle)  2=ALWAYS (default view)
// faceStyle: 0=procedural robot eyes  1=uploaded GIF loop (falls back to eyes)
uint8_t faceMode  = 1;
uint8_t faceStyle = 0;
char    faceGif[32] = "";                 // selected SPIFFS path, e.g. "/idle.gif"
// IDLE threshold comes from faceCfg.idleS (configurable, default 12s).
const unsigned long FACE_ALWAYS_MS = 2000;  // ALWAYS: return to face after this

// Expression pack — every visual parameter of the eyes, uploadable as JSON
// via the config API so the companion app can "generate" personalities.
struct FaceCfg {
  uint16_t color;        // RGB565 eye color; 0 = follow active preset color
  uint8_t  eyeW, eyeH;   // eye size at rest (px)
  uint8_t  gap;          // horizontal gap between the eyes (px)
  uint8_t  rnd;          // corner radius (px)
  uint8_t  blinkMinS, blinkMaxS;    // idle blink interval range (seconds)
  uint8_t  glanceMinS, glanceMaxS;  // idle glance interval range (seconds)
  uint8_t  pairScalePct; // pairing-mode wide-eye width scale (percent)
  uint8_t  idleS;        // IDLE mode: seconds of no input before face shows
};
// color 0x3DFF = robotic blue (#3ABEFF) — the default face. Set to 0 to
// follow the active preset's accent instead (app: "match preset").
FaceCfg faceCfg = { 0x3DFF, 64, 84, 44, 18, 3, 6, 7, 15, 115, 12 };

// ── Personality ─────────────────────────────────
// One renderable posture. Everything the face can express reduces to these
// eight numbers, so emote keyframes, persona resting poses and the mood
// engine all speak the same language and can be blended by the frame loop.
struct EyePose {
  uint8_t openPct;      // 0-100 lid opening (scales eyeH)
  uint8_t lidTopPct;    // 0-100 top lid coverage — sleepy/heavy when high
  int8_t  lidTopAngle;  // -100..100: + outer-deep (sad), - inner-deep (angry)
  uint8_t lidBotPct;    // 0-100 bottom crescent — the happy squint
  uint8_t wPct, hPct;   // eye size scale, 100 = rest (surprised goes ~115)
  int8_t  gx, gy;       // glance bias (px)
};
const EyePose POSE_NEUTRAL = { 100, 0, 0, 0, 100, 100, 0, 0 };

// A keyframe holds a pose from tMs until the next frame's tMs.
// winkMask: bit0 = left eye closed, bit1 = right eye closed.
struct EmoteKey { uint16_t tMs; EyePose pose; uint8_t winkMask; };
struct Emote    { const EmoteKey* keys; uint8_t n; uint16_t durMs; };

// Persona = a tuning table. Percentages scale the faceCfg intervals and the
// emote amplitudes, so one enum changes how the whole face carries itself.
struct Personality {
  char    name[8];
  uint8_t blinkPct, glancePct;              // 100 = faceCfg timings as-is
  uint8_t emotePct;                         // emote amplitude scale
  uint8_t saccadePct, dblBlinkPct, yawnPct; // micro-behaviour likelihoods
  uint8_t squintPct;                        // idle thoughtful-squint likelihood
  int8_t  energyBias, valenceBias;          // resting mood offsets (-100..100)
  EyePose rest;                             // posture with nothing happening
};
const Personality PERSONAS[] = {
  // name      blink glance emote sacc dbl yawn sqnt  eBias vBias  rest pose
  { "CALM",     120,  110,   70,   30,  10,  15,  20,   -10,   10, { 100,  8,   0,  0, 100, 100, 0, 0 } },
  { "PLAYFUL",   70,   60,  130,  100,  40,  10,  25,    25,   25, { 100,  0,   0,  8, 100, 100, 0, 0 } },
  { "GRUMPY",   140,  130,   60,   20,   5,   5,  50,   -15,  -30, {  92, 18, -35,  0, 100, 100, 0, 0 } },
  { "SLEEPY",   170,  150,   50,   15,  10,  60,  30,   -40,    0, {  80, 35,  20,  0, 100,  96, 0, 2 } },
};
const uint8_t NUM_PERSONAS = sizeof(PERSONAS) / sizeof(PERSONAS[0]);
uint8_t facePersona = 0;

const uint16_t KEY_MIN_PRESS_MS  = 30;
const unsigned long FN_MENU_MS   = 1000;   // FN hold → SYSTEM menu
const unsigned long PAIR_HOLD_MS = 1500;   // slot key hold → pairing mode
const unsigned long CLRALL_HOLD_MS = 2000; // devices screen K11 hold → wipe bonds
const unsigned long FLASH_MS     = 250;
#define WAKEKEY_NONE  -1
#define RECONNECT_TIMEOUT_MS  8000UL

// ════════════════════════════════════════════════
//  HID KEYCODES
// ════════════════════════════════════════════════
#define MOD_LCTRL   0x01
#define MOD_LSHIFT  0x02
#define MOD_LALT    0x04
#define MOD_LGUI    0x08

// Key usage codes (HID page 0x07)
#define KEY_A         0x04
#define KEY_B         0x05
#define KEY_C         0x06
#define KEY_D         0x07
#define KEY_E         0x08
#define KEY_F         0x09
#define KEY_G         0x0A
#define KEY_H         0x0B
#define KEY_I         0x0C
#define KEY_K         0x0E
#define KEY_L         0x0F
#define KEY_M         0x10
#define KEY_N         0x11
#define KEY_O         0x12
#define KEY_P         0x13
#define KEY_R         0x15
#define KEY_S         0x16
#define KEY_T         0x17
#define KEY_U         0x18
#define KEY_V         0x19
#define KEY_W         0x1A
#define KEY_X         0x1B
#define KEY_Y         0x1C
#define KEY_Z         0x1D
#define KEY_1         0x1E
#define KEY_2         0x1F
#define KEY_3         0x20
#define KEY_5         0x22
#define KEY_7         0x24
#define KEY_0         0x27
#define KEY_ENTER     0x28
#define KEY_TAB       0x2B
#define KEY_MINUS     0x2D
#define KEY_EQUAL     0x2E
#define KEY_BACKTICK  0x35
#define KEY_F4        0x3D
#define KEY_F11       0x44
#define KEY_PRTSC     0x46
#define KEY_RIGHT_ARR 0x4F
#define KEY_LEFT_ARR  0x50
#define KEY_DOWN_ARR  0x51
#define KEY_UP_ARR    0x52

// Consumer page usage IDs (16-bit)
#define CONSUMER_PLAY_PAUSE  0x00CD
#define CONSUMER_NEXT        0x00B5
#define CONSUMER_PREV        0x00B6
#define CONSUMER_MUTE        0x00E2
#define CONSUMER_VOL_UP      0x00E9
#define CONSUMER_VOL_DOWN    0x00EA

// ════════════════════════════════════════════════
//  HID REPORT DESCRIPTOR
// ════════════════════════════════════════════════
static const uint8_t hidReportMap[] = {
  // Keyboard — Report ID 1
  0x05,0x01, 0x09,0x06, 0xA1,0x01,
  0x85,0x01,
  0x05,0x07, 0x19,0xE0, 0x29,0xE7,
  0x15,0x00, 0x25,0x01, 0x75,0x01, 0x95,0x08,
  0x81,0x02,                    // modifier byte
  0x95,0x01, 0x75,0x08, 0x81,0x01, // reserved
  0x95,0x06, 0x75,0x08,
  0x15,0x00, 0x25,0x65,
  0x05,0x07, 0x19,0x00, 0x29,0x65,
  0x81,0x00,                    // 6 keycodes
  0xC0,
  // Consumer — Report ID 2
  0x05,0x0C, 0x09,0x01, 0xA1,0x01,
  0x85,0x02,
  0x15,0x00, 0x26,0xFF,0x03,
  0x19,0x00, 0x2A,0xFF,0x03,
  0x75,0x10, 0x95,0x01,
  0x81,0x00,
  0xC0,
};

#define DEVICE_NAME "ESP32 MacroPad"

// ════════════════════════════════════════════════
//  MULTI-HOST SLOTS  (the Easy-Switch feature)
// ════════════════════════════════════════════════
#define NUM_SLOTS 3

struct HostSlot {           // POD — stored raw as one NVS blob
  uint8_t addr[6];          // peer identity address (NimBLE native order)
  uint8_t type;             // address type
  uint8_t bonded;           // 0/1
};
HostSlot hostSlots[NUM_SLOTS] = {};
int  activeSlot = 0;
volatile bool pairingMode = false;

// BLE→loop event mailbox — NimBLE callbacks must never draw on the TFT,
// the loop task owns the display
enum BleEvent : uint8_t { EVT_NONE=0, EVT_PAIRED, EVT_WRONG_HOST };
volatile uint8_t pendingBleEvent = EVT_NONE;

// ════════════════════════════════════════════════
//  NIMBLE BLE GLOBALS
// ════════════════════════════════════════════════
NimBLEServer*         pServer   = nullptr;
NimBLEHIDDevice*      pHID      = nullptr;
NimBLECharacteristic* pKbReport = nullptr;  // Report ID 1
NimBLECharacteristic* pCcReport = nullptr;  // Report ID 2
volatile bool bleConnected = false;
volatile uint16_t bleConnHandle = 0;

// ════════════════════════════════════════════════
//  HOST-LINK — custom GATT service for the companion app
//  Lives alongside HID on the same bond. The app subscribes to EVT and
//  receives key/preset events; it writes TLV commands to CMD. Wire format:
//  [opcode:1][len:1][payload]. Spec in docs/CONFIG_API.md.
// ════════════════════════════════════════════════
#define HOSTLINK_SVC_UUID "6d616372-6f70-6164-0000-000000000001"  // "macropad"
#define HOSTLINK_EVT_UUID "6d616372-6f70-6164-0000-000000000002"
#define HOSTLINK_CMD_UUID "6d616372-6f70-6164-0000-000000000003"

enum : uint8_t {  // device → host
  HEV_HELLO  = 0x01,   // [fwMajor][keys][presets][activePreset][faceMode][persona]
  HEV_KEY    = 0x02,   // [preset][keyIdx] — a KA_HOST key was tapped
  HEV_PRESET = 0x03,   // [preset] — active preset changed (either side)
};
enum : uint8_t {  // host → device
  HCMD_LABEL  = 0x81,  // [preset][key][utf8 ≤8] — live label override
  HCMD_STATUS = 0x82,  // [utf8 ≤23] — status-bar line; empty clears
  HCMD_PRESET = 0x83,  // [preset] — foreground-follow switches the pad
  HCMD_FACE   = 0x84,  // [mode 0-2][persona 0-3]
  HCMD_COLOR  = 0x85,  // [preset][rgb565 hi][rgb565 lo] — preset accent + eye color
  HCMD_KEY    = 0x86,  // [preset][key][kaType][mod][hid][cons lo][cons hi][label…]
  HCMD_COMMIT = 0x87,  // persist presets to NVS (send once after a setKey burst)
  HCMD_TEXT   = 0x88,  // [preset][key][utf8 ≤23] — text payload for a KA_TEXT key
  HCMD_EYES   = 0x89,  // [rgb565 hi][rgb565 lo][persist] — eye color, 0 = follow preset
  HCMD_MEDIA  = 0x8A,  // [flags][pos lo][hi][dur lo][hi][title ≤20]
                       // flags: bit0 = playing, bit1 = favorited
};

NimBLECharacteristic* pEvtChar = nullptr;
volatile bool hostAppSubscribed = false;
volatile bool hostHelloPending  = false;

// Command mailbox — onWrite runs in the NimBLE task and must never touch the
// display (same rule as pendingBleEvent). Sized for a full 12-label burst.
#define HOSTCMD_QMAX   16
#define HOSTCMD_MAXLEN 28
struct HostCmd { uint8_t len; uint8_t data[HOSTCMD_MAXLEN]; };
HostCmd hostCmdQ[HOSTCMD_QMAX];
volatile uint8_t hostCmdHead = 0, hostCmdTail = 0;   // head=write, tail=read

char hostStatus[24] = "";   // companion status line shown in the main bar

// Now-playing pushed by the companion (Windows media sessions — covers
// Feishin, Spotify, browsers). Position is extrapolated locally between
// pushes so the timeline moves without constant BLE traffic.
char     mediaTitle[24] = "";
uint16_t mediaPosS = 0, mediaDurS = 0;
bool     mediaPlaying = false;
bool     mediaFav = false;          // heart shown beside the title
unsigned long mediaRxMs = 0;
uint8_t  mediaShow = 1;      // Settings → MEDIA, NVS "media"
// Set by the command handler, consumed by the face so the eyes can react to
// music without the BLE task ever touching the display.
volatile bool mediaNewSong = false, mediaJustFaved = false;

class EvtCB : public NimBLECharacteristicCallbacks {
  void onSubscribe(NimBLECharacteristic* c, NimBLEConnInfo& info,
                   uint16_t subValue) override {
    hostAppSubscribed = (subValue & 0x0001);
    if (hostAppSubscribed) hostHelloPending = true;   // loop sends the hello
    Serial.printf("[HOST] subscribe=%u\n", subValue);
  }
};

class CmdCB : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* c, NimBLEConnInfo& info) override {
    NimBLEAttValue v = c->getValue();
    if (v.size() < 2) return;
    uint8_t next = (uint8_t)((hostCmdHead + 1) % HOSTCMD_QMAX);
    if (next == hostCmdTail) return;                  // full — drop, don't block
    HostCmd& q = hostCmdQ[hostCmdHead];
    q.len = (uint8_t)min((unsigned)HOSTCMD_MAXLEN, (unsigned)v.size());
    memcpy(q.data, v.data(), q.len);
    hostCmdHead = next;
  }
};

void startAdvertisingForSlot();   // fwd
void saveSlots();                 // fwd
void faceSlotGlance(int slot);    // fwd
void enterConfigMode();           // fwd
void clampFaceCfg();              // fwd

int slotForAddress(const uint8_t addr[6]) {
  for (int i = 0; i < NUM_SLOTS; i++)
    if (hostSlots[i].bonded && memcmp(hostSlots[i].addr, addr, 6) == 0) return i;
  return -1;
}

class ServerCB : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer* s, NimBLEConnInfo& info) override {
    Serial.printf("[BLE] connect h=%u peer=%s\n",
                  info.getConnHandle(), info.getAddress().toString().c_str());
    bleConnected  = true;
    bleConnHandle = info.getConnHandle();
    // Request faster connection interval — critical for Android/Linux responsiveness
    s->updateConnParams(info.getConnHandle(), 6, 12, 0, 400);
    NimBLEDevice::stopAdvertising();
  }
  void onAuthenticationComplete(NimBLEConnInfo& info) override {
    Serial.printf("[BLE] auth enc=%d bonded=%d id=%s pairing=%d slot=%d\n",
                  info.isEncrypted(), info.isBonded(),
                  info.getIdAddress().toString().c_str(),
                  (int)pairingMode, activeSlot);
    // Require full bonding, not just encryption — a host that encrypts but
    // fails to store keys (half-completed pairing) must not claim the slot,
    // otherwise the whitelist deadlocks on a peer that can never reconnect
    if (!info.isEncrypted() || !info.isBonded()) {
      pServer->disconnect(info.getConnHandle());
      return;
    }
    NimBLEAddress id = info.getIdAddress();
    const uint8_t* idBytes = id.getBase()->val;
    int knownSlot = slotForAddress(idBytes);

    if (pairingMode) {
      if (knownSlot >= 0 && knownSlot != activeSlot) {
        // A host already bonded to a DIFFERENT slot grabbed the open
        // advertising — kick it, keep waiting for a genuinely new device
        pendingBleEvent = EVT_WRONG_HOST;
        pServer->disconnect(info.getConnHandle());
        return;
      }
      memcpy(hostSlots[activeSlot].addr, idBytes, 6);
      hostSlots[activeSlot].type   = id.getBase()->type;
      hostSlots[activeSlot].bonded = 1;
      pairingMode = false;
      saveSlots();
      pendingBleEvent = EVT_PAIRED;
    } else {
      // Enforce Easy-Switch in software: only kick a host we POSITIVELY know
      // belongs to a different slot. A host that resolves to knownSlot == -1
      // (address didn't map cleanly) is kept — better than wrongly rejecting
      // the legitimate active host and breaking reconnection.
      if (knownSlot >= 0 && knownSlot != activeSlot) {
        pendingBleEvent = EVT_WRONG_HOST;
        pServer->disconnect(info.getConnHandle());
      }
    }
  }
  void onDisconnect(NimBLEServer* s, NimBLEConnInfo& info, int reason) override {
    Serial.printf("[BLE] disconnect reason=%d\n", reason);
    bleConnected = false;
    hostAppSubscribed = false;   // CCCD subscriptions die with the link
    hostHelloPending  = false;
    delay(200);
    // Rebuild adv data on every restart — BlueZ requires the flags byte to
    // be present each time, not just on first boot
    startAdvertisingForSlot();
  }
};

// Rebuilds advertisement payload from scratch (BlueZ flags requirement)
void configureAdvertising() {
  NimBLEAdvertising* pAdv = NimBLEDevice::getAdvertising();
  NimBLEAdvertisementData advData;
  // Flags: 0x06 = LE General Discoverable (0x02) | BR/EDR Not Supported (0x04)
  // BlueZ ignores devices that don't have this exact flags byte
  advData.setFlags(0x06);
  advData.setAppearance(0x03C1);           // HID keyboard
  advData.addServiceUUID(pHID->getHidService()->getUUID());
  pAdv->setAdvertisementData(advData);
  // Scan response carries the full device name
  NimBLEAdvertisementData scanData;
  scanData.setName(DEVICE_NAME);
  pAdv->setScanResponseData(scanData);
  pAdv->enableScanResponse(true);
  // 20–30ms interval — short enough for Linux's default scan window
  pAdv->setMinInterval(32);
  pAdv->setMaxInterval(48);
}

// Advertise for the ACTIVE slot.
//
// We advertise OPEN (no controller accept-list / scan filter) even for a
// bonded slot. Whitelist-filtered advertising silently rejects a bonded host
// that reconnects from a Resolvable Private Address (phones + Windows use RPAs
// by default) unless the controller resolving list is perfectly populated —
// in practice this blocks reconnection after a reboot. Instead any bonded host
// may reconnect, and onAuthenticationComplete disconnects one that doesn't
// belong to the active slot. Same Easy-Switch guarantee, reliable reconnect.
void startAdvertisingForSlot() {
  NimBLEAdvertising* pAdv = NimBLEDevice::getAdvertising();
  if (pAdv->isAdvertising()) pAdv->stop();

  while (NimBLEDevice::getWhiteListCount() > 0)
    NimBLEDevice::whiteListRemove(NimBLEDevice::getWhiteListAddress(0));

  pAdv->setScanFilter(false, false);   // open — bonded hosts resolve via bond DB
  configureAdvertising();
  pAdv->start();
  Serial.printf("[BLE] advertising slot=%d bonded=%d pairing=%d\n",
                activeSlot, (int)hostSlots[activeSlot].bonded, (int)pairingMode);
}

// Easy-Switch: jump to slot n. forcePair drops the slot's old bond and
// opens pairing (Pebble Keys long-press behaviour).
void switchToSlot(int n, bool forcePair) {
  if (n < 0 || n >= NUM_SLOTS) return;
  if (forcePair && hostSlots[n].bonded) {
    NimBLEDevice::deleteBond(NimBLEAddress(hostSlots[n].addr, hostSlots[n].type));
    hostSlots[n].bonded = 0;
  }
  activeSlot  = n;
  pairingMode = forcePair || !hostSlots[n].bonded;
  faceSlotGlance(n);          // face glances toward the new slot if visible soon
  saveSlots();
  if (bleConnected) {
    pServer->disconnect(bleConnHandle);  // onDisconnect → startAdvertisingForSlot
  } else {
    startAdvertisingForSlot();
  }
}

void clearSlotBond(int n) {
  if (n < 0 || n >= NUM_SLOTS || !hostSlots[n].bonded) return;
  NimBLEDevice::deleteBond(NimBLEAddress(hostSlots[n].addr, hostSlots[n].type));
  hostSlots[n].bonded = 0;
  saveSlots();
  if (n == activeSlot) {
    pairingMode = true;
    if (bleConnected) pServer->disconnect(bleConnHandle);
    else startAdvertisingForSlot();
  }
}

void clearAllBonds() {
  NimBLEDevice::deleteAllBonds();
  for (int i = 0; i < NUM_SLOTS; i++) hostSlots[i].bonded = 0;
  pairingMode = true;
  saveSlots();
  if (bleConnected) pServer->disconnect(bleConnHandle);
  else startAdvertisingForSlot();
}

// ════════════════════════════════════════════════
//  KEY SEND HELPERS
// ════════════════════════════════════════════════
void sendKey(uint8_t mod, uint8_t key) {
  if (!bleConnected || !pKbReport) return;
  uint8_t report[8] = {mod, 0, key, 0, 0, 0, 0, 0};
  pKbReport->setValue(report, 8);
  pKbReport->notify();
  delay(20);
  uint8_t rel[8] = {};
  pKbReport->setValue(rel, 8);
  pKbReport->notify();
  delay(10);
}

void sendConsumer(uint16_t usage) {
  if (!bleConnected || !pCcReport) return;
  uint8_t report[2] = {(uint8_t)(usage & 0xFF), (uint8_t)(usage >> 8)};
  pCcReport->setValue(report, 2);
  pCcReport->notify();
  delay(20);
  uint8_t rel[2] = {};
  pCcReport->setValue(rel, 2);
  pCcReport->notify();
  delay(10);
}

void releaseAll() {
  if (!pKbReport) return;
  uint8_t rel[8] = {};
  pKbReport->setValue(rel, 8);
  pKbReport->notify();
  uint8_t rel2[2] = {};
  if (pCcReport) { pCcReport->setValue(rel2, 2); pCcReport->notify(); }
}

// ════════════════════════════════════════════════
//  ACTION IDS
// ════════════════════════════════════════════════
#define A_NONE         0
#define A_PLAY       101
#define A_NEXT       102
#define A_PREV       103
#define A_MUTE       104
#define A_SPOTIFY    105
#define A_VOLUP      106
#define A_VOLDN      107
#define A_LOCK       110
#define A_SNIP       111
#define A_SS_FULL    112
#define A_TASK       113
#define A_MINALL     114
#define A_DESK_R     115
#define A_DESK_L     116
#define A_CALC       117
#define A_EXPLORER   118
#define A_SETTINGS_W 119
#define A_UNDO       120
#define A_REDO       121
#define A_SAVE       122
#define A_COPY       123
#define A_PASTE      124
#define A_CUT        125
#define A_SELALL     126
#define A_CLOSE      127
#define A_FIND       128
#define A_REPLACE    129
#define A_NEWFILE    130
#define A_OPENFILE   131
#define A_OS_FIT     140
#define A_OS_FRONT   141
#define A_OS_TOP     142
#define A_OS_RIGHT   143
#define A_OS_ISO     144
#define A_OS_ZOOM_FIT 145
#define A_OS_EXTRUDE 146
#define A_OS_SKETCH  147
#define A_OS_MATE    148
#define A_OS_ASSEMBLY 149
#define A_KC_ROUTE   150
#define A_KC_ADD_NET 151
#define A_KC_ZOOM_FIT 152
#define A_KC_DRC     153
#define A_KC_3D      154
#define A_KC_COPPER  155
#define A_KC_GERBER  156
#define A_KC_RATSNEST 157
#define A_PUSH_TO_TALK 160
#define A_RELOAD     161
#define A_MAP        162
#define A_SCORE      163
#define A_FULLSCREEN 164
#define A_OBS_REC    165
#define A_OBS_STREAM 166
#define A_DISCORD    167
#define A_NEW_TAB    170
#define A_CLOSE_TAB  171
#define A_RETAB      172
#define A_BACK       173
#define A_FORWARD    174
#define A_REFRESH    175
#define A_ADDR_BAR   176
#define A_ALTTAB     177
#define A_LTS_MOVE   190
#define A_LTS_GND    191
#define A_LTS_VCC    192
#define A_LTS_RES    193
#define A_LTS_CAP    194
#define A_LTS_COMP   195
#define A_LTS_WIRE   196
#define A_LTS_RUN    197

// ── Linux-specific action IDs (200+) — GNOME/Tilix shortcuts ──
#define A_LX_TERMINAL    212   // Ctrl+Alt+T
#define A_LX_ZOOM_IN     213   // Ctrl+=
#define A_LX_ZOOM_OUT    214   // Ctrl+-
#define A_LX_ZOOM_RST    215   // Ctrl+0
#define A_LX_NOTIF       216   // Super+V
#define A_LX_SPLIT_H     217   // Ctrl+Shift+E (Tilix)
#define A_LX_SPLIT_V     218   // Ctrl+Shift+O (Tilix)
#define A_LX_NEW_TERM    219   // Ctrl+Shift+T
#define A_LX_MAXIMIZE    220   // Super+Up
#define A_LX_HALF_L      221   // Super+Left
#define A_LX_HALF_R      222   // Super+Right
#define A_LX_MOVE_WS1    223   // Shift+Super+1
#define A_LX_MOVE_WS2    224   // Shift+Super+2

struct Action { const char* label; int id; };
const Action ACTION_LIB[] = {
  {"---",A_NONE},
  {"Play/Pse",A_PLAY},{"Next",A_NEXT},{"Prev",A_PREV},{"Mute",A_MUTE},
  {"Spotify",A_SPOTIFY},{"Vol Up",A_VOLUP},{"Vol Down",A_VOLDN},
  {"Lock PC",A_LOCK},{"Snip",A_SNIP},{"Full SS",A_SS_FULL},
  {"TaskView",A_TASK},{"Min All",A_MINALL},{"Desk R",A_DESK_R},
  {"Desk L",A_DESK_L},{"Calc",A_CALC},{"Explorer",A_EXPLORER},
  {"Win Sett",A_SETTINGS_W},{"Alt-Tab",A_ALTTAB},
  {"Undo",A_UNDO},{"Redo",A_REDO},{"Save",A_SAVE},{"Copy",A_COPY},
  {"Paste",A_PASTE},{"Cut",A_CUT},{"Sel All",A_SELALL},
  {"CloseWin",A_CLOSE},{"Find",A_FIND},{"Replace",A_REPLACE},
  {"New File",A_NEWFILE},{"OpenFile",A_OPENFILE},
  {"OS: Fit",A_OS_FIT},{"OS:Front",A_OS_FRONT},{"OS: Top",A_OS_TOP},
  {"OS:Right",A_OS_RIGHT},{"OS: Iso",A_OS_ISO},{"OS: ZFit",A_OS_ZOOM_FIT},
  {"OS:Extrd",A_OS_EXTRUDE},{"OS:Sktch",A_OS_SKETCH},
  {"OS: Mate",A_OS_MATE},{"OS:Assem",A_OS_ASSEMBLY},
  {"KC:Route",A_KC_ROUTE},{"KC:AddNt",A_KC_ADD_NET},
  {"KC: ZFit",A_KC_ZOOM_FIT},{"KC: DRC",A_KC_DRC},{"KC: 3D",A_KC_3D},
  {"KC:Coppr",A_KC_COPPER},{"KC:Gerbr",A_KC_GERBER},
  {"KC: Rats",A_KC_RATSNEST},
  {"PTT",A_PUSH_TO_TALK},{"Reload",A_RELOAD},{"Map",A_MAP},
  {"Score",A_SCORE},{"FullScrn",A_FULLSCREEN},
  {"OBS Rec",A_OBS_REC},{"OBS Strm",A_OBS_STREAM},{"Discord",A_DISCORD},
  {"New Tab",A_NEW_TAB},{"CloseTab",A_CLOSE_TAB},{"ReopnTab",A_RETAB},
  {"Back",A_BACK},{"Forward",A_FORWARD},{"Refresh",A_REFRESH},
  {"Addr Bar",A_ADDR_BAR},
  {"LTS Move",A_LTS_MOVE},{"LTS GND",A_LTS_GND},{"LTS VCC",A_LTS_VCC},
  {"LTS Res",A_LTS_RES},{"LTS Cap",A_LTS_CAP},{"LTS Comp",A_LTS_COMP},
  {"LTS Wire",A_LTS_WIRE},{"LTS Run",A_LTS_RUN},
  // Linux / GNOME shortcuts
  {"LX:Term",A_LX_TERMINAL},{"LX:ZmIn",A_LX_ZOOM_IN},{"LX:ZmOut",A_LX_ZOOM_OUT},
  {"LX:ZmRst",A_LX_ZOOM_RST},{"LX:Notif",A_LX_NOTIF},{"LX:SpltH",A_LX_SPLIT_H},
  {"LX:SpltV",A_LX_SPLIT_V},{"LX:NwTrm",A_LX_NEW_TERM},{"LX:MaxWn",A_LX_MAXIMIZE},
  {"LX:HalfL",A_LX_HALF_L},{"LX:HalfR",A_LX_HALF_R},
  {"LX:MvWS1",A_LX_MOVE_WS1},{"LX:MvWS2",A_LX_MOVE_WS2},
};
const int ACTION_LIB_SIZE = sizeof(ACTION_LIB)/sizeof(Action);

// ════════════════════════════════════════════════
//  PRESETS — 12 keys each (4 wide × 3 tall)
// ════════════════════════════════════════════════
#define NUM_PRESETS  8
#define PRESETS_VER  2        // bump to invalidate stored NVS presets
// KeyAction / MacroStep / Preset are defined near the top of the file.

// A key is "empty" only if it's an unassigned built-in slot
static inline bool kaEmpty(const KeyAction& ka) {
  return ka.type == KA_BUILTIN && ka.id == A_NONE;
}

Preset presets[NUM_PRESETS] = {
  {"ONSHAPE",{
    {"Fit",A_OS_FIT},{"Front",A_OS_FRONT},{"Top",A_OS_TOP},{"Right",A_OS_RIGHT},
    {"Iso",A_OS_ISO},{"ZFit",A_OS_ZOOM_FIT},{"Extrude",A_OS_EXTRUDE},{"Sketch",A_OS_SKETCH},
    {"Mate",A_OS_MATE},{"Assem",A_OS_ASSEMBLY},{"Undo",A_UNDO},{"Save",A_SAVE}
  }},
  {"KICAD",{
    {"Route",A_KC_ROUTE},{"ZFit",A_KC_ZOOM_FIT},{"DRC",A_KC_DRC},{"3D",A_KC_3D},
    {"Copper",A_KC_COPPER},{"Gerber",A_KC_GERBER},{"Rats",A_KC_RATSNEST},{"AddNet",A_KC_ADD_NET},
    {"Undo",A_UNDO},{"Save",A_SAVE},{"Copy",A_COPY},{"Paste",A_PASTE}
  }},
  {"MUSIC",{
    {"Play",A_PLAY},{"Next",A_NEXT},{"Prev",A_PREV},{"Mute",A_MUTE},
    {"Vol Up",A_VOLUP},{"Vol Dn",A_VOLDN},{"Spotify",A_SPOTIFY},{"New Tab",A_NEW_TAB},
    {"CloseTab",A_CLOSE_TAB},{"Back",A_BACK},{"Forward",A_FORWARD},{"Refresh",A_REFRESH}
  }},
  {"LTSPICE",{
    {"Move",A_LTS_MOVE},{"GND",A_LTS_GND},{"VCC",A_LTS_VCC},{"Res",A_LTS_RES},
    {"Cap",A_LTS_CAP},{"AddComp",A_LTS_COMP},{"Wire",A_LTS_WIRE},{"Run",A_LTS_RUN},
    {"Undo",A_UNDO},{"Save",A_SAVE},{"Copy",A_COPY},{"Paste",A_PASTE}
  }},
  {"GAMING",{
    {"PTT",A_PUSH_TO_TALK},{"Reload",A_RELOAD},{"Map",A_MAP},{"Score",A_SCORE},
    {"FullScr",A_FULLSCREEN},{"OBS Rec",A_OBS_REC},{"OBS Str",A_OBS_STREAM},{"Discord",A_DISCORD},
    {"Mute",A_MUTE},{"Snip",A_SNIP},{"Full SS",A_SS_FULL},{"Task",A_TASK}
  }},
  {"SYS",{
    {"Term",A_LX_TERMINAL},{"Lock",A_LOCK},{"Snip",A_SNIP},{"Files",A_EXPLORER},
    {"MaxWin",A_LX_MAXIMIZE},{"HalfL",A_LX_HALF_L},{"HalfR",A_LX_HALF_R},{"Show Dk",A_MINALL},
    {"Task",A_TASK},{"Desk L",A_DESK_L},{"Desk R",A_DESK_R},{"Calc",A_CALC}
  }},
  {"DEV",{
    {"Term",A_LX_TERMINAL},{"NewTab",A_LX_NEW_TERM},{"SplitH",A_LX_SPLIT_H},{"SplitV",A_LX_SPLIT_V},
    {"Desk R",A_DESK_R},{"Desk L",A_DESK_L},{"Zoom In",A_LX_ZOOM_IN},{"ZoomOut",A_LX_ZOOM_OUT},
    {"ZmRst",A_LX_ZOOM_RST},{"Undo",A_UNDO},{"Save",A_SAVE},{"Find",A_FIND}
  }},
  {"EDIT",{
    {"Undo",A_UNDO},{"Redo",A_REDO},{"Save",A_SAVE},{"Copy",A_COPY},
    {"Paste",A_PASTE},{"Cut",A_CUT},{"Find",A_FIND},{"Rplace",A_REPLACE},
    {"SelAll",A_SELALL},{"NewFile",A_NEWFILE},{"OpenFile",A_OPENFILE},{"Alt-Tab",A_ALTTAB}
  }},
};

// ════════════════════════════════════════════════
//  SCREEN / STATE
// ════════════════════════════════════════════════
enum Screen {
  SCR_MAIN, SCR_SYSMENU, SCR_PRESET, SCR_SETTINGS,
  SCR_EDIT_BRIGHT, SCR_EDIT_SLEEP, SCR_DEVICES,
  SCR_BUILD_PRESET, SCR_BUILD_KEYS, SCR_BUILD_ACTION,
  SCR_FACE
};
Screen currentScreen = SCR_MAIN;

int  activePreset = 0;
int  lastFlashKey = -1;
unsigned long flashUntil = 0;

int  buildPreset  = 0;
int  buildSlot    = 0;
int  buildActPage = 0;

bool lastBleConn    = false;
bool lastPairingUi  = false;

char toastMsg[28] = "";
uint16_t toastColor = C_CYAN;
unsigned long toastUntil = 0;

static int  wakeKeyIdx     = WAKEKEY_NONE;
static bool wakeKeyPending = false;
static unsigned long wakeTimeMs = 0;

unsigned long lastActivityMs = 0;
static inline void recordActivity() { lastActivityMs = millis(); }

Preferences prefs;

// ════════════════════════════════════════════════
//  TFT — direct drawing + small sprites only
//  (full 320×240 sprite = 150KB, impossible with BLE active)
// ════════════════════════════════════════════════
TFT_eSPI    tft    = TFT_eSPI();
TFT_eSprite sprBar = TFT_eSprite(&tft);   // 320×26 status bar
TFT_eSprite sprCell= TFT_eSprite(&tft);   // 78×68 reusable key cell
TFT_eSprite sprEye = TFT_eSprite(&tft);   // 92×110 reusable eye canvas (face)
#define EYE_SPR_W 92
#define EYE_SPR_H 110

// Landscape cell grid: 4 cols × 3 rows below the 26px status bar
static inline int cellX(int i){ return 1 + (i % 4) * 80; }
static inline int cellY(int i){ return 28 + (i / 4) * 70; }
#define CELL_W 78
#define CELL_H 68

// Anti-stutter: only wipe the whole screen when the layout actually changes.
// A full tft.fillScreen() over SPI is what causes the black-flash + repaint
// stutter; on a same-screen refresh the cell/status sprites overwrite their
// own rectangles, so clearing is unnecessary. Overlays (toast, HUD) set the
// dirty flag so the next full draw clears their leftovers.
int  lastClearedScreen = -1;
bool screenDirty       = true;
static inline void beginDraw(int screenId) {
  if (screenId != lastClearedScreen || screenDirty) {
    tft.fillScreen(C_BG);
    lastClearedScreen = screenId;
    screenDirty = false;
  }
}

// ════════════════════════════════════════════════
//  FORWARD DECLARATIONS
// ════════════════════════════════════════════════
void redraw();
void drawMain(); void drawSysMenu(); void drawPresetPicker();
void drawSettings(); void drawEditor(); void drawDevices();
void drawBuildPreset(); void drawBuildKeys(); void drawBuildAction();
void drawReconnectHUD(const char* keyName);
void fireAction(int id);
void faceEnter(); void faceWake(); void faceSleepClose();
void updateFace(unsigned long now); void faceGifTick(unsigned long now);
void faceSlotGlance(int slot); void faceKeyReact(int i);

// ════════════════════════════════════════════════
//  PERSISTENCE (NVS)
// ════════════════════════════════════════════════
void loadState() {
  prefs.begin("mpv5", false);
  backlightBrightness = prefs.getInt("bright", 180);
  int sleepMin        = prefs.getInt("sleepMin", 5);
  sleepTimeoutMs      = (sleepMin == 0) ? 0UL : (unsigned long)sleepMin * 60000UL;
  linuxLayout         = prefs.getBool("linux", false);
  activePreset        = constrain(prefs.getInt("preset", 0), 0, NUM_PRESETS-1);
  activeSlot          = constrain(prefs.getInt("slot", 0), 0, NUM_SLOTS-1);
  if (prefs.getBytesLength("slots") == sizeof(hostSlots))
    prefs.getBytes("slots", hostSlots, sizeof(hostSlots));
  if (prefs.getInt("pver", 0) == PRESETS_VER &&
      prefs.getBytesLength("presets") == sizeof(presets))
    prefs.getBytes("presets", presets, sizeof(presets));
  faceMode  = min((uint8_t)2, (uint8_t)prefs.getUChar("face", 1));
  faceStyle = min((uint8_t)1, (uint8_t)prefs.getUChar("fstyle", 0));
  prefs.getString("fgif", faceGif, sizeof(faceGif));
  if (prefs.getBytesLength("fcfg") == sizeof(faceCfg))
    prefs.getBytes("fcfg", &faceCfg, sizeof(faceCfg));
  // Persona is its own key, so an old "fcfg" blob still loads unchanged
  facePersona = min((uint8_t)(NUM_PERSONAS - 1), (uint8_t)prefs.getUChar("fpers", 0));
  mediaShow   = prefs.getUChar("media", 1) ? 1 : 0;
  clampFaceCfg();
}

// Keep an uploaded expression pack inside sane/renderable bounds
void clampFaceCfg() {
  faceCfg.eyeW = constrain(faceCfg.eyeW, 20, 72);
  faceCfg.eyeH = constrain(faceCfg.eyeH, 28, 96);
  faceCfg.gap  = constrain(faceCfg.gap, 8, 120);
  faceCfg.rnd  = constrain(faceCfg.rnd, 2, 40);
  if (faceCfg.blinkMinS < 1) faceCfg.blinkMinS = 1;
  if (faceCfg.blinkMaxS < faceCfg.blinkMinS) faceCfg.blinkMaxS = faceCfg.blinkMinS;
  if (faceCfg.glanceMinS < 2) faceCfg.glanceMinS = 2;
  if (faceCfg.glanceMaxS < faceCfg.glanceMinS) faceCfg.glanceMaxS = faceCfg.glanceMinS;
  faceCfg.pairScalePct = constrain(faceCfg.pairScalePct, 100, 130);
  faceCfg.idleS = constrain(faceCfg.idleS, 3, 120);
}

void saveSettings() {
  prefs.putInt("bright", backlightBrightness);
  prefs.putInt("sleepMin", (sleepTimeoutMs == 0) ? 0 : (int)(sleepTimeoutMs / 60000UL));
  prefs.putBool("linux", linuxLayout);
  prefs.putInt("preset", activePreset);
  prefs.putUChar("face", faceMode);
  prefs.putUChar("fstyle", faceStyle);
  prefs.putUChar("fpers", facePersona);
  prefs.putUChar("media", mediaShow);
  prefs.putString("fgif", faceGif);
}

void saveFaceCfg() {
  clampFaceCfg();
  prefs.putBytes("fcfg", &faceCfg, sizeof(faceCfg));
}

void saveSlots() {
  prefs.putBytes("slots", hostSlots, sizeof(hostSlots));
  prefs.putInt("slot", activeSlot);
}

void savePresets() {
  prefs.putBytes("presets", presets, sizeof(presets));
  prefs.putInt("pver", PRESETS_VER);
}

// ════════════════════════════════════════════════
//  MATRIX SCAN
//  Row (drive) lines pulled LOW one at a time, inactive rows hi-Z;
//  column (read) lines with pullups. Diode cathodes face the rows.
//  Logical key idx = driveRow*4 + readCol → 4-wide × 3-tall UI grid.
// ════════════════════════════════════════════════
void matrixInit() {
  for (int d = 0; d < 3; d++) pinMode(DRIVE_PINS[d], INPUT);
  for (int j = 0; j < 4; j++) pinMode(READ_PINS[j], INPUT_PULLUP);
}

uint16_t scanMatrixRaw() {
  uint16_t bits = 0;
  for (int d = 0; d < 3; d++) {
    pinMode(DRIVE_PINS[d], OUTPUT);
    digitalWrite(DRIVE_PINS[d], LOW);
    delayMicroseconds(25);
    for (int j = 0; j < 4; j++)
      if (digitalRead(READ_PINS[j]) == LOW) bits |= 1u << (d * 4 + j);
    pinMode(DRIVE_PINS[d], INPUT);
  }
  return bits;
}

// Debounced per-key state
bool keyStable[NUM_KEYS]  = {};
bool keyRaw[NUM_KEYS]     = {};
unsigned long keyChangeMs[NUM_KEYS] = {};
unsigned long keyDownMs[NUM_KEYS]   = {};
bool keyHoldFired[NUM_KEYS] = {};
bool keyFiredOnDown[NUM_KEYS] = {};

// ════════════════════════════════════════════════
//  STATUS BAR — slot chips = Easy-Switch "LEDs"
// ════════════════════════════════════════════════
void drawStatusBar(const char* title, uint16_t accent) {
  sprBar.fillSprite(C_SURF);
  sprBar.setTextColor(accent, C_SURF);
  sprBar.setTextSize(2);
  sprBar.setCursor(6, 6);
  sprBar.print(title);

  // OS layout tag
  sprBar.setTextSize(1);
  sprBar.setTextColor(C_DIM, C_SURF);
  sprBar.setCursor(216, 10);
  sprBar.print(linuxLayout ? "LNX" : "WIN");

  // Slot chips 1 2 3
  for (int i = 0; i < NUM_SLOTS; i++) {
    int x = 244 + i * 26;
    uint16_t fill, border, txt;
    if (i == activeSlot) {
      if      (bleConnected) fill = C_GREEN;
      else if (pairingMode)  fill = C_MAGENTA;
      else                   fill = C_AMBER;
      border = fill; txt = 0x0000;
    } else {
      fill = C_SURF2;
      border = hostSlots[i].bonded ? C_DIM : C_BORDER;
      txt = hostSlots[i].bonded ? C_DIM : C_BORDER;
    }
    sprBar.fillRoundRect(x, 4, 22, 18, 4, fill);
    sprBar.drawRoundRect(x, 4, 22, 18, 4, border);
    sprBar.setTextColor(txt, fill);
    sprBar.setCursor(x + 9, 9);
    sprBar.print(i + 1);
  }
  sprBar.pushSprite(0, 0);
}

// ════════════════════════════════════════════════
//  KEY CELL — drawn via reusable sprite, flicker free
// ════════════════════════════════════════════════
void drawCell(int i, const char* line1, const char* line2,
              uint16_t accent, bool filled) {
  uint16_t bg = filled ? accent : C_SURF;
  sprCell.fillSprite(C_BG);
  sprCell.fillRoundRect(0, 0, CELL_W, CELL_H, 8, bg);
  sprCell.drawRoundRect(0, 0, CELL_W, CELL_H, 8, filled ? accent : C_BORDER);

  sprCell.setTextSize(1);
  sprCell.setTextColor(filled ? 0x0000 : C_DIM, bg);
  sprCell.setCursor(5, 4);
  sprCell.print(i + 1);
  if (i == KEY_FN) { sprCell.setCursor(60, 4); sprCell.print("FN"); }

  if (line1 && line1[0]) {
    int w = strlen(line1) * 6;
    sprCell.setTextColor(filled ? 0x0000 : C_WHITE, bg);
    sprCell.setCursor(max(3, (CELL_W - w) / 2), line2 && line2[0] ? 24 : 30);
    sprCell.print(line1);
  }
  if (line2 && line2[0]) {
    int w = strlen(line2) * 6;
    sprCell.setTextColor(filled ? 0x0000 : accent, bg);
    sprCell.setCursor(max(3, (CELL_W - w) / 2), 42);
    sprCell.print(line2);
  }
  sprCell.pushSprite(cellX(i), cellY(i));
}

void drawCellEmpty(int i) { drawCell(i, "", "", C_BORDER, false); }

// ════════════════════════════════════════════════
//  TOAST — transient centered message
// ════════════════════════════════════════════════
void showToast(const char* msg, uint16_t color, unsigned long ms) {
  strncpy(toastMsg, msg, sizeof(toastMsg) - 1);
  toastMsg[sizeof(toastMsg) - 1] = '\0';
  toastColor = color;
  toastUntil = millis() + ms;
  int w = strlen(toastMsg) * 12 + 28;
  int x = (320 - w) / 2;
  tft.fillRoundRect(x, 96, w, 48, 10, C_SURF2);
  tft.drawRoundRect(x, 96, w, 48, 10, color);
  tft.setTextSize(2);
  tft.setTextColor(color, C_SURF2);
  tft.setCursor(x + 14, 113);
  tft.print(toastMsg);
  screenDirty = true;   // next full redraw must clear this overlay
}

// ════════════════════════════════════════════════
//  SCREENS
// ════════════════════════════════════════════════
void drawMain() {
  beginDraw(SCR_MAIN);
  uint16_t accent = PRESET_COLORS[activePreset];
  drawStatusBar(hostStatus[0] ? hostStatus : presets[activePreset].name, accent);
  for (int i = 0; i < NUM_KEYS; i++) {
    KeyAction& ka = presets[activePreset].keys[i];
    bool flash = (i == lastFlashKey);
    if (kaEmpty(ka)) drawCellEmpty(i);
    else drawCell(i, ka.label, nullptr, accent, flash);
  }
}

void drawSysMenu() {
  beginDraw(SCR_SYSMENU);
  drawStatusBar("SYSTEM", C_LTBLUE);
  for (int i = 0; i < NUM_SLOTS; i++) {
    char l1[10]; snprintf(l1, sizeof(l1), "SLOT %d", i + 1);
    const char* l2 = (i == activeSlot) ? (bleConnected ? "ACTIVE" : (pairingMode ? "PAIRING" : "WAITING"))
                                       : (hostSlots[i].bonded ? "linked" : "empty");
    uint16_t col = (i == activeSlot) ? (bleConnected ? C_GREEN : (pairingMode ? C_MAGENTA : C_AMBER))
                                     : (hostSlots[i].bonded ? C_CYAN : C_BORDER);
    drawCell(i, l1, l2, col, i == activeSlot);
  }
  drawCell(3,  "PRESETS", nullptr, C_YELLOW, false);
  drawCellEmpty(4); drawCellEmpty(5); drawCellEmpty(6);
  drawCell(7,  "SETTINGS", nullptr, C_CYAN, false);
  drawCell(8,  "FN", "release=X", C_LTBLUE, false);
  drawCellEmpty(9);
  drawCellEmpty(10);
  drawCell(11, "BUILD", nullptr, C_BUILD, false);
}

void drawPresetPicker() {
  beginDraw(SCR_PRESET);
  drawStatusBar("PRESET", C_YELLOW);
  for (int i = 0; i < NUM_PRESETS; i++)
    drawCell(i, presets[i].name, nullptr, PRESET_COLORS[i], i == activePreset);
  drawCellEmpty(8); drawCellEmpty(9); drawCellEmpty(10);
  drawCell(11, "BACK", nullptr, C_DIM, false);
}

void drawSettings() {
  beginDraw(SCR_SETTINGS);
  drawStatusBar("SETTINGS", C_CYAN);
  char v[10];
  snprintf(v, sizeof(v), "%d", backlightBrightness);
  drawCell(0, "BRIGHT", v, C_CYAN, false);
  int sm = (sleepTimeoutMs == 0) ? 0 : (int)(sleepTimeoutMs / 60000UL);
  if (sm == 0) snprintf(v, sizeof(v), "OFF");
  else         snprintf(v, sizeof(v), "%d min", sm);
  drawCell(1, "SLEEP", v, C_AMBER, false);
  drawCell(2, "OS", linuxLayout ? "LINUX" : "WINDOWS", C_GREEN, false);
  drawCell(3, "DEVICES", nullptr, C_MAGENTA, false);
  drawCell(4, "CONFIG", "WiFi/OTA", C_LTBLUE, false);
  drawCell(5, "FACE", faceMode == 0 ? "OFF" : (faceMode == 1 ? "IDLE" : "ALWAYS"),
           C_PINK, false);
  drawCell(6, "STYLE", faceStyle == 0 ? "EYES" : "GIF", C_PINK, false);
  drawCell(7, "PERSONA", PERSONAS[facePersona].name, C_PINK, false);
  drawCellEmpty(8);                        // K9 = FN — taps as SAVE, keep clear
  drawCell(9, "MEDIA", mediaShow ? "ON" : "OFF", C_PINK, false);
  drawCellEmpty(10);
  drawCell(11, "SAVE", "+ exit", C_WHITE, false);
}

void drawEditor() {
  bool isBright = (currentScreen == SCR_EDIT_BRIGHT);
  beginDraw(currentScreen);
  drawStatusBar(isBright ? "BRIGHTNESS" : "SLEEP", isBright ? C_CYAN : C_AMBER);

  char v[12];
  if (isBright) snprintf(v, sizeof(v), "%d", backlightBrightness);
  else {
    int sm = (sleepTimeoutMs == 0) ? 0 : (int)(sleepTimeoutMs / 60000UL);
    if (sm == 0) snprintf(v, sizeof(v), "OFF");
    else         snprintf(v, sizeof(v), "%d min", sm);
  }
  // Clear the number band so a shorter value can't leave ghost digits
  // (this screen refreshes in place without a full clear)
  tft.fillRect(0, 66, 320, 48, C_BG);
  tft.setTextSize(4);
  tft.setTextColor(C_WHITE, C_BG);
  int w = strlen(v) * 24;
  tft.setCursor((320 - w) / 2, 70);
  tft.print(v);

  // bar
  int maxV = isBright ? 255 : 60;
  int curV = isBright ? backlightBrightness
                      : ((sleepTimeoutMs == 0) ? 0 : (int)(sleepTimeoutMs / 60000UL));
  tft.drawRoundRect(40, 130, 240, 14, 5, C_BORDER);
  int bw = (curV * 236) / maxV;
  if (bw > 0) tft.fillRoundRect(42, 132, bw, 10, 4, isBright ? C_CYAN : C_AMBER);

  tft.setTextSize(1);
  tft.setTextColor(C_DIM, C_BG);
  tft.setCursor(48, 170);  tft.print("K5 = -");
  tft.setCursor(232, 170); tft.print("K8 = +");
  tft.setCursor(116, 200); tft.print("K12 / FN = done");
}

void drawDevices() {
  beginDraw(SCR_DEVICES);
  drawStatusBar("DEVICES", C_MAGENTA);
  for (int i = 0; i < NUM_SLOTS; i++) {
    int y = 30 + i * 56;
    bool act = (i == activeSlot);
    uint16_t col = act ? (bleConnected ? C_GREEN : (pairingMode ? C_MAGENTA : C_AMBER)) : C_BORDER;
    tft.fillRoundRect(4, y, 312, 50, 8, C_SURF);
    tft.drawRoundRect(4, y, 312, 50, 8, col);
    tft.setTextSize(2);
    tft.setTextColor(act ? col : C_WHITE, C_SURF);
    tft.setCursor(14, y + 8);
    tft.printf("SLOT %d", i + 1);
    tft.setTextSize(1);
    tft.setTextColor(C_DIM, C_SURF);
    tft.setCursor(14, y + 32);
    if (hostSlots[i].bonded) {
      const uint8_t* a = hostSlots[i].addr;
      tft.printf("%02X:%02X:%02X:%02X:%02X:%02X", a[5], a[4], a[3], a[2], a[1], a[0]);
    } else tft.print("empty — hold key to pair");
    if (act) {
      tft.setTextColor(col, C_SURF);
      tft.setCursor(230, y + 8);
      tft.print(bleConnected ? "CONNECTED" : (pairingMode ? "PAIRING..." : "WAITING"));
    }
  }
  tft.setTextSize(1);
  tft.setTextColor(C_DIM, C_BG);
  tft.setCursor(8, 226);
  tft.print("K1-3 switch  hold=pair  K5-7 clear  K11 hold=wipe all  K12 back");
}

void drawBuildPreset() {
  beginDraw(SCR_BUILD_PRESET);
  drawStatusBar("BUILD: PICK", C_BUILD);
  for (int i = 0; i < NUM_PRESETS; i++)
    drawCell(i, presets[i].name, nullptr, PRESET_COLORS[i], false);
  drawCellEmpty(8); drawCellEmpty(9); drawCellEmpty(10);
  drawCell(11, "CANCEL", nullptr, C_DIM, false);
}

void drawBuildKeys() {
  beginDraw(SCR_BUILD_KEYS);
  char t[16];
  snprintf(t, sizeof(t), "ED:%s", presets[buildPreset].name);
  drawStatusBar(t, C_BUILD);
  for (int i = 0; i < NUM_KEYS; i++) {
    KeyAction& ka = presets[buildPreset].keys[i];
    if (i == KEY_FN)
      drawCell(i, kaEmpty(ka) ? "---" : ka.label, "tap=DONE", C_BUILD, false);
    else
      drawCell(i, kaEmpty(ka) ? "---" : ka.label, nullptr, C_BUILD, false);
  }
}

void drawBuildAction() {
  beginDraw(SCR_BUILD_ACTION);
  int pages = (ACTION_LIB_SIZE + 7) / 8;
  char t[18];
  snprintf(t, sizeof(t), "K%d %d/%d", buildSlot + 1, buildActPage + 1, pages);
  drawStatusBar(t, C_BUILD);
  for (int i = 0; i < 8; i++) {
    int idx = buildActPage * 8 + i;
    if (idx < ACTION_LIB_SIZE)
      drawCell(i, ACTION_LIB[idx].label, nullptr, C_BUILD, false);
    else drawCellEmpty(i);
  }
  drawCell(8,  "BACK", nullptr, C_DIM, false);
  drawCellEmpty(9);
  drawCell(10, "< PAGE", nullptr, C_LTBLUE, false);
  drawCell(11, "PAGE >", nullptr, C_LTBLUE, false);
}

void drawReconnectHUD(const char* keyName) {
  beginDraw(200);   // distinct id: clears when entered, animates without re-clearing
  drawStatusBar("RECONNECT", C_AMBER);
  uint16_t pulse = ((millis() / 300) % 2) ? C_AMBER : C_SURF2;
  tft.fillCircle(160, 110, 30, C_SURF2);
  tft.drawCircle(160, 110, 30, pulse);
  tft.drawCircle(160, 110, 22, pulse);
  tft.setTextSize(3);
  tft.setTextColor(C_AMBER, C_SURF2);
  tft.setCursor(152, 100);
  tft.print("B");
  tft.fillRoundRect(80, 160, 160, 34, 8, C_SURF2);
  tft.drawRoundRect(80, 160, 160, 34, 8, C_AMBER);
  tft.setTextSize(1);
  tft.setTextColor(C_DIM, C_SURF2);
  tft.setCursor(90, 166);
  tft.print("Queued:");
  tft.setTextSize(2);
  tft.setTextColor(C_WHITE, C_SURF2);
  int kx = 160 - (int)strlen(keyName) * 6;
  tft.setCursor(max(90, kx), 176);
  tft.print(keyName);
  tft.setTextSize(1);
  tft.setTextColor(C_DIM, C_BG);
  tft.setCursor(96, 210);
  tft.print("Will fire when connected");
}

void redraw() {
  switch (currentScreen) {
    case SCR_MAIN:         drawMain();         break;
    case SCR_SYSMENU:      drawSysMenu();      break;
    case SCR_PRESET:       drawPresetPicker(); break;
    case SCR_SETTINGS:     drawSettings();     break;
    case SCR_EDIT_BRIGHT:
    case SCR_EDIT_SLEEP:   drawEditor();       break;
    case SCR_DEVICES:      drawDevices();      break;
    case SCR_BUILD_PRESET: drawBuildPreset();  break;
    case SCR_BUILD_KEYS:   drawBuildKeys();    break;
    case SCR_BUILD_ACTION: drawBuildAction();  break;
    case SCR_FACE:         faceEnter();        break;
  }
}

// ════════════════════════════════════════════════
//  HOST-LINK — event senders + command drain (loop task only)
// ════════════════════════════════════════════════
static void hostNotify(const uint8_t* buf, size_t n) {
  if (!hostAppSubscribed || !pEvtChar) return;
  pEvtChar->setValue(buf, n);
  pEvtChar->notify();
}

void hostNotifyKey(uint8_t keyIdx) {
  uint8_t ev[4] = { HEV_KEY, 2, (uint8_t)activePreset, keyIdx };
  hostNotify(ev, sizeof(ev));
}

void hostNotifyPreset() {
  uint8_t ev[3] = { HEV_PRESET, 1, (uint8_t)activePreset };
  hostNotify(ev, sizeof(ev));
}

// Drain queued companion commands. Runs every loop pass; the queue is almost
// always empty so the common case is two array-index compares.
void hostLinkTick() {
  if (hostHelloPending) {
    hostHelloPending = false;
    uint8_t ev[8] = { HEV_HELLO, 6, 5 /*fw major*/, NUM_KEYS, NUM_PRESETS,
                      (uint8_t)activePreset, faceMode, facePersona };
    hostNotify(ev, sizeof(ev));
  }
  while (hostCmdTail != hostCmdHead) {
    HostCmd& q = hostCmdQ[hostCmdTail];
    uint8_t op = q.data[0], n = q.data[1];
    const uint8_t* p = q.data + 2;
    if ((size_t)(2 + n) <= q.len) switch (op) {

      case HCMD_LABEL:                     // [preset][key][utf8 ≤8]
        if (n >= 2 && p[0] < NUM_PRESETS && p[1] < NUM_KEYS) {
          KeyAction& ka = presets[p[0]].keys[p[1]];
          int L = min((int)n - 2, 8);
          memcpy(ka.label, p + 2, L); ka.label[L] = '\0';
          if (currentScreen == SCR_MAIN && p[0] == activePreset) drawMain();
        }
        break;

      case HCMD_STATUS: {                  // [utf8 ≤23]; empty clears
        int L = min((int)n, (int)sizeof(hostStatus) - 1);
        memcpy(hostStatus, p, L); hostStatus[L] = '\0';
        if (currentScreen == SCR_MAIN)
          drawStatusBar(hostStatus[0] ? hostStatus : presets[activePreset].name,
                        PRESET_COLORS[activePreset]);
        break;
      }

      case HCMD_PRESET:                    // [preset] — foreground-follow
        if (n >= 1 && p[0] < NUM_PRESETS && p[0] != activePreset) {
          activePreset = p[0];
          hostStatus[0] = '\0';            // stale app status dies with the app
          if (currentScreen == SCR_MAIN) drawMain();
          hostNotifyPreset();              // echo so both sides agree
        }
        break;

      case HCMD_FACE:                      // [mode 0-2][persona 0-3]
        if (n >= 2) {
          if (p[0] <= 2) faceMode = p[0];
          if (p[1] < NUM_PERSONAS) facePersona = p[1];
        }
        break;

      case HCMD_COLOR:                     // [preset][rgb565 hi][rgb565 lo]
        if (n >= 3 && p[0] < NUM_PRESETS) {
          PRESET_COLORS[p[0]] = (uint16_t)((p[1] << 8) | p[2]);
          // Eyes pick the new color up next frame via faceEyeColor(); the
          // grid only needs a repaint if this preset is on screen now.
          if (currentScreen == SCR_MAIN && p[0] == activePreset) drawMain();
        }
        break;

      case HCMD_KEY:                       // full key reassignment from the app
        // [preset][key][kaType][mod][hid][cons lo][cons hi][label…]
        if (n >= 7 && p[0] < NUM_PRESETS && p[1] < NUM_KEYS && p[2] <= KA_HOST
            && p[2] != KA_MACRO) {         // macros stay web-UI-only (steps > MTU)
          KeyAction& ka = presets[p[0]].keys[p[1]];
          char keepText[24];               // KA_TEXT payload arrives via HCMD_TEXT
          memcpy(keepText, ka.text, sizeof(keepText));
          ka = KeyAction{};
          ka.type = p[2];
          ka.mod  = p[3];
          ka.key  = p[4];
          ka.consumer = (uint16_t)(p[5] | (p[6] << 8));
          if (ka.type == KA_BUILTIN) ka.id = (int16_t)ka.consumer;  // field reused as id
          if (ka.type == KA_TEXT) memcpy(ka.text, keepText, sizeof(ka.text));
          int L = min((int)n - 7, 8);
          if (L > 0) memcpy(ka.label, p + 7, L);
          ka.label[max(0, L)] = '\0';
          if (currentScreen == SCR_MAIN && p[0] == activePreset) drawMain();
        }
        break;

      case HCMD_TEXT:                      // [preset][key][utf8 ≤23]
        if (n >= 2 && p[0] < NUM_PRESETS && p[1] < NUM_KEYS) {
          KeyAction& ka = presets[p[0]].keys[p[1]];
          int L = min((int)n - 2, 23);
          memcpy(ka.text, p + 2, L); ka.text[L] = '\0';
        }
        break;

      case HCMD_COMMIT:                    // one NVS write per editor save
        savePresets();
        break;

      case HCMD_EYES:                      // [rgb565 hi][lo][persist]
        // 0x0000 = follow the active preset's color. Session pushes come
        // with persist=0 so a reconnect never costs an NVS write.
        if (n >= 2) {
          faceCfg.color = (uint16_t)((p[0] << 8) | p[1]);
          if (n >= 3 && p[2]) saveFaceCfg();
        }
        break;

      case HCMD_MEDIA:                     // [flags][pos lo][hi][dur lo][hi][title]
        if (n >= 5) {
          mediaPlaying = (p[0] & 0x01) != 0;
          bool fav     = (p[0] & 0x02) != 0;
          mediaPosS = (uint16_t)(p[1] | (p[2] << 8));
          mediaDurS = (uint16_t)(p[3] | (p[4] << 8));
          char t[sizeof(mediaTitle)];
          int L = min((int)n - 5, (int)sizeof(t) - 1);
          memcpy(t, p + 5, L); t[L] = '\0';
          // Edge-detect for the face: a new track, or this one just got loved
          if (strcmp(t, mediaTitle) != 0 && t[0]) mediaNewSong = true;
          if (fav && !mediaFav)                   mediaJustFaved = true;
          memcpy(mediaTitle, t, sizeof(t));
          mediaFav = fav;
          mediaRxMs = millis();
        }
        break;
    }
    hostCmdTail = (uint8_t)((hostCmdTail + 1) % HOSTCMD_QMAX);
  }
}

// Small heart for the now-playing strip: two lobes + a point.
static void drawHeart(int cx, int cy, uint16_t c) {
  sprBar.fillCircle(cx - 2, cy - 1, 3, c);
  sprBar.fillCircle(cx + 2, cy - 1, 3, c);
  sprBar.fillTriangle(cx - 5, cy, cx + 5, cy, cx, cy + 6, c);
}

// ── Now-playing strip under the face ────────────
// Reuses sprBar (320×26) pushed at the bottom edge — well clear of the eye
// sprites. Redrawn once a second; the wipe only ever runs on SCR_FACE.
void mediaStripTick(unsigned long now) {
  static unsigned long lastDraw = 0;
  static bool wasVisible = false;
  bool live = mediaShow && mediaTitle[0] && (now - mediaRxMs < 30000UL);
  if (!live) {
    if (wasVisible) { tft.fillRect(0, 214, 320, 26, C_BG); wasVisible = false; }
    return;
  }
  if (wasVisible && now - lastDraw < 1000) return;
  lastDraw = now;

  uint32_t pos = mediaPosS;
  if (mediaPlaying) pos += (now - mediaRxMs) / 1000UL;
  if (mediaDurS && pos > mediaDurS) pos = mediaDurS;

  uint16_t acc = faceEyeColor();
  sprBar.fillSprite(C_BG);
  sprBar.setTextSize(1);
  sprBar.setTextColor(C_WHITE, C_BG);
  int w = strlen(mediaTitle) * 6;
  // Centre title+heart as one unit so the title doesn't shift when it's loved
  int tx = max(2, (320 - (w + (mediaFav ? 14 : 0))) / 2);
  if (mediaFav) { drawHeart(tx + 5, 4, C_PINK); tx += 14; }
  sprBar.setCursor(tx, 0);
  sprBar.print(mediaTitle);

  sprBar.drawRoundRect(40, 14, 240, 7, 3, C_SURF2);
  if (mediaDurS) {
    int fw = (int)(236.0f * pos / mediaDurS);
    sprBar.fillRoundRect(42, 16, max(2, fw), 3, 1, acc);
  }
  char tb[8];
  snprintf(tb, sizeof(tb), "%lu:%02lu", pos / 60, pos % 60);
  sprBar.setTextColor(C_DIM, C_BG);
  sprBar.setCursor(4, 14); sprBar.print(tb);
  if (mediaDurS) {
    snprintf(tb, sizeof(tb), "%u:%02u", mediaDurS / 60, mediaDurS % 60);
    sprBar.setCursor(286, 14); sprBar.print(tb);
  }
  sprBar.pushSprite(0, 214);
  wasVisible = true;
}

// ════════════════════════════════════════════════
//  FIRE ACTION  (OS-layout aware)
// ════════════════════════════════════════════════
void fireAction(int id) {
  if (id == A_NONE || !bleConnected) return;
  switch (id) {
    // ── Media / Consumer (same on all OS) ──────
    case A_PLAY:      sendConsumer(CONSUMER_PLAY_PAUSE); break;
    case A_NEXT:      sendConsumer(CONSUMER_NEXT);       break;
    case A_PREV:      sendConsumer(CONSUMER_PREV);       break;
    case A_MUTE:      sendConsumer(CONSUMER_MUTE);       break;
    case A_VOLUP:     sendConsumer(CONSUMER_VOL_UP);     break;
    case A_VOLDN:     sendConsumer(CONSUMER_VOL_DOWN);   break;

    // ── Universal text editing ─────────────────
    case A_UNDO:      sendKey(MOD_LCTRL, KEY_Z); break;
    case A_REDO:
      if (linuxLayout) sendKey(MOD_LCTRL | MOD_LSHIFT, KEY_Z);
      else             sendKey(MOD_LCTRL, KEY_Y);
      break;
    case A_SAVE:      sendKey(MOD_LCTRL, KEY_S); break;
    case A_COPY:      sendKey(MOD_LCTRL, KEY_C); break;
    case A_PASTE:     sendKey(MOD_LCTRL, KEY_V); break;
    case A_CUT:       sendKey(MOD_LCTRL, KEY_X); break;
    case A_SELALL:    sendKey(MOD_LCTRL, KEY_A); break;
    case A_FIND:      sendKey(MOD_LCTRL, KEY_F); break;
    case A_REPLACE:   sendKey(MOD_LCTRL, KEY_H); break;
    case A_NEWFILE:   sendKey(MOD_LCTRL, KEY_N); break;
    case A_OPENFILE:  sendKey(MOD_LCTRL, KEY_O); break;
    case A_CLOSE:     sendKey(MOD_LALT, KEY_F4); break;
    case A_ALTTAB:    sendKey(MOD_LALT, KEY_TAB); break;

    // ── OS-specific system actions ─────────────
    case A_LOCK:      sendKey(MOD_LGUI, KEY_L); break;   // Super+L / Win+L
    case A_SNIP:
      if (linuxLayout) sendKey(MOD_LSHIFT, KEY_PRTSC);            // GNOME region
      else             sendKey(MOD_LGUI | MOD_LSHIFT, KEY_S);     // Win+Shift+S
      break;
    case A_SS_FULL:
      if (linuxLayout) sendKey(0, KEY_PRTSC);
      else             sendKey(MOD_LGUI, KEY_PRTSC);
      break;
    case A_TASK:      sendKey(MOD_LGUI, KEY_TAB); break;  // Activities / Win+Tab
    case A_MINALL:
      if (linuxLayout) sendKey(MOD_LGUI, KEY_D);
      else             sendKey(MOD_LGUI, KEY_M);
      break;
    case A_DESK_R:
      if (linuxLayout) sendKey(MOD_LCTRL | MOD_LALT, KEY_RIGHT_ARR);
      else             sendKey(MOD_LCTRL | MOD_LGUI, KEY_RIGHT_ARR);
      break;
    case A_DESK_L:
      if (linuxLayout) sendKey(MOD_LCTRL | MOD_LALT, KEY_LEFT_ARR);
      else             sendKey(MOD_LCTRL | MOD_LGUI, KEY_LEFT_ARR);
      break;
    case A_EXPLORER:  sendKey(MOD_LGUI, KEY_E); break;
    case A_SETTINGS_W: sendKey(MOD_LGUI, KEY_I); break;
    case A_SPOTIFY:   sendKey(MOD_LGUI, KEY_3); break;
    case A_CALC:
      if (linuxLayout) {
        sendKey(MOD_LGUI, 0);        // open launcher, type manually
      } else {
        sendKey(MOD_LGUI, KEY_R); delay(300);
        { uint8_t calcKeys[] = {KEY_C, KEY_A, KEY_L, KEY_C};
          for (int i = 0; i < 4; i++) { sendKey(0, calcKeys[i]); delay(30); }
          sendKey(0, KEY_ENTER); }
      }
      break;

    // ── OnShape ────────────────────────────────
    case A_OS_FIT:       sendKey(0, KEY_F); break;
    case A_OS_FRONT:     sendKey(MOD_LSHIFT, KEY_1); break;
    case A_OS_TOP:       sendKey(MOD_LSHIFT, KEY_2); break;
    case A_OS_RIGHT:     sendKey(MOD_LSHIFT, KEY_3); break;
    case A_OS_ISO:       sendKey(MOD_LSHIFT, KEY_7); break;
    case A_OS_ZOOM_FIT:  sendKey(MOD_LCTRL | MOD_LSHIFT, KEY_F); break;
    case A_OS_EXTRUDE:   sendKey(MOD_LSHIFT, KEY_E); break;
    case A_OS_SKETCH:    sendKey(0, KEY_S); break;
    case A_OS_MATE:      sendKey(MOD_LCTRL, KEY_M); break;
    case A_OS_ASSEMBLY:  sendKey(MOD_LALT, KEY_A); break;

    // ── KiCad ──────────────────────────────────
    case A_KC_ROUTE:     sendKey(0, KEY_X); break;
    case A_KC_ADD_NET:   sendKey(0, KEY_W); break;
    case A_KC_ZOOM_FIT:  sendKey(0, KEY_5); break;
    case A_KC_DRC:       sendKey(MOD_LALT, KEY_3); break;
    case A_KC_3D:        sendKey(0, KEY_3); break;
    case A_KC_COPPER:    sendKey(MOD_LCTRL, KEY_K); break;
    case A_KC_GERBER:    sendKey(MOD_LCTRL, KEY_P); break;
    case A_KC_RATSNEST:  sendKey(0, KEY_BACKTICK); break;

    // ── Gaming ─────────────────────────────────
    case A_PUSH_TO_TALK: sendKey(0, KEY_V); break;
    case A_RELOAD:       sendKey(0, KEY_R); break;
    case A_MAP:          sendKey(0, KEY_M); break;
    case A_SCORE:        sendKey(0, KEY_TAB); break;
    case A_FULLSCREEN:   sendKey(0, KEY_F11); break;
    case A_OBS_REC:      sendKey(MOD_LCTRL | MOD_LALT, KEY_R); break;
    case A_OBS_STREAM:   sendKey(MOD_LCTRL | MOD_LALT, KEY_S); break;
    case A_DISCORD:      sendKey(MOD_LCTRL | MOD_LALT, KEY_A); break;

    // ── Browser ────────────────────────────────
    case A_NEW_TAB:      sendKey(MOD_LCTRL, KEY_T); break;
    case A_CLOSE_TAB:    sendKey(MOD_LCTRL, KEY_W); break;
    case A_RETAB:        sendKey(MOD_LCTRL | MOD_LSHIFT, KEY_T); break;
    case A_BACK:         sendKey(MOD_LALT, KEY_LEFT_ARR); break;
    case A_FORWARD:      sendKey(MOD_LALT, KEY_RIGHT_ARR); break;
    case A_REFRESH:      sendKey(MOD_LCTRL, KEY_R); break;
    case A_ADDR_BAR:     sendKey(MOD_LCTRL, KEY_L); break;

    // ── LTspice ────────────────────────────────
    case A_LTS_MOVE:     sendKey(0, KEY_M); break;
    case A_LTS_GND:      sendKey(0, KEY_G); break;
    case A_LTS_VCC:      sendKey(0, KEY_V); break;
    case A_LTS_RES:      sendKey(0, KEY_R); break;
    case A_LTS_CAP:      sendKey(0, KEY_C); break;
    case A_LTS_COMP:     sendKey(0, KEY_P); break;
    case A_LTS_WIRE:     sendKey(0, KEY_W); break;
    case A_LTS_RUN:      sendKey(MOD_LALT, KEY_R); break;

    // ── Linux / GNOME exclusive ────────────────
    case A_LX_TERMINAL:  sendKey(MOD_LCTRL | MOD_LALT, KEY_T); break;
    case A_LX_ZOOM_IN:   sendKey(MOD_LCTRL, KEY_EQUAL); break;
    case A_LX_ZOOM_OUT:  sendKey(MOD_LCTRL, KEY_MINUS); break;
    case A_LX_ZOOM_RST:  sendKey(MOD_LCTRL, KEY_0); break;
    case A_LX_NOTIF:     sendKey(MOD_LGUI, KEY_V); break;
    case A_LX_SPLIT_H:   sendKey(MOD_LCTRL | MOD_LSHIFT, KEY_E); break;
    case A_LX_SPLIT_V:   sendKey(MOD_LCTRL | MOD_LSHIFT, KEY_O); break;
    case A_LX_NEW_TERM:  sendKey(MOD_LCTRL | MOD_LSHIFT, KEY_T); break;
    case A_LX_MAXIMIZE:  sendKey(MOD_LGUI, KEY_UP_ARR); break;
    case A_LX_HALF_L:    sendKey(MOD_LGUI, KEY_LEFT_ARR); break;
    case A_LX_HALF_R:    sendKey(MOD_LGUI, KEY_RIGHT_ARR); break;
    case A_LX_MOVE_WS1:  sendKey(MOD_LGUI | MOD_LSHIFT, KEY_1); break;
    case A_LX_MOVE_WS2:  sendKey(MOD_LGUI | MOD_LSHIFT, KEY_2); break;
  }
}

// ════════════════════════════════════════════════
//  TYPE-STRING — ASCII → HID for KA_TEXT / macros
// ════════════════════════════════════════════════
// Returns HID keycode for a printable ASCII char and sets `shift`.
static uint8_t asciiToHid(char c, bool& shift) {
  shift = false;
  if (c >= 'a' && c <= 'z') return KEY_A + (c - 'a');
  if (c >= 'A' && c <= 'Z') { shift = true; return KEY_A + (c - 'A'); }
  if (c >= '1' && c <= '9') return KEY_1 + (c - '1');
  switch (c) {
    case '0': return KEY_0;
    case ' ': return 0x2C;                 // space
    case '\n': case '\r': return KEY_ENTER;
    case '\t': return KEY_TAB;
    case '-': return KEY_MINUS;   case '_': shift = true; return KEY_MINUS;
    case '=': return KEY_EQUAL;   case '+': shift = true; return KEY_EQUAL;
    case '`': return KEY_BACKTICK;case '~': shift = true; return KEY_BACKTICK;
    case '.': return 0x37;        case '>': shift = true; return 0x37;
    case ',': return 0x36;        case '<': shift = true; return 0x36;
    case '/': return 0x38;        case '?': shift = true; return 0x38;
    case ';': return 0x33;        case ':': shift = true; return 0x33;
    case '\'':return 0x34;        case '"': shift = true; return 0x34;
    case '[': return 0x2F;        case '{': shift = true; return 0x2F;
    case ']': return 0x30;        case '}': shift = true; return 0x30;
    case '\\':return 0x31;        case '|': shift = true; return 0x31;
    case '!': shift = true; return KEY_1;   case '@': shift = true; return KEY_2;
    case '#': shift = true; return KEY_3;   case '$': shift = true; return 0x21;
    case '%': shift = true; return 0x22;    case '^': shift = true; return 0x23;
    case '&': shift = true; return KEY_7;   case '*': shift = true; return 0x25;
    case '(': shift = true; return 0x26;    case ')': shift = true; return KEY_0;
  }
  return 0;
}

void typeString(const char* s) {
  for (const char* p = s; *p; p++) {
    bool shift; uint8_t k = asciiToHid(*p, shift);
    if (k) { sendKey(shift ? MOD_LSHIFT : 0, k); delay(6); }
  }
}

// ════════════════════════════════════════════════
//  FIRE KEY — dispatch a rich KeyAction by type
// ════════════════════════════════════════════════
void fireKeyAction(const KeyAction& ka, int keyIdx) {
  if (!bleConnected) return;
  if (ka.type == KA_HOST) {
    // Companion event, not a keystroke. Falls back to the chord baked into
    // the key so the pad still does something when no app is listening.
    if (hostAppSubscribed && keyIdx >= 0) hostNotifyKey((uint8_t)keyIdx);
    else if (ka.key || ka.mod)            sendKey(ka.mod, ka.key);
    return;
  }
  switch (ka.type) {
    case KA_BUILTIN:  fireAction(ka.id); break;
    case KA_KEY:      sendKey(ka.mod, ka.key); break;
    case KA_CONSUMER: sendConsumer(ka.consumer); break;
    case KA_TEXT:     typeString(ka.text); break;
    case KA_MACRO:
      for (int s = 0; s < ka.nSteps && s < MACRO_MAX; s++) {
        const MacroStep& st = ka.steps[s];
        if (st.key) sendKey(st.mod, st.key);
        else if (st.consumer) sendConsumer(st.consumer);
        if (st.delayCs) delay((unsigned long)st.delayCs * 10);
      }
      break;
  }
}

// ════════════════════════════════════════════════
//  INPUT — per-screen tap / hold handlers
// ════════════════════════════════════════════════
unsigned long holdThresholdFor(int i) {   // 0 = no hold action for this key
  switch (currentScreen) {
    case SCR_MAIN:       return (i == KEY_FN) ? FN_MENU_MS : 0;
    // ALWAYS mode: the face IS the main screen, so it inherits SCR_MAIN's
    // hold map. Without this the face path fired taps on key-down and the
    // FN hold arrived second — FN typed its tap action before opening the menu
    case SCR_FACE:       return (faceMode == 2 && i == KEY_FN) ? FN_MENU_MS : 0;
    case SCR_SYSMENU:    return (i <= 2) ? PAIR_HOLD_MS : 0;
    case SCR_DEVICES:    return (i <= 2) ? PAIR_HOLD_MS : ((i == 10) ? CLRALL_HOLD_MS : 0);
    case SCR_BUILD_KEYS: return (i == KEY_FN) ? PAIR_HOLD_MS : 0;
    default:             return 0;
  }
}

void onKeyHold(int i) {
  recordActivity();
  char msg[24];
  switch (currentScreen) {
    case SCR_MAIN:
      if (i == KEY_FN) { currentScreen = SCR_SYSMENU; drawSysMenu(); }
      break;
    case SCR_FACE:            // ALWAYS mode: FN opens the menu from the face
      if (i == KEY_FN) {
        faceGifStop();
        currentScreen = SCR_SYSMENU; screenDirty = true; drawSysMenu();
      }
      break;
    case SCR_SYSMENU:
      if (i <= 2) {
        switchToSlot(i, true);
        currentScreen = SCR_MAIN;
        drawMain();
        snprintf(msg, sizeof(msg), "PAIRING SLOT %d", i + 1);
        showToast(msg, C_MAGENTA, 1500);
      }
      break;
    case SCR_DEVICES:
      if (i <= 2) { switchToSlot(i, true); drawDevices(); }
      else if (i == 10) {
        clearAllBonds();
        drawDevices();
        showToast("ALL BONDS WIPED", C_RED, 1500);
      }
      break;
    case SCR_BUILD_KEYS:
      if (i == KEY_FN) {  // edit the FN key's own macro slot
        buildSlot = KEY_FN; buildActPage = 0;
        currentScreen = SCR_BUILD_ACTION; drawBuildAction();
      }
      break;
    default: break;
  }
}

void onKeyTap(int i) {
  recordActivity();
  char msg[24];
  switch (currentScreen) {

    case SCR_MAIN: {
      KeyAction& ka = presets[activePreset].keys[i];
      if (kaEmpty(ka)) return;
      lastFlashKey = i; flashUntil = millis() + FLASH_MS;
      drawCell(i, ka.label, nullptr, PRESET_COLORS[activePreset], true);
      fireKeyAction(ka, i);
      break;
    }

    // ALWAYS mode types straight from the face — no grid, no cell flash,
    // the eyes acknowledge the press instead
    case SCR_FACE: {
      KeyAction& ka = presets[activePreset].keys[i];
      if (kaEmpty(ka)) return;
      faceKeyReact(i);
      fireKeyAction(ka, i);
      break;
    }

    case SCR_SYSMENU:
      if (i <= 2) {
        if (i == activeSlot && bleConnected) {
          currentScreen = SCR_MAIN; drawMain();
          snprintf(msg, sizeof(msg), "SLOT %d ACTIVE", i + 1);
          showToast(msg, C_GREEN, 900);
        } else {
          switchToSlot(i, false);
          currentScreen = SCR_MAIN; drawMain();
          snprintf(msg, sizeof(msg), hostSlots[i].bonded ? "SLOT %d" : "PAIR SLOT %d",
                   i + 1);
          showToast(msg, hostSlots[i].bonded ? C_CYAN : C_MAGENTA, 1200);
        }
      }
      else if (i == 3)  { currentScreen = SCR_PRESET;       drawPresetPicker(); }
      else if (i == 7)  { currentScreen = SCR_SETTINGS;     drawSettings(); }
      else if (i == 11) { buildPreset = activePreset;
                          currentScreen = SCR_BUILD_PRESET; drawBuildPreset(); }
      break;

    case SCR_PRESET:
      if (i < NUM_PRESETS) {
        activePreset = i;
        hostStatus[0] = '\0';        // app status belongs to the old context
        saveSettings();
        hostNotifyPreset();          // manual switch — tell the companion
        currentScreen = SCR_MAIN; drawMain();
      } else if (i == 11 || i == KEY_FN) {
        currentScreen = SCR_MAIN; drawMain();
      }
      break;

    case SCR_SETTINGS:
      if      (i == 0) { currentScreen = SCR_EDIT_BRIGHT; drawEditor(); }
      else if (i == 1) { currentScreen = SCR_EDIT_SLEEP;  drawEditor(); }
      else if (i == 2) { linuxLayout = !linuxLayout; drawSettings(); }
      else if (i == 3) { currentScreen = SCR_DEVICES; drawDevices(); }
      else if (i == 4) { saveSettings(); enterConfigMode(); }  // no return — reboots on exit
      else if (i == 5) { faceMode = (faceMode + 1) % 3; drawSettings(); }
      else if (i == 6) { faceStyle = (faceStyle + 1) % 2; drawSettings(); }
      else if (i == 7) { facePersona = (facePersona + 1) % NUM_PERSONAS; drawSettings(); }
      else if (i == 9) { mediaShow = !mediaShow; drawSettings(); }
      else if (i == 11 || i == KEY_FN) {
        saveSettings();
        currentScreen = SCR_MAIN; drawMain();
        showToast("SAVED", C_GREEN, 800);
      }
      break;

    case SCR_EDIT_BRIGHT:
      if (i == 4) { backlightBrightness = max(10,  backlightBrightness - 15);
                    ledcWrite(TFT_BL, backlightBrightness); drawEditor(); }
      else if (i == 7) { backlightBrightness = min(255, backlightBrightness + 15);
                    ledcWrite(TFT_BL, backlightBrightness); drawEditor(); }
      else if (i == 11 || i == KEY_FN) { currentScreen = SCR_SETTINGS; drawSettings(); }
      break;

    case SCR_EDIT_SLEEP: {
      int sm = (sleepTimeoutMs == 0) ? 0 : (int)(sleepTimeoutMs / 60000UL);
      if      (i == 4) { sm = max(0, sm - 1); }
      else if (i == 7) { sm = min(60, sm + 1); }
      else if (i == 11 || i == KEY_FN) { currentScreen = SCR_SETTINGS; drawSettings(); break; }
      else break;
      sleepTimeoutMs = (sm == 0) ? 0UL : (unsigned long)sm * 60000UL;
      drawEditor();
      break;
    }

    case SCR_DEVICES:
      if (i <= 2) { switchToSlot(i, false); drawDevices(); }
      else if (i >= 4 && i <= 6) {
        clearSlotBond(i - 4);
        drawDevices();
        snprintf(msg, sizeof(msg), "SLOT %d CLEARED", i - 3);
        showToast(msg, C_RED, 1200);
      }
      else if (i == 11 || i == KEY_FN) { currentScreen = SCR_SETTINGS; drawSettings(); }
      break;

    case SCR_BUILD_PRESET:
      if (i < NUM_PRESETS) { buildPreset = i; currentScreen = SCR_BUILD_KEYS; drawBuildKeys(); }
      else if (i == 11)    { currentScreen = SCR_MAIN; drawMain(); }
      break;

    case SCR_BUILD_KEYS:
      if (i == KEY_FN) {   // done — persist and leave
        savePresets();
        activePreset = buildPreset;
        saveSettings();
        hostNotifyPreset();
        currentScreen = SCR_MAIN; drawMain();
        showToast("PRESET SAVED", C_GREEN, 1200);
      } else {
        buildSlot = i; buildActPage = 0;
        currentScreen = SCR_BUILD_ACTION; drawBuildAction();
      }
      break;

    case SCR_BUILD_ACTION: {
      int pages = (ACTION_LIB_SIZE + 7) / 8;
      if (i < 8) {
        int idx = buildActPage * 8 + i;
        if (idx < ACTION_LIB_SIZE) {
          KeyAction& ka = presets[buildPreset].keys[buildSlot];
          ka = KeyAction{};                        // clear macro/text/key fields
          strncpy(ka.label, ACTION_LIB[idx].label, 8);
          ka.label[8] = '\0';
          ka.type = KA_BUILTIN;
          ka.id   = ACTION_LIB[idx].id;
          currentScreen = SCR_BUILD_KEYS; drawBuildKeys();
        }
      }
      else if (i == 8)  { currentScreen = SCR_BUILD_KEYS; drawBuildKeys(); }
      else if (i == 10) { buildActPage = (buildActPage + pages - 1) % pages; drawBuildAction(); }
      else if (i == 11) { buildActPage = (buildActPage + 1) % pages; drawBuildAction(); }
      break;
    }

    default: break;
  }
}

void onKeyUp(int i, unsigned long heldMs) {
  // FN release always closes the SYSTEM menu, hold-fired or not
  if (i == KEY_FN && currentScreen == SCR_SYSMENU) {
    currentScreen = SCR_MAIN; drawMain();
    return;
  }
  if (keyFiredOnDown[i]) { keyFiredOnDown[i] = false; return; }  // already fired
  if (keyHoldFired[i]) return;                 // hold action already consumed it
  if (heldMs < KEY_MIN_PRESS_MS) return;
  onKeyTap(i);
}

// ════════════════════════════════════════════════
//  FACE — state-reactive robot eyes + optional GIF loop
//  Procedural: every visual parameter comes from faceCfg (the uploadable
//  "expression pack"), state from live BLE globals. Renders both eyes
//  through one reused sprite — no full-screen clears while animating.
// ════════════════════════════════════════════════
float eyeOpen = 1.0f, eyeOpenTarget = 1.0f;
float eyeGlanceX = 0, eyeGlanceY = 0, eyeGlanceTX = 0, eyeGlanceTY = 0;
bool  faceBlinking = false;
unsigned long faceNextBlink = 0, faceBlinkEnd = 0;
unsigned long faceNextGlance = 0, faceGlanceEnd = 0;
unsigned long faceDartNext = 0;
unsigned long faceFrameMs = 0;

// Expression state — everything below eases toward its *T target each frame,
// so callers only ever set targets and never animate anything by hand.
float lidTop = 0, lidTopT = 0;          // 0..1 top lid coverage
float lidAngle = 0, lidAngleT = 0;      // -1..1 (+ sad / - angry)
float lidBot = 0, lidBotT = 0;          // 0..1 happy crescent
float eyeScaleW = 1, eyeScaleWT = 1;    // size multipliers on top of faceCfg
float eyeScaleH = 1, eyeScaleHT = 1;
uint8_t eyeWinkMask = 0;                // bit0 left eye shut, bit1 right

// Mood: two slow scalars in 0..1. Energy tracks how much you have been using
// the pad, valence how well things are going (BLE up/down). They bias the
// resting pose so the face has a baseline mood, not just reactions.
float moodEnergy = 0.5f, moodValence = 0.5f;
unsigned long moodTickMs = 0;
uint8_t  typeBurstCount = 0;      // presses in the current 4s burst window
unsigned long typeBurstStart = 0;
uint8_t  moodKeyCount = 0;        // presses since the last 5s mood sample
float    typeRateEma = 0;

// Emote player — one transient animation at a time, always time-boxed
const Emote*  emoteCur = nullptr;
unsigned long emoteStart = 0;
int8_t        emoteGx = 0, emoteGy = 0;   // direction supplied by the trigger
const Emote*  emotePending = nullptr;     // queued while the face is off-screen
unsigned long emotePendingUntil = 0;
int8_t        emotePendGx = 0, emotePendGy = 0;
bool          faceBootPending = true;     // first face entry after power-on

// Micro-behaviours
unsigned long faceNextSaccade = 0;
unsigned long faceLastYawn = 0;
unsigned long faceNextSquint = 0;
bool          faceDblBlink = false;

static inline uint32_t frnd(uint32_t lo, uint32_t hi) {   // [lo, hi] ms
  return lo + (esp_random() % (hi - lo + 1));
}

// Scale a faceCfg interval by a persona percentage, never below 1s —
// frnd(lo, hi) needs hi >= lo and a degenerate range would spin.
static unsigned long personaMs(uint8_t seconds, uint8_t pct) {
  unsigned long ms = (unsigned long)seconds * 1000UL * pct / 100UL;
  return ms < 1000UL ? 1000UL : ms;
}

uint16_t faceEyeColor() {
  return faceCfg.color ? faceCfg.color : PRESET_COLORS[activePreset];
}

// Draw one eye into sprEye and push at center (cx, cy).
// The eye itself is one rounded rect; expression comes from painting lids
// back over it in the background colour, which costs 2-3 primitives and
// keeps every shape inside the sprite the frame already had to clear.
void drawEyeAt(int cx, int cy, float open, float wScale, bool isLeft) {
  sprEye.fillSprite(C_BG);
  if (eyeWinkMask & (isLeft ? 1 : 2)) open = 0.04f;   // this eye is winking
  int w = (int)(faceCfg.eyeW * wScale * eyeScaleW);
  int h = max(6, (int)(faceCfg.eyeH * open * eyeScaleH));
  w = min(w, EYE_SPR_W - 4); h = min(h, EYE_SPR_H - 4);
  int gx = (int)eyeGlanceX, gy = (int)eyeGlanceY;
  int x = (EYE_SPR_W - w) / 2 + gx;
  int y = (EYE_SPR_H - h) / 2 + gy;
  x = constrain(x, 0, EYE_SPR_W - w);
  y = constrain(y, 0, EYE_SPR_H - h);
  int r = min((int)faceCfg.rnd, h / 2);
  r = min(r, w / 2);
  sprEye.fillRoundRect(x, y, w, h, r, faceEyeColor());

  // Top lid: a flat band plus a wedge. The wedge is deeper on the outer
  // edge for sadness and on the inner edge for anger — mirrored per eye,
  // which is what makes a pair of rectangles read as a facial expression.
  int flat = (int)(lidTop * h);
  if (flat > 0) sprEye.fillRect(x, y, w, min(flat, h), C_BG);
  if (lidAngle > 0.02f || lidAngle < -0.02f) {
    int wedge = (int)(fabsf(lidAngle) * h * 0.55f);
    int top   = y + flat;
    bool deepOuter = (lidAngle > 0);                  // sad droops outward
    bool deepLeft  = isLeft ? deepOuter : !deepOuter; // outer edge flips
    if (deepLeft) sprEye.fillTriangle(x, top, x + w, top, x, top + wedge, C_BG);
    else          sprEye.fillTriangle(x, top, x + w, top, x + w, top + wedge, C_BG);
  }

  // Bottom crescent: a circle rising into the eye from below carves the
  // convex "^ ^" happy squint that a straight lid can't produce.
  if (lidBot > 0.02f) {
    int rad = w;
    int cyc = y + h + rad - (int)(lidBot * h * 0.62f);
    sprEye.fillCircle(x + w / 2, cyc, rad, C_BG);
  }
  sprEye.pushSprite(cx - EYE_SPR_W / 2, cy - EYE_SPR_H / 2);
}

void drawFaceFrame(float open, float wScale) {
  // Eye *positions* stay fixed while scale changes: the two 92px sprites sit
  // shoulder to shoulder, so moving them would overlap and leave trails.
  int half = faceCfg.gap / 2 + faceCfg.eyeW / 2;
  drawEyeAt(160 - half, 120, open, wScale, true);
  drawEyeAt(160 + half, 120, open, wScale, false);
}

// Push a pose into the eased targets. Emotes, personas and the mood engine
// all land here, so they can never disagree about what a pose means.
// withGlance is off for the resting pose: glance targets are event-driven and
// persist between frames, so a per-frame rewrite would cancel every glance
// the moment it started. Only a full override (an emote, pairing) sets them.
void applyPose(const EyePose& p, float amp, bool withGlance = false) {
  eyeOpenTarget = p.openPct / 100.0f;
  lidTopT   = (p.lidTopPct / 100.0f) * amp;
  lidAngleT = (p.lidTopAngle / 100.0f) * amp;
  lidBotT   = (p.lidBotPct / 100.0f) * amp;
  eyeScaleWT = 1.0f + ((p.wPct / 100.0f) - 1.0f) * amp;
  eyeScaleHT = 1.0f + ((p.hPct / 100.0f) - 1.0f) * amp;
  if (withGlance) {
    eyeGlanceTX = p.gx * amp;
    eyeGlanceTY = p.gy * amp;
  }
}

// ── Emotes ──────────────────────────────────────
// Stepped keyframes: the frame loop's easing does the interpolation, so a
// four-row table is enough to read as a fluid animation.
//                          open lidT angle lidB   w    h  gx gy
const EmoteKey EK_BOOT[] = {
  {   0, {   4, 90,   0,  0, 100, 100, 0, 0 }, 0 },
  { 380, {  55, 30,   0,  0, 100, 100, 0, 0 }, 0 },
  { 700, {  15, 70,   0,  0, 100, 100, 0, 0 }, 0 },
  { 950, { 100,  0,   0,  0, 106, 106, 0,-2 }, 0 },
};
const EmoteKey EK_HAPPY[] = {
  {   0, { 100,  0,   0, 25, 106, 106, 0,-6 }, 0 },
  { 220, {  70,  0,   0, 55, 100, 100, 0, 2 }, 0 },
  { 430, { 100,  0,   0, 25, 106, 106, 0,-5 }, 0 },
  { 650, {  85,  0,   0, 45, 100, 100, 0, 0 }, 0 },
};
const EmoteKey EK_SAD[] = {
  {   0, {  70, 20,  70,  0, 100,  96,-8, 4 }, 0 },
  { 450, {  62, 28,  80,  0, 100,  94, 8, 5 }, 0 },
  { 950, {  66, 24,  75,  0, 100,  95,-6, 5 }, 0 },
};
const EmoteKey EK_WINK[] = {
  {   0, { 100,  0,   0, 30, 100, 100, 0, 0 }, 2 },   // right eye shut
  { 260, { 100,  0,   0, 20, 100, 100, 0, 0 }, 0 },
};
const EmoteKey EK_EXCITED[] = {
  {   0, { 100,  0, -10,  0, 108, 108,-4,-4 }, 0 },
  { 160, { 100,  0,   0, 15, 108, 108, 4,-4 }, 0 },
  { 320, { 100,  0, -10,  0, 108, 108,-4,-4 }, 0 },
  { 480, { 100,  0,   0, 20, 104, 104, 0,-2 }, 0 },
};
const EmoteKey EK_GLANCE[] = {
  {   0, {  62,  0,   0, 10, 100, 100, 0, 0 }, 0 },   // gx/gy from the trigger
  { 190, { 100,  0,   0,  0, 100, 100, 0, 0 }, 0 },
};
const EmoteKey EK_YAWN[] = {
  {   0, {  90,  5,   0,  0, 100, 106, 0, 0 }, 0 },
  { 260, { 100,  0,   0,  0, 104, 114, 0,-3 }, 0 },
  { 620, {   6, 80,   0,  0,  96, 100, 0, 3 }, 0 },
  { 980, {  85, 12,  15,  0, 100, 100, 0, 1 }, 0 },
};
const EmoteKey EK_SQUINT[] = {              // thoughtful squint — narrows, holds
  {   0, {  62, 22,   0, 26, 103,  96, 0, 0 }, 0 },
  { 480, {  48, 32,   0, 36, 105,  92, 2, 1 }, 0 },
  {1050, {  56, 26,   0, 30, 104,  94,-2, 0 }, 0 },
  {1500, {  85,  8,   0, 10, 100, 100, 0, 0 }, 0 },
};
#define EM_DEF(tbl, dur) { tbl, sizeof(tbl)/sizeof(EmoteKey), dur }
const Emote EM_BOOT    = EM_DEF(EK_BOOT,    1400);
const Emote EM_HAPPY   = EM_DEF(EK_HAPPY,    900);
const Emote EM_SAD     = EM_DEF(EK_SAD,     1500);
const Emote EM_WINK    = EM_DEF(EK_WINK,     520);
const Emote EM_EXCITED = EM_DEF(EK_EXCITED,  800);
const Emote EM_GLANCE  = EM_DEF(EK_GLANCE,   380);
const Emote EM_YAWN    = EM_DEF(EK_YAWN,    1300);
const Emote EM_SQUINT  = EM_DEF(EK_SQUINT,  1900);
// Loved a track: a big warm squint with the bottom crescent right up.
const EmoteKey EK_LOVE[] = {
  {   0, { 100,  0,   0, 20, 110, 110, 0,-4 }, 0 },
  { 200, {  55,  0,   0, 62, 104, 100, 0, 3 }, 0 },
  { 620, {  62,  0,   0, 55, 106, 102, 0, 2 }, 0 },
  { 980, {  95,  0,   0, 22, 100, 100, 0, 0 }, 0 },
};
const Emote EM_LOVE = EM_DEF(EK_LOVE, 1300);

// Queue an emote. If the face is on screen it starts now; otherwise it waits
// (briefly) so an event that happens on the grid still gets acknowledged
// when the face comes back.
void faceEmote(const Emote* e, int8_t gx = 0, int8_t gy = 0, uint16_t waitMs = 2500) {
  if (pairingMode) return;                // pairing wide-eyes own the face
  if (currentScreen == SCR_FACE && faceStyle == 0) {
    emoteCur = e; emoteStart = millis(); emoteGx = gx; emoteGy = gy;
    emotePending = nullptr;
  } else {
    emotePending = e; emotePendGx = gx; emotePendGy = gy;
    emotePendingUntil = millis() + waitMs;
  }
}

// ALWAYS mode: acknowledge a keystroke without leaving the face.
// Targets only — the frame loop eases toward them, so this never blocks
// the key path (a delay here would stall the very keystroke it reacts to).
void faceKeyReact(int i) {
  if (emoteCur == &EM_EXCITED) return;    // mid-burst: don't stomp the wobble
  faceEmote(&EM_GLANCE, (int8_t)(((i % 4) - 1.5f) * 9.0f),   // key's column
                        (int8_t)(((i / 4) - 1.0f) * 7.0f));  // ...and row
}

void faceSlotGlance(int slot) {           // called from switchToSlot
  int8_t dir = (slot == 0) ? -14 : (slot == 1 ? 0 : 14);
  faceEmote(&EM_WINK, dir, (slot == 1) ? -6 : 0, 3000);
}

void faceEnter() {
  const Personality& P = PERSONAS[facePersona];
  beginDraw(SCR_FACE);
  eyeOpen = 0.0f; eyeOpenTarget = 1.0f;   // eyes open on arrival
  eyeGlanceX = eyeGlanceY = eyeGlanceTX = eyeGlanceTY = 0;
  lidTop = lidTopT = lidAngle = lidAngleT = lidBot = lidBotT = 0;
  eyeScaleW = eyeScaleWT = eyeScaleH = eyeScaleHT = 1.0f;
  eyeWinkMask = 0;
  faceBlinking = false; faceDblBlink = false;
  unsigned long now = millis();
  faceNextBlink  = now + frnd(personaMs(faceCfg.blinkMinS, P.blinkPct),
                              personaMs(faceCfg.blinkMaxS, P.blinkPct));
  faceNextGlance = now + frnd(personaMs(faceCfg.glanceMinS, P.glancePct),
                              personaMs(faceCfg.glanceMaxS, P.glancePct));
  faceGlanceEnd = 0; faceDartNext = 0; faceFrameMs = 0; faceNextSaccade = 0;
  faceNextSquint = now + frnd(6000, 15000);   // no squint the moment we appear
  // The very first face of the session gets a proper waking-up animation
  if (faceBootPending) { faceBootPending = false; faceEmote(&EM_BOOT); }
}

// Happy squint flash on the wake press, then caller returns to the grid
void faceWake() {
  lidBot = 0.5f; lidTop = 0;              // squint reads as pleased, not startled
  drawFaceFrame(0.32f, 1.05f);
  delay(160);
  lidBot = 0;
  screenDirty = true;                     // grid must fully repaint over us
}

// Lids close animation right before light sleep (eyes style only)
void faceSleepClose() {
  for (float o = eyeOpen; o > 0.02f; o *= 0.72f) {
    drawFaceFrame(o, 1.0f);
    delay(30);
  }
  drawFaceFrame(0.02f, 1.0f);
  delay(120);
}

// Mood drifts on a minutes-long clock so the pad has a baseline temperament
// rather than just twitching per event. Energy follows how much you type,
// valence follows whether the link is healthy.
void faceMoodTick(unsigned long now) {
  if (now - moodTickMs < 5000) return;
  moodTickMs = now;
  const Personality& P = PERSONAS[facePersona];
  typeRateEma = typeRateEma * 0.80f + moodKeyCount * 0.20f;
  moodKeyCount = 0;
  float eTarget = constrain(0.35f + typeRateEma * 0.09f + P.energyBias / 200.0f, 0.0f, 1.0f);
  float vTarget = constrain(0.5f + P.valenceBias / 200.0f, 0.0f, 1.0f);
  moodEnergy  += (eTarget - moodEnergy) * 0.20f;
  moodValence += (vTarget - moodValence) * 0.12f;   // grudges outlast moods
}

void updateFace(unsigned long now) {
  if (now - faceFrameMs < 33) return;     // ~30 fps
  faceFrameMs = now;
  const Personality& P = PERSONAS[facePersona];
  faceMoodTick(now);

  float wScale = 1.0f;
  bool allowBlink = true;
  bool calm = false;
  eyeWinkMask = 0;

  // ── L0: resting posture from persona + mood ──
  // Low energy adds lid weight, low valence tips the lids into a sad angle,
  // high valence lifts a hint of the happy crescent.
  EyePose rest = P.rest;
  rest.lidTopPct   = min(90, rest.lidTopPct + (int)((1.0f - moodEnergy) * 22.0f));
  rest.lidTopAngle = constrain(rest.lidTopAngle + (int)((0.5f - moodValence) * 60.0f), -100, 100);
  rest.lidBotPct   = min(60, rest.lidBotPct + (int)(max(0.0f, moodValence - 0.6f) * 40.0f));
  applyPose(rest, 1.0f);

  // ── L1: what the pad is actually doing right now ──
  if (pairingMode) {                      // wide + curious
    emoteCur = emotePending = nullptr;    // functional UI outranks personality
    applyPose(POSE_NEUTRAL, 1.0f, true);
    wScale = faceCfg.pairScalePct / 100.0f;
    allowBlink = false;
    eyeGlanceTX = 0; eyeGlanceTY = -3;
    faceGlanceEnd = 0;
  } else if (!bleConnected) {             // searching — eyes dart around
    if (now >= faceDartNext) {
      faceDartNext = now + frnd(400, 800);
      eyeGlanceTX = (float)((int)frnd(0, 24)) - 12.0f;
      eyeGlanceTY = (float)((int)frnd(0, 10)) - 5.0f;
    }
    eyeOpenTarget = 0.85f;
    allowBlink = false;
  } else {                                // connected, idle — calm
    calm = true;
    if (faceGlanceEnd != 0 && now >= faceGlanceEnd) {
      eyeGlanceTX = eyeGlanceTY = 0;
      faceGlanceEnd = 0;
    } else if (faceGlanceEnd == 0 && now >= faceNextGlance) {
      eyeGlanceTX = (frnd(0, 1) ? 10.0f : -10.0f);
      eyeGlanceTY = (float)((int)frnd(0, 6)) - 3.0f;
      faceGlanceEnd  = now + frnd(500, 900);
      faceNextGlance = now + frnd(personaMs(faceCfg.glanceMinS, P.glancePct),
                                  personaMs(faceCfg.glanceMaxS, P.glancePct));
    }
  }

  // ── L2: micro-behaviours — the small motions that sell "alive" ──
  if (calm && !emoteCur) {
    if (now >= faceNextSaccade) {         // eyes are never perfectly still
      faceNextSaccade = now + frnd(300, 900);
      if ((int)frnd(0, 99) < P.saccadePct) {
        // Bounded: these accumulate between glances, and an unclamped
        // random walk would slowly drag the eyes into a corner
        eyeGlanceTX = constrain(eyeGlanceTX + ((int)frnd(0, 4) - 2.0f), -14.0f, 14.0f);
        eyeGlanceTY = constrain(eyeGlanceTY + ((int)frnd(0, 4) - 2.0f), -8.0f, 8.0f);
      }
    }
    unsigned long idle = millis() - lastActivityMs;
    if (idle > 60000UL && now - faceLastYawn > 120000UL &&
        (int)frnd(0, 999) < P.yawnPct) {
      faceLastYawn = now;
      faceEmote(&EM_YAWN);
    }
    // Thoughtful squint — a "hmm" while watching you. Unlike the yawn it
    // needs no long idle; it's the face concentrating, not getting bored.
    if (now >= faceNextSquint) {
      faceNextSquint = now + frnd(9000, 22000);
      if ((int)frnd(0, 99) < P.squintPct) faceEmote(&EM_SQUINT);
    }
  }

  // ── Music reactions ──────────────────────────
  // Edge events set by the host-link handler; consumed here so the drawing
  // always happens on the loop task.
  if (mediaJustFaved) { mediaJustFaved = false; mediaNewSong = false;
                        faceEmote(&EM_LOVE); }
  if (mediaNewSong)   { mediaNewSong = false;
                        if (mediaShow) faceEmote(&EM_EXCITED); }

  // Gentle bob while a track plays — the pad quietly vibing. Applied after
  // the branches (which rewrite the glance targets every frame) but before
  // emotes, so any emote still wins outright.
  if (mediaShow && mediaPlaying && !emoteCur && (now - mediaRxMs < 30000UL)) {
    float ph = (float)(now % 2400) / 2400.0f * 6.2832f;
    eyeGlanceTY += sinf(ph) * 2.0f;
    eyeGlanceTX += sinf(ph * 0.5f) * 1.2f;
  }

  // ── L3: transient emote overrides everything above ──
  if (!emoteCur && emotePending && now < emotePendingUntil) {
    emoteCur = emotePending; emoteStart = now;
    emoteGx = emotePendGx; emoteGy = emotePendGy;
    emotePending = nullptr;
  } else if (emotePending && now >= emotePendingUntil) emotePending = nullptr;

  if (emoteCur) {
    unsigned long t = now - emoteStart;
    if (t >= emoteCur->durMs) emoteCur = nullptr;   // always time-boxed
    else {
      const EmoteKey* k = &emoteCur->keys[0];
      for (uint8_t n = 0; n < emoteCur->n; n++)
        if (t >= emoteCur->keys[n].tMs) k = &emoteCur->keys[n];
      applyPose(k->pose, P.emotePct / 100.0f, true);
      eyeGlanceTX += emoteGx; eyeGlanceTY += emoteGy;
      eyeWinkMask = k->winkMask;
      allowBlink = false;                 // the emote owns the lids
      faceGlanceEnd = 0;                  // and cancels any idle glance
    }
  }

  // ── Final: pre-sleep droop always wins, it reports a real state ──
  if (sleepTimeoutMs > 0) {
    unsigned long idle = millis() - lastActivityMs;
    if (idle + 10000 > sleepTimeoutMs) {
      unsigned long left = (sleepTimeoutMs > idle) ? sleepTimeoutMs - idle : 0;
      float droop = 0.18f + 0.82f * ((float)left / 10000.0f);
      if (droop < eyeOpenTarget) eyeOpenTarget = droop;
      allowBlink = false;
      emoteCur = nullptr;
    }
  }

  // Blink scheduling
  if (faceBlinking) {
    if (now < faceBlinkEnd) eyeOpenTarget = 0.0f;
    else {
      faceBlinking = false;
      if (faceDblBlink) {                 // ...and sometimes twice
        faceDblBlink = false;
        faceNextBlink = now + 150;        // reopen, then straight back down
      }
    }
  } else if (allowBlink && now >= faceNextBlink) {
    faceBlinking = true;
    faceBlinkEnd  = now + 110;
    faceDblBlink  = ((int)frnd(0, 99) < P.dblBlinkPct);
    faceNextBlink = now + frnd(personaMs(faceCfg.blinkMinS, P.blinkPct),
                               personaMs(faceCfg.blinkMaxS, P.blinkPct));
  }

  // Ease current values toward targets, then render
  eyeOpen    += (eyeOpenTarget - eyeOpen) * 0.38f;
  eyeGlanceX += (eyeGlanceTX - eyeGlanceX) * 0.30f;
  eyeGlanceY += (eyeGlanceTY - eyeGlanceY) * 0.30f;
  lidTop     += (lidTopT   - lidTop)   * 0.30f;
  lidAngle   += (lidAngleT - lidAngle) * 0.30f;
  lidBot     += (lidBotT   - lidBot)   * 0.30f;
  eyeScaleW  += (eyeScaleWT - eyeScaleW) * 0.30f;
  eyeScaleH  += (eyeScaleHT - eyeScaleH) * 0.30f;
  drawFaceFrame(eyeOpen, wScale);
}

// ── GIF playback (faceStyle == 1) ───────────────
AnimatedGIF gifDec;
File gifFile;
bool gifOpen = false;
int  gifOffX = 0, gifOffY = 0;
unsigned long gifNextFrame = 0;
static uint16_t gifLineBuf[320];

void* GIFOpenFile(const char* fname, int32_t* pSize) {
  gifFile = SPIFFS.open(fname);
  if (gifFile) { *pSize = gifFile.size(); return (void*)&gifFile; }
  return NULL;
}
void GIFCloseFile(void* pHandle) {
  File* f = static_cast<File*>(pHandle);
  if (f != NULL) f->close();
}
int32_t GIFReadFile(GIFFILE* pFile, uint8_t* pBuf, int32_t iLen) {
  File* f = static_cast<File*>(pFile->fHandle);
  int32_t iBytesRead = iLen;
  if ((pFile->iSize - pFile->iPos) < iLen) iBytesRead = pFile->iSize - pFile->iPos - 1;
  if (iBytesRead <= 0) return 0;
  iBytesRead = (int32_t)f->read(pBuf, iBytesRead);
  pFile->iPos = f->position();
  return iBytesRead;
}
int32_t GIFSeekFile(GIFFILE* pFile, int32_t iPosition) {
  File* f = static_cast<File*>(pFile->fHandle);
  f->seek(iPosition);
  pFile->iPos = (int32_t)f->position();
  return pFile->iPos;
}

// Line-at-a-time draw straight to the panel — no framebuffer needed
void GIFDraw(GIFDRAW* pDraw) {
  int iWidth = pDraw->iWidth;
  if (iWidth + pDraw->iX > 320) iWidth = 320 - pDraw->iX;
  int y = gifOffY + pDraw->iY + pDraw->y;
  if (y < 0 || y >= 240 || iWidth < 1) return;
  uint16_t* usPalette = pDraw->pPalette;
  uint8_t*  s = pDraw->pPixels;

  if (pDraw->ucDisposalMethod == 2) {
    for (int x = 0; x < iWidth; x++)
      if (s[x] == pDraw->ucTransparent) s[x] = pDraw->ucBackground;
    pDraw->ucHasTransparency = 0;
  }
  if (pDraw->ucHasTransparency) {
    uint8_t t = pDraw->ucTransparent;
    int x = 0;
    while (x < iWidth) {
      if (s[x] == t) { x++; continue; }
      int start = x, n = 0;
      while (x < iWidth && s[x] != t) gifLineBuf[n++] = usPalette[s[x++]];
      tft.pushImage(gifOffX + pDraw->iX + start, y, n, 1, gifLineBuf);
    }
  } else {
    for (int x = 0; x < iWidth; x++) gifLineBuf[x] = usPalette[s[x]];
    tft.pushImage(gifOffX + pDraw->iX, y, iWidth, 1, gifLineBuf);
  }
}

void faceGifStop() {
  if (gifOpen) { gifDec.close(); gifOpen = false; }
}

void faceGifTick(unsigned long now) {
  if (!gifOpen) {
    if (faceGif[0] == '\0' ||
        !gifDec.open(faceGif, GIFOpenFile, GIFCloseFile, GIFReadFile, GIFSeekFile, GIFDraw)) {
      updateFace(now);                    // fall back to procedural eyes
      return;
    }
    gifOpen = true;
    gifOffX = (320 - gifDec.getCanvasWidth()) / 2;
    gifOffY = (240 - gifDec.getCanvasHeight()) / 2;
    if (gifOffX < 0) gifOffX = 0;
    if (gifOffY < 0) gifOffY = 0;
    gifNextFrame = 0;
  }
  if (now < gifNextFrame) return;
  int frameDelay = 0;
  int rc = gifDec.playFrame(false, &frameDelay);
  gifNextFrame = now + (unsigned long)max(frameDelay, 20);
  if (rc == 0) gifDec.reset();            // loop forever
}

// ════════════════════════════════════════════════
//  SLEEP / WAKE
// ════════════════════════════════════════════════
void maybeEnterSleep() {
  if (sleepTimeoutMs == 0) return;
  if (millis() - lastActivityMs < sleepTimeoutMs) return;
  if (scanMatrixRaw() != 0) { recordActivity(); return; }

  // Face: lids close before the lights go out
  if (currentScreen == SCR_FACE) {
    faceGifStop();
    if (faceStyle == 0) faceSleepClose();
  }

  // Blank display
  ledcWrite(TFT_BL, 0);
  tft.fillScreen(0x0000);
  releaseAll();
  screenDirty = true;                     // wake must repaint fully

  NimBLEDevice::stopAdvertising();
  delay(200);

  // Clear previous wakeup config — prevents spurious 2nd+ cycle wakeup
  esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
  delay(50);

  // Any keypress pulls a read line low: drive ALL rows low, wake on any col
  for (int d = 0; d < 3; d++) {
    pinMode(DRIVE_PINS[d], OUTPUT);
    digitalWrite(DRIVE_PINS[d], LOW);
  }
  for (int j = 0; j < 4; j++)
    gpio_wakeup_enable((gpio_num_t)READ_PINS[j], GPIO_INTR_LOW_LEVEL);
  esp_sleep_enable_gpio_wakeup();

  Serial.flush();
  esp_light_sleep_start();

  // ── Woken up ────────────────────────────────
  wakeTimeMs     = millis();
  lastActivityMs = wakeTimeMs;   // reset FIRST — prevents instant re-sleep

  for (int d = 0; d < 3; d++) pinMode(DRIVE_PINS[d], INPUT);  // back to scan idle

  // Re-attach LEDC — dropped silently during light-sleep
  ledcAttach(TFT_BL, 5000, 8);
  ledcWrite(TFT_BL, backlightBrightness);
  delay(50);

  // Light sleep dropped any BLE connection; if the stack didn't get an
  // onDisconnect (we weren't connected), restart advertising ourselves
  if (!bleConnected && !NimBLEDevice::getAdvertising()->isAdvertising())
    startAdvertisingForSlot();

  // Wake-key buffer: remember which key woke us, fire it on reconnect
  wakeKeyIdx = WAKEKEY_NONE; wakeKeyPending = false;
  uint16_t raw = scanMatrixRaw();
  for (int i = 0; i < NUM_KEYS; i++) {
    if (raw & (1u << i)) {
      if (presets[activePreset].keys[i].id != A_NONE) {
        wakeKeyIdx = i; wakeKeyPending = true;
      }
      break;
    }
  }

  // Waking by keypress lands on the grid, not back on the face
  if (currentScreen == SCR_FACE) currentScreen = SCR_MAIN;

  if (wakeKeyPending && !bleConnected)
    drawReconnectHUD(presets[activePreset].keys[wakeKeyIdx].label);
  else redraw();
}

// ════════════════════════════════════════════════
//  CONFIG MODE — WiFi SoftAP + HTTP JSON API + OTA
//  BLE is torn down on entry (frees radio + RAM); exit = clean reboot.
// ════════════════════════════════════════════════
WebServer server(80);
bool configMode = false;
volatile bool otaActive = false;
volatile size_t otaProgress = 0;
void drawConfigScreen();

const char* kaTypeName(uint8_t t) {
  switch (t) {
    case KA_KEY:      return "key";
    case KA_CONSUMER: return "consumer";
    case KA_MACRO:    return "macro";
    case KA_TEXT:     return "text";
    case KA_HOST:     return "host";
    default:          return "builtin";
  }
}
// Personas travel over the API as lowercase names ("calm"), ints also accepted
const char* personaName(uint8_t p) {
  static char buf[8];
  strncpy(buf, PERSONAS[min(p, (uint8_t)(NUM_PERSONAS - 1))].name, sizeof(buf));
  buf[sizeof(buf) - 1] = 0;
  for (char* c = buf; *c; c++) *c = tolower(*c);
  return buf;
}
int personaFromName(const char* s) {
  for (uint8_t p = 0; p < NUM_PERSONAS; p++)
    if (!strcasecmp(s, PERSONAS[p].name)) return p;
  return -1;
}

uint8_t kaTypeFromName(const char* s) {
  if (!strcmp(s, "key"))      return KA_KEY;
  if (!strcmp(s, "consumer")) return KA_CONSUMER;
  if (!strcmp(s, "macro"))    return KA_MACRO;
  if (!strcmp(s, "text"))     return KA_TEXT;
  if (!strcmp(s, "host"))     return KA_HOST;
  return KA_BUILTIN;
}

void keyToJson(const KeyAction& ka, JsonObject o) {
  o["label"] = ka.label;
  o["type"]  = kaTypeName(ka.type);
  switch (ka.type) {
    case KA_KEY:      o["mod"] = ka.mod; o["key"] = ka.key; break;
    case KA_HOST:     o["mod"] = ka.mod; o["key"] = ka.key; break;  // fallback chord
    case KA_CONSUMER: o["consumer"] = ka.consumer; break;
    case KA_TEXT:     o["text"] = ka.text; break;
    case KA_MACRO: {
      JsonArray st = o["steps"].to<JsonArray>();
      for (int i = 0; i < ka.nSteps && i < MACRO_MAX; i++) {
        JsonObject s = st.add<JsonObject>();
        s["mod"] = ka.steps[i].mod;   s["key"] = ka.steps[i].key;
        s["consumer"] = ka.steps[i].consumer; s["delay"] = ka.steps[i].delayCs;
      }
      break;
    }
    default:          o["id"] = ka.id; break;   // KA_BUILTIN
  }
}

void keyFromJson(JsonObjectConst o, KeyAction& ka) {
  ka = KeyAction{};
  strncpy(ka.label, o["label"] | "", 8); ka.label[8] = '\0';
  ka.type = kaTypeFromName(o["type"] | "builtin");
  switch (ka.type) {
    case KA_KEY:      ka.mod = o["mod"] | 0; ka.key = o["key"] | 0; break;
    case KA_HOST:     ka.mod = o["mod"] | 0; ka.key = o["key"] | 0; break;  // fallback chord
    case KA_CONSUMER: ka.consumer = o["consumer"] | 0; break;
    case KA_TEXT:     strncpy(ka.text, o["text"] | "", 23); ka.text[23] = '\0'; break;
    case KA_MACRO: {
      int n = 0;
      for (JsonObjectConst s : o["steps"].as<JsonArrayConst>()) {
        if (n >= MACRO_MAX) break;
        ka.steps[n].mod = s["mod"] | 0;   ka.steps[n].key = s["key"] | 0;
        ka.steps[n].consumer = s["consumer"] | 0; ka.steps[n].delayCs = s["delay"] | 0;
        n++;
      }
      ka.nSteps = n;
      break;
    }
    default:          ka.id = o["id"] | A_NONE; break;
  }
}

void handleGetInfo() {
  JsonDocument doc;
  doc["device"]   = DEVICE_NAME;   doc["fw"]      = FW_VERSION;
  doc["api"]      = CONFIG_API;     doc["keys"]    = NUM_KEYS;
  doc["presets"]  = NUM_PRESETS;    doc["slots"]   = NUM_SLOTS;
  doc["macroMax"] = MACRO_MAX;      doc["heap"]    = ESP.getFreeHeap();
  String out; serializeJson(doc, out);
  server.send(200, "application/json", out);
}

void handleGetActions() {
  JsonDocument doc;
  JsonArray a = doc.to<JsonArray>();
  for (int i = 0; i < ACTION_LIB_SIZE; i++) {
    JsonObject o = a.add<JsonObject>();
    o["id"] = ACTION_LIB[i].id;  o["label"] = ACTION_LIB[i].label;
  }
  String out; serializeJson(doc, out);
  server.send(200, "application/json", out);
}

void handleGetConfig() {
  JsonDocument doc;
  doc["api"] = CONFIG_API; doc["fw"] = FW_VERSION; doc["device"] = DEVICE_NAME;
  JsonObject s = doc["settings"].to<JsonObject>();
  s["brightness"]   = backlightBrightness;
  s["sleepMin"]     = (sleepTimeoutMs == 0) ? 0 : (int)(sleepTimeoutMs / 60000UL);
  s["linux"]        = linuxLayout;
  s["activePreset"] = activePreset;
  // Face / screensaver — the "expression pack"
  JsonObject f = doc["face"].to<JsonObject>();
  f["mode"]  = (faceMode == 0) ? "off" : (faceMode == 1 ? "idle" : "always");
  f["style"] = (faceStyle == 0) ? "eyes" : "gif";
  f["gif"]   = faceGif;
  f["personality"] = personaName(facePersona);
  JsonObject e = f["eyes"].to<JsonObject>();
  e["color"] = faceCfg.color;   e["eyeW"] = faceCfg.eyeW;
  e["eyeH"] = faceCfg.eyeH;     e["gap"] = faceCfg.gap;
  e["round"] = faceCfg.rnd;
  e["blinkMinS"] = faceCfg.blinkMinS;   e["blinkMaxS"] = faceCfg.blinkMaxS;
  e["glanceMinS"] = faceCfg.glanceMinS; e["glanceMaxS"] = faceCfg.glanceMaxS;
  e["pairScalePct"] = faceCfg.pairScalePct;
  e["idleS"] = faceCfg.idleS;
  JsonArray pr = doc["presets"].to<JsonArray>();
  for (int p = 0; p < NUM_PRESETS; p++) {
    JsonObject po = pr.add<JsonObject>();
    po["name"] = presets[p].name;
    JsonArray ks = po["keys"].to<JsonArray>();
    for (int k = 0; k < NUM_KEYS; k++) keyToJson(presets[p].keys[k], ks.add<JsonObject>());
  }
  String out; serializeJson(doc, out);
  server.send(200, "application/json", out);
}

void handlePostConfig() {
  if (!server.hasArg("plain")) { server.send(400, "application/json", "{\"err\":\"no body\"}"); return; }
  JsonDocument doc;
  if (deserializeJson(doc, server.arg("plain"))) {
    server.send(400, "application/json", "{\"err\":\"bad json\"}"); return;
  }
  JsonObjectConst s = doc["settings"];
  if (!s.isNull()) {
    if (s["brightness"].is<int>())   backlightBrightness = constrain((int)s["brightness"], 10, 255);
    if (s["sleepMin"].is<int>())   { int sm = s["sleepMin"]; sleepTimeoutMs = (sm == 0) ? 0UL : (unsigned long)sm * 60000UL; }
    if (s["linux"].is<bool>())       linuxLayout = s["linux"];
    if (s["activePreset"].is<int>()) activePreset = constrain((int)s["activePreset"], 0, NUM_PRESETS - 1);
  }
  JsonObjectConst f = doc["face"];
  if (!f.isNull()) {
    if (f["mode"].is<const char*>()) {
      const char* m = f["mode"];
      faceMode = !strcmp(m, "always") ? 2 : (!strcmp(m, "idle") ? 1 : 0);
    }
    if (f["style"].is<const char*>())
      faceStyle = !strcmp((const char*)f["style"], "gif") ? 1 : 0;
    if (f["gif"].is<const char*>()) {
      strncpy(faceGif, f["gif"], sizeof(faceGif) - 1);
      faceGif[sizeof(faceGif) - 1] = '\0';
    }
    if (f["personality"].is<const char*>()) {
      int p = personaFromName(f["personality"]);
      if (p >= 0) facePersona = p;       // unknown names leave it alone
    } else if (f["personality"].is<int>())
      facePersona = constrain((int)f["personality"], 0, NUM_PERSONAS - 1);
    JsonObjectConst e = f["eyes"];
    if (!e.isNull()) {
      if (e["color"].is<int>())        faceCfg.color = e["color"];
      if (e["eyeW"].is<int>())         faceCfg.eyeW = e["eyeW"];
      if (e["eyeH"].is<int>())         faceCfg.eyeH = e["eyeH"];
      if (e["gap"].is<int>())          faceCfg.gap = e["gap"];
      if (e["round"].is<int>())        faceCfg.rnd = e["round"];
      if (e["blinkMinS"].is<int>())    faceCfg.blinkMinS = e["blinkMinS"];
      if (e["blinkMaxS"].is<int>())    faceCfg.blinkMaxS = e["blinkMaxS"];
      if (e["glanceMinS"].is<int>())   faceCfg.glanceMinS = e["glanceMinS"];
      if (e["glanceMaxS"].is<int>())   faceCfg.glanceMaxS = e["glanceMaxS"];
      if (e["pairScalePct"].is<int>()) faceCfg.pairScalePct = e["pairScalePct"];
      if (e["idleS"].is<int>())        faceCfg.idleS = e["idleS"];
      saveFaceCfg();                   // clamps + persists
    }
  }
  JsonArrayConst pr = doc["presets"];
  if (!pr.isNull()) {
    int p = 0;
    for (JsonObjectConst po : pr) {
      if (p >= NUM_PRESETS) break;
      if (po["name"].is<const char*>()) { strncpy(presets[p].name, po["name"], 9); presets[p].name[9] = '\0'; }
      JsonArrayConst ks = po["keys"];
      if (!ks.isNull()) {
        int k = 0;
        for (JsonObjectConst ko : ks) { if (k >= NUM_KEYS) break; keyFromJson(ko, presets[p].keys[k]); k++; }
      }
      p++;
    }
  }
  savePresets();
  backlightBrightness = constrain(backlightBrightness, 10, 255);
  ledcWrite(TFT_BL, backlightBrightness);
  saveSettings();
  server.send(200, "application/json", "{\"ok\":true}");
}

void handleExit() {
  server.send(200, "application/json", "{\"ok\":true}");
  delay(300);
  ESP.restart();
}

void handleUpdateDone() {
  bool ok = !Update.hasError();
  server.send(200, "application/json", ok ? "{\"ok\":true}" : "{\"ok\":false}");
  delay(500);
  if (ok) ESP.restart();
}

// NOTE: this callback runs inside server.handleClient() for the WHOLE upload.
// Keep it lean — no TFT redraws here. A full-screen SPI redraw per chunk
// starves the TCP receive path and stalls the upload. We draw a tiny text
// progress line only, and only a few times.
void handleUpdateUpload() {
  HTTPUpload& up = server.upload();
  if (up.status == UPLOAD_FILE_START) {
    otaActive = true; otaProgress = 0;
    tft.fillRect(0, 150, 320, 60, C_SURF);
    tft.setTextColor(C_GREEN, C_SURF); tft.setTextSize(1);
    tft.setCursor(12, 158); tft.print("OTA: flashing... do NOT power off");
    if (!Update.begin(UPDATE_SIZE_UNKNOWN)) Update.printError(Serial);
  } else if (up.status == UPLOAD_FILE_WRITE) {
    if (Update.write(up.buf, up.currentSize) != up.currentSize) Update.printError(Serial);
    size_t prevKB = otaProgress / 1024;
    otaProgress = up.totalSize;
    // Redraw the KB counter only ~every 128KB, single short text line
    if (otaProgress / 131072 != (prevKB * 1024) / 131072) {
      tft.fillRect(12, 176, 200, 12, C_SURF);
      tft.setTextColor(C_WHITE, C_SURF); tft.setTextSize(1);
      tft.setCursor(12, 176); tft.printf("%u KB", (unsigned)(otaProgress / 1024));
    }
    yield();
  } else if (up.status == UPLOAD_FILE_END) {
    if (Update.end(true)) Serial.printf("[OTA] success %u bytes\n", up.totalSize);
    else Update.printError(Serial);
    otaActive = false;
  }
}

// ── Animation (GIF) file management on SPIFFS ──
File animUpFile;

void handleAnimList() {
  JsonDocument doc;
  JsonArray a = doc["files"].to<JsonArray>();
  File root = SPIFFS.open("/");
  File f = root.openNextFile();
  while (f) {
    if (!f.isDirectory()) {
      JsonObject o = a.add<JsonObject>();
      o["name"] = String(f.path());
      o["size"] = (uint32_t)f.size();
    }
    f = root.openNextFile();
  }
  doc["used"]     = (uint32_t)SPIFFS.usedBytes();
  doc["total"]    = (uint32_t)SPIFFS.totalBytes();
  doc["selected"] = faceGif;
  String out; serializeJson(doc, out);
  server.send(200, "application/json", out);
}

void handleAnimUploadData() {
  HTTPUpload& up = server.upload();
  if (up.status == UPLOAD_FILE_START) {
    String fn = up.filename;
    int cut = max((int)fn.lastIndexOf('/'), (int)fn.lastIndexOf('\\'));
    if (cut >= 0) fn = fn.substring(cut + 1);
    if (!fn.endsWith(".gif") && !fn.endsWith(".GIF")) fn += ".gif";
    if (fn.length() > 28) fn = fn.substring(fn.length() - 28);
    animUpFile = SPIFFS.open("/" + fn, FILE_WRITE);
  } else if (up.status == UPLOAD_FILE_WRITE) {
    if (animUpFile) animUpFile.write(up.buf, up.currentSize);
    yield();
  } else if (up.status == UPLOAD_FILE_END || up.status == UPLOAD_FILE_ABORTED) {
    if (animUpFile) {
      String path = animUpFile.path();
      animUpFile.close();
      if (up.status == UPLOAD_FILE_END && faceGif[0] == '\0') {
        strncpy(faceGif, path.c_str(), sizeof(faceGif) - 1);  // auto-select first
        saveSettings();
      }
    }
  }
}
void handleAnimUploadDone() { server.send(200, "application/json", "{\"ok\":true}"); }

// body: {"name":"/x.gif"}
static bool animNameFromBody(char* out, size_t outLen) {
  if (!server.hasArg("plain")) return false;
  JsonDocument doc;
  if (deserializeJson(doc, server.arg("plain"))) return false;
  const char* n = doc["name"];
  if (!n || n[0] != '/') return false;
  strncpy(out, n, outLen - 1); out[outLen - 1] = '\0';
  return true;
}

void handleAnimSelect() {
  char name[32];
  if (!animNameFromBody(name, sizeof(name)) || !SPIFFS.exists(name)) {
    server.send(400, "application/json", "{\"err\":\"no such file\"}"); return;
  }
  strncpy(faceGif, name, sizeof(faceGif) - 1); faceGif[sizeof(faceGif) - 1] = '\0';
  faceStyle = 1;
  saveSettings();
  server.send(200, "application/json", "{\"ok\":true}");
}

void handleAnimDelete() {
  char name[32];
  if (!animNameFromBody(name, sizeof(name))) {
    server.send(400, "application/json", "{\"err\":\"bad name\"}"); return;
  }
  SPIFFS.remove(name);
  if (!strcmp(name, faceGif)) { faceGif[0] = '\0'; faceStyle = 0; saveSettings(); }
  server.send(200, "application/json", "{\"ok\":true}");
}

// Minimal in-browser config UI — works out of the box, doubles as the
// reference client for the future companion app.
static const char CONFIG_HTML[] PROGMEM = R"HTML(<!doctype html><html><head>
<meta charset=utf-8><meta name=viewport content="width=device-width,initial-scale=1">
<title>MacroPad Config</title><style>
body{font-family:system-ui,sans-serif;margin:0;background:#111;color:#eee}
header{background:#1a6ef5;padding:10px 14px;font-weight:700}
main{padding:12px;max-width:760px;margin:auto}
.tabs{display:flex;flex-wrap:wrap;gap:4px;margin:8px 0}
.tabs button{background:#222;color:#ccc;border:1px solid #444;border-radius:6px;padding:6px 10px;cursor:pointer}
.tabs button.on{background:#1a6ef5;color:#fff;border-color:#1a6ef5}
.grid{display:grid;grid-template-columns:repeat(4,1fr);gap:6px}
.cell{background:#1b1b1b;border:1px solid #333;border-radius:8px;padding:8px;min-height:54px;cursor:pointer}
.cell b{font-size:11px;color:#7cf} .cell small{color:#888;font-size:10px}
.row{margin:6px 0} label{display:block;font-size:12px;color:#aaa;margin-top:6px}
input,select,textarea{width:100%;box-sizing:border-box;background:#000;color:#eee;border:1px solid #444;border-radius:5px;padding:6px}
button.act{background:#16a34a;color:#fff;border:0;border-radius:6px;padding:9px 14px;cursor:pointer;margin-top:8px}
button.warn{background:#b4530a} .sec{border-top:1px solid #333;margin-top:16px;padding-top:12px}
#msg{position:fixed;bottom:10px;left:10px;background:#16a34a;padding:8px 12px;border-radius:6px;display:none}
</style></head><body>
<header>MacroPad Config <span id=fw></span></header><main>
<div class=tabs id=tabs></div>
<div class=grid id=grid></div>
<div id=editor style=display:none class=sec></div>
<div class=sec><button class=act onclick=save()>Save to device</button>
<button class=act onclick=toggleRaw()>Advanced JSON</button></div>
<textarea id=raw rows=10 style=display:none></textarea>
<div class=sec><h3>Face personality</h3>
<select id=persona onchange=setPersona()>
<option value=calm>Calm</option><option value=playful>Playful</option>
<option value=grumpy>Grumpy</option><option value=sleepy>Sleepy</option>
</select> <small>how the eyes carry themselves when idle</small></div>
<div class=sec><h3>Face animations (GIF)</h3>
<input type=file id=gif_file accept=".gif">
<button class=act onclick=upGif()>Upload GIF</button>
<div id=gifs></div></div>
<div class=sec><h3>Firmware update (OTA)</h3>
<input type=file id=fw_file accept=".bin">
<button class=act onclick=upload()>Upload &amp; flash</button>
<div id=prog></div></div>
<div class=sec><button class="act warn" onclick=exitCfg()>Exit config (reboot)</button></div>
</main><div id=msg></div><script>
let cfg=null,acts=[],cur=0,sel=-1;
const $=x=>document.getElementById(x);
const MODS=[["Ctrl",1],["Shift",2],["Alt",4],["Gui",8]];
async function load(){
 acts=await (await fetch('/api/actions')).json();
 cfg=await (await fetch('/api/config')).json();
 $('fw').textContent=cfg.fw; drawTabs(); drawGrid(); loadGifs();
 if(cfg.face&&cfg.face.personality)$('persona').value=cfg.face.personality;
}
async function setPersona(){
 await fetch('/api/config',{method:'POST',
  body:JSON.stringify({face:{personality:$('persona').value}})});
 if(cfg.face)cfg.face.personality=$('persona').value;
 toast('Personality saved')}
async function loadGifs(){let d=await (await fetch('/api/anim')).json();let h='';
 (d.files||[]).forEach(f=>{h+='<div class=row>'+f.name+' ('+Math.round(f.size/1024)+'KB) '
  +(d.selected==f.name?'<b style=color:#7cf>[active]</b> ':'')
  +'<button class=act onclick="selGif(\''+f.name+'\')">use</button> '
  +'<button class="act warn" onclick="delGif(\''+f.name+'\')">del</button></div>'});
 h+='<small>'+Math.round(d.used/1024)+' / '+Math.round(d.total/1024)+' KB used</small>';
 $('gifs').innerHTML=h}
async function upGif(){let f=$('gif_file').files[0];if(!f)return toast('pick a .gif',1);
 if(f.size>700000)return toast('too big (max ~700KB)',1);
 let fd=new FormData();fd.append('f',f);await fetch('/api/anim',{method:'POST',body:fd});
 toast('Uploaded');loadGifs()}
async function selGif(n){await fetch('/api/anim/select',{method:'POST',body:JSON.stringify({name:n})});toast('Active');loadGifs()}
async function delGif(n){await fetch('/api/anim/delete',{method:'POST',body:JSON.stringify({name:n})});loadGifs()}
function drawTabs(){$('tabs').innerHTML='';cfg.presets.forEach((p,i)=>{
 let b=document.createElement('button');b.textContent=p.name;if(i==cur)b.className='on';
 b.onclick=()=>{cur=i;sel=-1;$('editor').style.display='none';drawTabs();drawGrid()};$('tabs').append(b)})}
function drawGrid(){$('grid').innerHTML='';cfg.presets[cur].keys.forEach((k,i)=>{
 let c=document.createElement('div');c.className='cell';
 c.innerHTML='<b>K'+(i+1)+'</b><br>'+(k.label||'---')+'<br><small>'+k.type+'</small>';
 c.onclick=()=>edit(i);$('grid').append(c)})}
function edit(i){sel=i;let k=cfg.presets[cur].keys[i];let e=$('editor');e.style.display='block';
 let h='<b>Key '+(i+1)+'</b><label>Label</label><input id=e_label value="'+(k.label||'')+'">';
 h+='<label>Type</label><select id=e_type onchange=fields()>';
 ['builtin','key','consumer','text','macro'].forEach(t=>h+='<option '+(k.type==t?'selected':'')+'>'+t+'</option>');
 h+='</select><div id=e_fields></div>';e.innerHTML=h;fields()}
function fields(){let k=cfg.presets[cur].keys[sel],t=$('e_type').value,f=$('e_fields'),h='';
 if(t=='builtin'){h='<label>Action</label><select id=e_id>';
  acts.forEach(a=>h+='<option value='+a.id+' '+(k.id==a.id?'selected':'')+'>'+a.label+'</option>');h+='</select>';}
 else if(t=='key'){h='<label>Modifiers</label>';MODS.forEach(m=>h+='<label style=display:inline;margin-right:8px><input type=checkbox class=e_mod value='+m[1]+' '+((k.mod||0)&m[1]?'checked':'')+'>'+m[0]+'</label>');
  h+='<label>HID keycode (decimal)</label><input id=e_key type=number value='+(k.key||0)+'>';}
 else if(t=='consumer'){h='<label>Consumer code (decimal)</label><input id=e_cons type=number value='+(k.consumer||0)+'>';}
 else if(t=='text'){h='<label>Text to type</label><input id=e_text value="'+(k.text||'')+'">';}
 else if(t=='macro'){h='<small>Edit macros in Advanced JSON for now.</small>';}
 f.innerHTML=h}
function grab(){let k=cfg.presets[cur].keys[sel];k.label=$('e_label').value;k.type=$('e_type').value;
 if(k.type=='builtin')k.id=+$('e_id').value;
 else if(k.type=='key'){k.mod=0;document.querySelectorAll('.e_mod:checked').forEach(c=>k.mod|=+c.value);k.key=+$('e_key').value;}
 else if(k.type=='consumer')k.consumer=+$('e_cons').value;
 else if(k.type=='text')k.text=$('e_text').value;}
async function save(){if(sel>=0&&$('e_type'))grab();
 if($('raw').style.display!='none'){try{cfg=JSON.parse($('raw').value)}catch(e){return toast('bad json',1)}}
 let r=await fetch('/api/config',{method:'POST',body:JSON.stringify(cfg)});
 toast(r.ok?'Saved':'Error',!r.ok);drawTabs();drawGrid()}
function toggleRaw(){let r=$('raw');if(r.style.display=='none'){if(sel>=0&&$('e_type'))grab();r.value=JSON.stringify(cfg,null,1);r.style.display='block'}else r.style.display='none'}
async function upload(){let f=$('fw_file').files[0];if(!f)return toast('pick a .bin',1);
 let fd=new FormData();fd.append('f',f);$('prog').textContent='Uploading...';
 let r=await fetch('/api/update',{method:'POST',body:fd});
 $('prog').textContent=r.ok?'Flashed — rebooting':'Update failed'}
async function exitCfg(){await fetch('/api/exit',{method:'POST'});toast('Rebooting to keyboard mode')}
function toast(m,bad){let e=$('msg');e.textContent=m;e.style.background=bad?'#b00':'#16a34a';e.style.display='block';setTimeout(()=>e.style.display='none',1800)}
load();
</script></body></html>)HTML";

void handleRoot() { server.send_P(200, "text/html", CONFIG_HTML); }

void enterConfigMode() {
  releaseAll();
  // Tear BLE all the way down — frees the radio + controller RAM for WiFi,
  // which the WROOM-32 needs (the two stacks don't coexist reliably)
  NimBLEDevice::stopAdvertising();
  if (bleConnected && pServer) pServer->disconnect(bleConnHandle);
  delay(100);
  NimBLEDevice::deinit(true);
  delay(150);

  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID, AP_PASS);
  delay(200);

  server.on("/",            HTTP_GET,  handleRoot);
  server.on("/api/info",    HTTP_GET,  handleGetInfo);
  server.on("/api/actions", HTTP_GET,  handleGetActions);
  server.on("/api/config",  HTTP_GET,  handleGetConfig);
  server.on("/api/config",  HTTP_POST, handlePostConfig);
  server.on("/api/exit",    HTTP_POST, handleExit);
  server.on("/api/update",  HTTP_POST, handleUpdateDone, handleUpdateUpload);
  server.on("/api/anim",        HTTP_GET,  handleAnimList);
  server.on("/api/anim",        HTTP_POST, handleAnimUploadDone, handleAnimUploadData);
  server.on("/api/anim/select", HTTP_POST, handleAnimSelect);
  server.on("/api/anim/delete", HTTP_POST, handleAnimDelete);
  server.begin();

  configMode = true;
  drawConfigScreen();
}

void drawConfigScreen() {
  beginDraw(201);
  drawStatusBar("CONFIG MODE", C_LTBLUE);
  tft.setTextSize(2);
  tft.setTextColor(C_WHITE, C_BG);
  tft.setCursor(10, 40);  tft.print("Wi-Fi Setup");
  tft.setTextSize(1);
  tft.setTextColor(C_DIM, C_BG);
  tft.setCursor(10, 72);  tft.print("1. Join Wi-Fi network:");
  tft.setTextColor(C_CYAN, C_BG);
  tft.setCursor(24, 86);  tft.print(AP_SSID);
  tft.setTextColor(C_DIM, C_BG);
  tft.setCursor(10, 104); tft.print("   password: ");
  tft.setTextColor(C_CYAN, C_BG); tft.print(AP_PASS);
  tft.setTextColor(C_DIM, C_BG);
  tft.setCursor(10, 122); tft.print("2. Open in browser:");
  tft.setTextColor(C_AMBER, C_BG);
  tft.setCursor(24, 136); tft.print("http://" AP_IP_STR);

  if (otaActive) {
    tft.fillRect(10, 160, 300, 40, C_SURF);
    tft.drawRect(10, 160, 300, 40, C_GREEN);
    tft.setTextColor(C_GREEN, C_SURF);
    tft.setCursor(18, 168); tft.printf("Flashing... %u KB", (unsigned)(otaProgress / 1024));
    tft.setTextColor(C_RED, C_SURF);
    tft.setCursor(18, 184); tft.print("do NOT power off");
  } else {
    tft.setTextColor(C_DIM, C_BG);
    tft.setCursor(10, 210); tft.print("Hold FN (K9) to exit + reboot");
  }
}

// ════════════════════════════════════════════════
//  SETUP
// ════════════════════════════════════════════════
void setup() {
  Serial.begin(115200);

  loadState();
  matrixInit();

  SPIFFS.begin(true);            // media store for uploaded GIF animations

  // TFT init first — no backlight yet, BLE init would clobber LEDC
  tft.init();
  tft.setRotation(1);            // landscape 320×240 — use 3 if upside down
  tft.setSwapBytes(true);        // pushImage byte order for GIF playback
  sprBar.setColorDepth(16);  sprBar.createSprite(320, 26);
  sprCell.setColorDepth(16); sprCell.createSprite(CELL_W, CELL_H);
  sprEye.setColorDepth(16);  sprEye.createSprite(EYE_SPR_W, EYE_SPR_H);
  gifDec.begin(GIF_PALETTE_RGB565_BE);

  // ── NimBLE init ──────────────────────────────
  // Must happen BEFORE ledcAttach — radio init resets LEDC state
  NimBLEDevice::init(DEVICE_NAME);
  NimBLEDevice::setPower(ESP_PWR_LVL_P9);
  NimBLEDevice::setMTU(185);   // host-link writes exceed the 23-byte default

  // Just Works bonding — no PIN. BOND alone (no MITM) = Android & BlueZ happy
  NimBLEDevice::setSecurityAuth(BLE_SM_PAIR_AUTHREQ_BOND);
  NimBLEDevice::setSecurityIOCap(BLE_HS_IO_NO_INPUT_OUTPUT);
  NimBLEDevice::setSecurityInitKey(BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID);
  NimBLEDevice::setSecurityRespKey(BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID);

  pServer = NimBLEDevice::createServer();
  pServer->setCallbacks(new ServerCB());

  pHID = new NimBLEHIDDevice(pServer);
  pHID->setManufacturer("DIY");
  pHID->setPnp(0x02, 0x05AC, 0x0220, 0x0100);   // generic PnP, no driver conflicts
  pHID->setHidInfo(0x00, 0x01);
  pHID->setReportMap((uint8_t*)hidReportMap, sizeof(hidReportMap));

  pKbReport = pHID->getInputReport(1);
  pCcReport = pHID->getInputReport(2);

  pHID->startServices();

  // ── Host-link service (companion app) ────────
  // Registered before advertising so the GATT DB is stable from boot.
  // Windows notices the DB-hash change on reconnect and re-discovers;
  // if a host's cache wedges anyway, clear that slot's bond and re-pair.
  NimBLEService* pHostSvc = pServer->createService(HOSTLINK_SVC_UUID);
  pEvtChar = pHostSvc->createCharacteristic(HOSTLINK_EVT_UUID,
                                            NIMBLE_PROPERTY::NOTIFY);
  pEvtChar->setCallbacks(new EvtCB());
  NimBLECharacteristic* pCmdChar = pHostSvc->createCharacteristic(
      HOSTLINK_CMD_UUID,
      NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_ENC);
  pCmdChar->setCallbacks(new CmdCB());
  pHostSvc->start();

  // First-boot: nothing bonded anywhere → slot 1 starts in pairing mode
  if (!hostSlots[activeSlot].bonded) pairingMode = true;
  startAdvertisingForSlot();

  // ── Backlight ON only after BLE init ─────────
  ledcAttach(TFT_BL, 5000, 8);
  ledcWrite(TFT_BL, backlightBrightness);

  // Boot splash
  tft.fillScreen(C_BG);
  tft.setTextSize(3);
  tft.setTextColor(C_CYAN, C_BG);
  tft.setCursor(60, 70);  tft.print("MACROPAD v5");
  tft.setTextSize(1);
  tft.setTextColor(C_BUILD, C_BG);
  tft.setCursor(60, 110); tft.print("MULTI-HOST  ·  3 device slots");
  tft.setTextColor(C_DIM, C_BG);
  tft.setCursor(60, 130); tft.print("NimBLE  ·  12-key matrix  ·  ST7789");
  tft.setCursor(60, 145); tft.print("Hold FN (K9) 1s = system menu");
  delay(900);

  lastActivityMs = millis();
  drawMain();
}

// ════════════════════════════════════════════════
//  LOOP
// ════════════════════════════════════════════════
void loop() {
  unsigned long now = millis();

  // ── Config Mode: WiFi/HTTP only, BLE is torn down ──
  // Hold FN (K9) ~1.5s to leave and reboot back into keyboard mode.
  if (configMode) {
    server.handleClient();
    static unsigned long fnDown = 0;
    bool fn = (scanMatrixRaw() >> KEY_FN) & 1;
    if (fn) { if (fnDown == 0) fnDown = now; else if (now - fnDown > 1500) ESP.restart(); }
    else fnDown = 0;
    delay(2);
    return;
  }

  // Companion-app link: hello + queued commands (loop owns the TFT)
  hostLinkTick();

  // BLE events from callback context → UI feedback (loop owns the TFT)
  if (pendingBleEvent != EVT_NONE) {
    uint8_t evt = pendingBleEvent;
    pendingBleEvent = EVT_NONE;
    if (evt == EVT_PAIRED) {
      char msg[24];
      snprintf(msg, sizeof(msg), "PAIRED SLOT %d", activeSlot + 1);
      redraw();
      showToast(msg, C_GREEN, 1500);
    } else if (evt == EVT_WRONG_HOST) {
      redraw();
      showToast("WRONG DEVICE", C_RED, 1200);
    }
  }

  // Connection state change → redraw (+ fire buffered wake-key)
  if (bleConnected != lastBleConn) {
    lastBleConn = bleConnected;
    // The face reacts to the link coming and going — queued, so it plays
    // whether the event lands on the grid or with the eyes already up
    if (bleConnected) { faceEmote(&EM_HAPPY); moodValence = min(1.0f, moodValence + 0.20f); }
    else              { faceEmote(&EM_SAD);   moodValence = max(0.0f, moodValence - 0.25f); }
    if (bleConnected) {
      if (wakeKeyPending && wakeKeyIdx != WAKEKEY_NONE) {
        wakeKeyPending = false;
        delay(150);
        lastFlashKey = wakeKeyIdx; flashUntil = now + FLASH_MS;
        drawMain();
        fireKeyAction(presets[activePreset].keys[wakeKeyIdx], wakeKeyIdx);
        wakeKeyIdx = WAKEKEY_NONE;
      } else redraw();
    } else {
      if (wakeKeyPending && wakeKeyIdx != WAKEKEY_NONE)
        drawReconnectHUD(presets[activePreset].keys[wakeKeyIdx].label);
      else redraw();
    }
  }

  // Pairing-state change (from BLE callbacks) → status bar refresh
  if (pairingMode != lastPairingUi) {
    lastPairingUi = pairingMode;
    if (currentScreen == SCR_MAIN || currentScreen == SCR_DEVICES) redraw();
  }

  // Reconnect HUD animation + timeout
  if (!bleConnected && wakeKeyPending && wakeKeyIdx != WAKEKEY_NONE) {
    static unsigned long lastHud = 0;
    if (now - lastHud > 400) {
      lastHud = now;
      if (now - wakeTimeMs < RECONNECT_TIMEOUT_MS)
        drawReconnectHUD(presets[activePreset].keys[wakeKeyIdx].label);
      else { wakeKeyPending = false; wakeKeyIdx = WAKEKEY_NONE; redraw(); }
    }
  }

  // Toast expiry
  if (toastUntil != 0 && now >= toastUntil) { toastUntil = 0; redraw(); }

  // Key flash expiry
  if (lastFlashKey >= 0 && now >= flashUntil) {
    int k = lastFlashKey; lastFlashKey = -1;
    if (currentScreen == SCR_MAIN) {
      KeyAction& ka = presets[activePreset].keys[k];
      // kaEmpty, not id==A_NONE: rich key types (host/key/text/…) all have
      // id 0 — the old test blanked their cells after every flash
      if (kaEmpty(ka)) drawCellEmpty(k);
      else drawCell(k, ka.label, nullptr, PRESET_COLORS[activePreset], false);
    }
  }

  // ── Matrix scan with per-key debounce + tap/hold events ──
  // Fixed cadence: scanning every loop pass made keys feel sticky on the
  // bench; 5ms polling + 25ms debounce filters contact bounce cleanly
  static unsigned long lastScanMs = 0;
  if (now - lastScanMs < KEY_SCAN_INTERVAL_MS) { maybeEnterSleep(); delay(1); return; }
  lastScanMs = now;
  uint16_t raw = scanMatrixRaw();
  static uint16_t lastRawDbg = 0;
  if (raw != lastRawDbg) {           // bench debug: print electrical position
    for (int d = 0; d < 3; d++) for (int j = 0; j < 4; j++) {
      uint16_t m = 1u << (d * 4 + j);
      if ((raw & m) && !(lastRawDbg & m))
        Serial.printf("[MX] DOWN row=P%d col=P%d  -> key idx %d\n",
                      DRIVE_PINS[d], READ_PINS[j], d * 4 + j);
    }
    lastRawDbg = raw;
  }
  for (int i = 0; i < NUM_KEYS; i++) {
    bool r = (raw >> i) & 1;
    if (r != keyRaw[i]) { keyRaw[i] = r; keyChangeMs[i] = now; }
    if ((now - keyChangeMs[i]) >= KEY_DEBOUNCE_MS && r != keyStable[i]) {
      keyStable[i] = r;
      if (r) {
        keyDownMs[i] = now;
        keyHoldFired[i] = false;
        recordActivity();
        // Typing burst → the face gets visibly excited and stays perkier
        if (moodKeyCount < 255) moodKeyCount++;
        if (now - typeBurstStart > 4000) { typeBurstStart = now; typeBurstCount = 0; }
        if (++typeBurstCount == 5) {
          faceEmote(&EM_EXCITED);
          moodEnergy = min(1.0f, moodEnergy + 0.15f);
        }
        // IDLE: the face is a screensaver — the first press only wakes it
        // (consumed, never typed) and hands the grid back.
        if (currentScreen == SCR_FACE && faceMode != 2) {
          keyFiredOnDown[i] = true;
          faceGifStop();
          if (faceStyle == 0) faceWake();       // happy squint flash
          else screenDirty = true;
          currentScreen = SCR_MAIN;
          drawMain();
          Serial.printf("[FACE] wake by K%d (mode=%d)\n", i + 1, faceMode);
        }
        // ALWAYS mode and the grid share one path: the face is just the
        // main screen wearing a different skin, so keys keep full tap/hold
        // semantics and ALWAYS never bounces through the grid to type.
        // Instant fire on press for keys with no hold action on this
        // screen — firing on release made keys feel laggy ("hanging")
        else if (holdThresholdFor(i) == 0) {
          keyFiredOnDown[i] = true;
          onKeyTap(i);
        } else keyFiredOnDown[i] = false;
      } else {
        onKeyUp(i, now - keyDownMs[i]);
      }
    }
    // In-flight hold detection (FN menu, slot pairing, bond wipe)
    if (keyStable[i] && !keyHoldFired[i]) {
      unsigned long th = holdThresholdFor(i);
      if (th != 0 && (now - keyDownMs[i]) >= th) {
        keyHoldFired[i] = true;
        onKeyHold(i);
      }
    }
  }

  // ── Face: idle entry + animation tick ──
  if (faceMode != 0 && currentScreen == SCR_MAIN &&
      toastUntil == 0 && !wakeKeyPending && lastFlashKey < 0) {
    unsigned long th = (faceMode == 2) ? FACE_ALWAYS_MS
                                       : (unsigned long)faceCfg.idleS * 1000UL;
    // MUST be a fresh millis(): recordActivity() may have run later in this
    // very pass than the loop-top `now`, and unsigned (now - lastActivityMs)
    // would wrap to ~4e9 → face re-entered on the same pass as the wake press
    if (millis() - lastActivityMs > th) {
      Serial.println("[FACE] enter");
      currentScreen = SCR_FACE; faceEnter();
    }
  }
  if (currentScreen == SCR_FACE) {
    if (faceStyle == 1) faceGifTick(now);
    else {
      updateFace(now);
      mediaStripTick(now);      // now-playing title + timeline under the eyes
    }
  }

  maybeEnterSleep();
  delay(3);
}
