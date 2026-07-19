/*********************************************************************
 * ESP32 BLE HID Macro Pad — v5 MULTIHOST (NimBLE)
 *
 * Hardware : NodeMCU ESP32-S V1.1 (WROOM-32, no PSRAM)
 *            ST7789 240×320 IPS, landscape mount (320×240)
 *            12× MX switches, 4×3 electrical matrix, 1N4148 diodes
 *            (AS5600 encoder puck is a separate ESP-NOW device — NOT
 *             handled by this firmware yet)
 *
 * Wiring (P-label = GPIO):
 *   TFT  : SCK=P18  MOSI=P23  CS=P5  DC=P21  RST=P22  BL=P19 (LEDC PWM)
 *   Matrix rows (OUTPUT, driven LOW one at a time): P25 P26 P27 P32
 *   Matrix cols (INPUT_PULLUP)                    : P33 P13 P14
 *   Diodes: cathode faces the ROW line (current col → switch → row)
 *
 * Key grid (logical, landscape, 4 wide × 3 tall):
 *   K1  K2  K3  K4        idx 0..3   = row line r, col line c
 *   K5  K6  K7  K8        idx 4..7     maps as  idx = c*4 + r
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

#include <NimBLEDevice.h>
#include <NimBLEHIDDevice.h>
#include <TFT_eSPI.h>
#include <SPI.h>
#include <Preferences.h>
#include <driver/gpio.h>

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

const uint16_t PRESET_COLORS[8] = {
  C_CYAN, C_GREEN, C_AMBER, C_MAGENTA,
  C_ORANGE, C_LTBLUE, C_PINK, C_YELLOW,
};

// ════════════════════════════════════════════════
//  PINS  (NodeMCU ESP32-S V1.1 — see header)
// ════════════════════════════════════════════════
#define TFT_BL 19
const uint8_t ROW_PINS[4] = {25, 26, 27, 32};  // driven LOW one at a time
const uint8_t COL_PINS[3] = {33, 13, 14};      // INPUT_PULLUP readers

#define NUM_KEYS   12
#define KEY_FN      8          // bottom-left key (K9)

// ════════════════════════════════════════════════
//  TUNING
// ════════════════════════════════════════════════
int           backlightBrightness = 180;
unsigned long sleepTimeoutMs      = 300000UL;   // 0 = no sleep
bool          linuxLayout         = false;      // false=Windows, true=Linux

const uint16_t KEY_DEBOUNCE_MS   = 12;
const uint16_t KEY_MIN_PRESS_MS  = 30;
const unsigned long FN_MENU_MS   = 1000;   // FN hold → SYSTEM menu
const unsigned long PAIR_HOLD_MS = 1500;   // slot key hold → pairing mode
const unsigned long CLRALL_HOLD_MS = 2000; // devices screen K11 hold → wipe bonds
const unsigned long FLASH_MS     = 500;
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

void startAdvertisingForSlot();   // fwd
void saveSlots();                 // fwd

int slotForAddress(const uint8_t addr[6]) {
  for (int i = 0; i < NUM_SLOTS; i++)
    if (hostSlots[i].bonded && memcmp(hostSlots[i].addr, addr, 6) == 0) return i;
  return -1;
}

class ServerCB : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer* s, NimBLEConnInfo& info) override {
    bleConnected  = true;
    bleConnHandle = info.getConnHandle();
    // Request faster connection interval — critical for Android/Linux responsiveness
    s->updateConnParams(info.getConnHandle(), 6, 12, 0, 400);
    NimBLEDevice::stopAdvertising();
  }
  void onAuthenticationComplete(NimBLEConnInfo& info) override {
    if (!info.isEncrypted()) { pServer->disconnect(info.getConnHandle()); return; }
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
      // Whitelist filtering should make this impossible, but enforce in
      // software too: only the active slot's bonded host may stay
      if (knownSlot != activeSlot) {
        pendingBleEvent = EVT_WRONG_HOST;
        pServer->disconnect(info.getConnHandle());
      }
    }
  }
  void onDisconnect(NimBLEServer* s, NimBLEConnInfo& info, int reason) override {
    bleConnected = false;
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

// Advertise for the ACTIVE slot:
//   bonded slot → accept-list filtered, only that host can reconnect
//   empty slot / pairing mode → open advertising (discoverable)
void startAdvertisingForSlot() {
  NimBLEAdvertising* pAdv = NimBLEDevice::getAdvertising();
  if (pAdv->isAdvertising()) pAdv->stop();

  while (NimBLEDevice::getWhiteListCount() > 0)
    NimBLEDevice::whiteListRemove(NimBLEDevice::getWhiteListAddress(0));

  bool filtered = hostSlots[activeSlot].bonded && !pairingMode;
  if (filtered) {
    NimBLEDevice::whiteListAdd(
      NimBLEAddress(hostSlots[activeSlot].addr, hostSlots[activeSlot].type));
    pAdv->setScanFilter(false, true);   // scan-req: anyone, connect: whitelist
  } else {
    pAdv->setScanFilter(false, false);  // open — pairing / first use
  }
  configureAdvertising();
  pAdv->start();
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
#define PRESETS_VER  1        // bump to invalidate stored NVS presets

struct KeyAction { char label[9]; int id; };
struct Preset    { char name[10]; KeyAction keys[NUM_KEYS]; };

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
  SCR_BUILD_PRESET, SCR_BUILD_KEYS, SCR_BUILD_ACTION
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

// Landscape cell grid: 4 cols × 3 rows below the 26px status bar
static inline int cellX(int i){ return 1 + (i % 4) * 80; }
static inline int cellY(int i){ return 28 + (i / 4) * 70; }
#define CELL_W 78
#define CELL_H 68

// ════════════════════════════════════════════════
//  FORWARD DECLARATIONS
// ════════════════════════════════════════════════
void redraw();
void drawMain(); void drawSysMenu(); void drawPresetPicker();
void drawSettings(); void drawEditor(); void drawDevices();
void drawBuildPreset(); void drawBuildKeys(); void drawBuildAction();
void drawReconnectHUD(const char* keyName);
void fireAction(int id);

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
}

void saveSettings() {
  prefs.putInt("bright", backlightBrightness);
  prefs.putInt("sleepMin", (sleepTimeoutMs == 0) ? 0 : (int)(sleepTimeoutMs / 60000UL));
  prefs.putBool("linux", linuxLayout);
  prefs.putInt("preset", activePreset);
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
//  Rows driven LOW one at a time (inactive rows hi-Z, tolerant of
//  breadboard wiring without diodes); cols read with pullups.
//  Logical key idx = colLine*4 + rowLine  → 4-wide × 3-tall UI grid.
// ════════════════════════════════════════════════
void matrixInit() {
  for (int r = 0; r < 4; r++) pinMode(ROW_PINS[r], INPUT);
  for (int c = 0; c < 3; c++) pinMode(COL_PINS[c], INPUT_PULLUP);
}

uint16_t scanMatrixRaw() {
  uint16_t bits = 0;
  for (int r = 0; r < 4; r++) {
    pinMode(ROW_PINS[r], OUTPUT);
    digitalWrite(ROW_PINS[r], LOW);
    delayMicroseconds(25);
    for (int c = 0; c < 3; c++)
      if (digitalRead(COL_PINS[c]) == LOW) bits |= 1u << (c * 4 + r);
    pinMode(ROW_PINS[r], INPUT);
  }
  return bits;
}

// Debounced per-key state
bool keyStable[NUM_KEYS]  = {};
bool keyRaw[NUM_KEYS]     = {};
unsigned long keyChangeMs[NUM_KEYS] = {};
unsigned long keyDownMs[NUM_KEYS]   = {};
bool keyHoldFired[NUM_KEYS] = {};

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
}

// ════════════════════════════════════════════════
//  SCREENS
// ════════════════════════════════════════════════
void drawMain() {
  tft.fillScreen(C_BG);
  uint16_t accent = PRESET_COLORS[activePreset];
  drawStatusBar(presets[activePreset].name, accent);
  for (int i = 0; i < NUM_KEYS; i++) {
    KeyAction& ka = presets[activePreset].keys[i];
    bool flash = (i == lastFlashKey);
    if (ka.id == A_NONE) drawCellEmpty(i);
    else drawCell(i, ka.label, nullptr, accent, flash);
  }
}

void drawSysMenu() {
  tft.fillScreen(C_BG);
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
  tft.fillScreen(C_BG);
  drawStatusBar("PRESET", C_YELLOW);
  for (int i = 0; i < NUM_PRESETS; i++)
    drawCell(i, presets[i].name, nullptr, PRESET_COLORS[i], i == activePreset);
  drawCellEmpty(8); drawCellEmpty(9); drawCellEmpty(10);
  drawCell(11, "BACK", nullptr, C_DIM, false);
}

void drawSettings() {
  tft.fillScreen(C_BG);
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
  for (int i = 4; i < 11; i++) drawCellEmpty(i);
  drawCell(11, "SAVE", "+ exit", C_WHITE, false);
}

void drawEditor() {
  bool isBright = (currentScreen == SCR_EDIT_BRIGHT);
  tft.fillScreen(C_BG);
  drawStatusBar(isBright ? "BRIGHTNESS" : "SLEEP", isBright ? C_CYAN : C_AMBER);

  char v[12];
  if (isBright) snprintf(v, sizeof(v), "%d", backlightBrightness);
  else {
    int sm = (sleepTimeoutMs == 0) ? 0 : (int)(sleepTimeoutMs / 60000UL);
    if (sm == 0) snprintf(v, sizeof(v), "OFF");
    else         snprintf(v, sizeof(v), "%d min", sm);
  }
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
  tft.fillScreen(C_BG);
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
  tft.fillScreen(C_BG);
  drawStatusBar("BUILD: PICK", C_BUILD);
  for (int i = 0; i < NUM_PRESETS; i++)
    drawCell(i, presets[i].name, nullptr, PRESET_COLORS[i], false);
  drawCellEmpty(8); drawCellEmpty(9); drawCellEmpty(10);
  drawCell(11, "CANCEL", nullptr, C_DIM, false);
}

void drawBuildKeys() {
  tft.fillScreen(C_BG);
  char t[16];
  snprintf(t, sizeof(t), "ED:%s", presets[buildPreset].name);
  drawStatusBar(t, C_BUILD);
  for (int i = 0; i < NUM_KEYS; i++) {
    KeyAction& ka = presets[buildPreset].keys[i];
    if (i == KEY_FN)
      drawCell(i, ka.id == A_NONE ? "---" : ka.label, "tap=DONE", C_BUILD, false);
    else
      drawCell(i, ka.id == A_NONE ? "---" : ka.label, nullptr, C_BUILD, false);
  }
}

void drawBuildAction() {
  tft.fillScreen(C_BG);
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
  tft.fillScreen(C_BG);
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
  }
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
//  INPUT — per-screen tap / hold handlers
// ════════════════════════════════════════════════
unsigned long holdThresholdFor(int i) {   // 0 = no hold action for this key
  switch (currentScreen) {
    case SCR_MAIN:       return (i == KEY_FN) ? FN_MENU_MS : 0;
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
      if (ka.id == A_NONE) return;
      lastFlashKey = i; flashUntil = millis() + FLASH_MS;
      drawCell(i, ka.label, nullptr, PRESET_COLORS[activePreset], true);
      fireAction(ka.id);
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
        saveSettings();
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
          strncpy(presets[buildPreset].keys[buildSlot].label, ACTION_LIB[idx].label, 8);
          presets[buildPreset].keys[buildSlot].label[8] = '\0';
          presets[buildPreset].keys[buildSlot].id = ACTION_LIB[idx].id;
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
  if (keyHoldFired[i]) return;                 // hold action already consumed it
  if (heldMs < KEY_MIN_PRESS_MS) return;
  onKeyTap(i);
}

// ════════════════════════════════════════════════
//  SLEEP / WAKE
// ════════════════════════════════════════════════
void maybeEnterSleep() {
  if (sleepTimeoutMs == 0) return;
  if (millis() - lastActivityMs < sleepTimeoutMs) return;
  if (scanMatrixRaw() != 0) { recordActivity(); return; }

  // Blank display
  ledcWrite(TFT_BL, 0);
  tft.fillScreen(0x0000);
  releaseAll();

  NimBLEDevice::stopAdvertising();
  delay(200);

  // Clear previous wakeup config — prevents spurious 2nd+ cycle wakeup
  esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
  delay(50);

  // Any keypress pulls a col low: drive ALL rows low, wake on any col
  for (int r = 0; r < 4; r++) {
    pinMode(ROW_PINS[r], OUTPUT);
    digitalWrite(ROW_PINS[r], LOW);
  }
  for (int c = 0; c < 3; c++)
    gpio_wakeup_enable((gpio_num_t)COL_PINS[c], GPIO_INTR_LOW_LEVEL);
  esp_sleep_enable_gpio_wakeup();

  Serial.flush();
  esp_light_sleep_start();

  // ── Woken up ────────────────────────────────
  wakeTimeMs     = millis();
  lastActivityMs = wakeTimeMs;   // reset FIRST — prevents instant re-sleep

  for (int r = 0; r < 4; r++) pinMode(ROW_PINS[r], INPUT);  // back to scan idle

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

  if (wakeKeyPending && !bleConnected)
    drawReconnectHUD(presets[activePreset].keys[wakeKeyIdx].label);
  else redraw();
}

// ════════════════════════════════════════════════
//  SETUP
// ════════════════════════════════════════════════
void setup() {
  Serial.begin(115200);

  loadState();
  matrixInit();

  // TFT init first — no backlight yet, BLE init would clobber LEDC
  tft.init();
  tft.setRotation(1);            // landscape 320×240 — use 3 if upside down
  sprBar.setColorDepth(16);  sprBar.createSprite(320, 26);
  sprCell.setColorDepth(16); sprCell.createSprite(CELL_W, CELL_H);

  // ── NimBLE init ──────────────────────────────
  // Must happen BEFORE ledcAttach — radio init resets LEDC state
  NimBLEDevice::init(DEVICE_NAME);
  NimBLEDevice::setPower(ESP_PWR_LVL_P9);

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
    if (bleConnected) {
      if (wakeKeyPending && wakeKeyIdx != WAKEKEY_NONE) {
        wakeKeyPending = false;
        delay(150);
        lastFlashKey = wakeKeyIdx; flashUntil = now + FLASH_MS;
        drawMain();
        fireAction(presets[activePreset].keys[wakeKeyIdx].id);
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
      if (ka.id == A_NONE) drawCellEmpty(k);
      else drawCell(k, ka.label, nullptr, PRESET_COLORS[activePreset], false);
    }
  }

  // ── Matrix scan with per-key debounce + tap/hold events ──
  uint16_t raw = scanMatrixRaw();
  for (int i = 0; i < NUM_KEYS; i++) {
    bool r = (raw >> i) & 1;
    if (r != keyRaw[i]) { keyRaw[i] = r; keyChangeMs[i] = now; }
    if ((now - keyChangeMs[i]) >= KEY_DEBOUNCE_MS && r != keyStable[i]) {
      keyStable[i] = r;
      if (r) {
        keyDownMs[i] = now;
        keyHoldFired[i] = false;
        recordActivity();
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

  maybeEnterSleep();
  delay(3);
}
