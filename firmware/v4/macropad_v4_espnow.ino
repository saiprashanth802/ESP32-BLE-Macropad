/*********************************************************************
 * ESP32 BLE HID Macro Pad — v4 + Optional ESP-NOW Remote Encoder
 *
 * Hardware:
 *   Encoder : CLK=2, DT=4, SW=15
 *   TFT     : CS=5, DC=17, RST=16, BL=12  (128×160 ST7735)
 *   Buttons : GPIO 14,13,26,25,22,21,35,19
 *
 * ESP-NOW (optional):
 *   Enable in Settings menu → ESP-NOW → ON
 *   Saved to NVS — persists across reboots
 *   Only inits WiFi radio when enabled — no memory conflict with BLE
 *
 * Libraries: NimBLE-Arduino, TFT_eSPI, Preferences
 *********************************************************************/

#include <NimBLEDevice.h>
#include <NimBLEServer.h>
#include <NimBLEHIDDevice.h>
#include <TFT_eSPI.h>
#include <SPI.h>
#include <Preferences.h>
#include <WiFi.h>
#include <esp_now.h>
#include "esp_sleep.h"
#include "driver/gpio.h"

// ════════════════════════════════════════════════
//  TFT — declared early so all functions can use spr
// ════════════════════════════════════════════════
TFT_eSPI    tft = TFT_eSPI();
TFT_eSprite spr = TFT_eSprite(&tft);

// Forward declaration
void redraw();

// ════════════════════════════════════════════════
//  PALETTE
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
#define C_GRID     0x0861
#define C_BUILD    0x9F1F

const uint16_t PRESET_COL[8] = {
  C_CYAN, C_GREEN, C_AMBER, C_MAGENTA,
  C_ORANGE, C_LTBLUE, C_PINK, C_YELLOW,
};

// ════════════════════════════════════════════════
//  PINS
// ════════════════════════════════════════════════
#define TFT_BL  12
#define ENC_CLK  2
#define ENC_DT   4
#define ENC_SW  15
const uint8_t BTN_PINS[8] = {14,13,26,25,22,21,35,19};

// ════════════════════════════════════════════════
//  TUNING
// ════════════════════════════════════════════════
int           backlightBrightness = 180;
unsigned long sleepTimeoutMs      = 300000UL;
int           encoderSensitivity  = 3;
bool          linuxLayout         = false;
bool          espNowEnabled       = false;
bool          espNowRunning       = false;

const uint16_t  BTN_DEBOUNCE_MS  = 15;
const uint16_t  BTN_MIN_PRESS_MS = 40;
const uint8_t   ENC_DEBOUNCE_MS  = 10;
const unsigned long FLASH_MS     = 600;
const unsigned long ALT_HOLD_MS  = 1000;
const int ENC_DIR                = -1;
#define WAKEKEY_NONE  -1
#define RECONNECT_TIMEOUT_MS  8000UL

// ════════════════════════════════════════════════
//  NVS
// ════════════════════════════════════════════════
Preferences prefs;

void loadSettings() {
  prefs.begin("macropad", true);
  espNowEnabled       = prefs.getBool("espnow", false);
  backlightBrightness = prefs.getInt("bright", 180);
  sleepTimeoutMs      = (unsigned long)prefs.getInt("sleep", 5) * 60000UL;
  encoderSensitivity  = prefs.getInt("sens", 3);
  linuxLayout         = prefs.getBool("linux", false);
  prefs.end();
  Serial.print("Loaded — espnow:"); Serial.print(espNowEnabled);
  Serial.print(" bright:"); Serial.print(backlightBrightness);
  Serial.print(" linux:"); Serial.println(linuxLayout);
}

void saveSettings() {
  prefs.begin("macropad", false);
  prefs.putBool("espnow",  espNowEnabled);
  prefs.putInt("bright",   backlightBrightness);
  prefs.putInt("sleep",    (sleepTimeoutMs == 0) ? 0 : (int)(sleepTimeoutMs / 60000UL));
  prefs.putInt("sens",     encoderSensitivity);
  prefs.putBool("linux",   linuxLayout);
  prefs.end();
  Serial.println("Settings saved to NVS");
}

// ════════════════════════════════════════════════
//  HID KEYCODES
// ════════════════════════════════════════════════
#define MOD_LCTRL   0x01
#define MOD_LSHIFT  0x02
#define MOD_LALT    0x04
#define MOD_LGUI    0x08

#define KEY_A         0x04
#define KEY_C         0x06
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
#define KEY_V         0x19
#define KEY_W         0x1A
#define KEY_X         0x1B
#define KEY_Y         0x1C
#define KEY_Z         0x1D
#define KEY_1         0x1E
#define KEY_2         0x1F
#define KEY_3         0x20
#define KEY_7         0x24
#define KEY_BACKTICK  0x35
#define KEY_MINUS     0x2D
#define KEY_EQUAL     0x2E
#define KEY_5         0x22
#define KEY_TAB       0x2B
#define KEY_F4        0x3D
#define KEY_F11       0x44
#define KEY_PRTSC     0x46
#define KEY_RIGHT_ARR 0x4F
#define KEY_LEFT_ARR  0x50
#define KEY_DOWN_ARR  0x51
#define KEY_UP_ARR    0x52
#define KEY_D         0x07
#define KEY_0         0x27

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
  0x05,0x01, 0x09,0x06, 0xA1,0x01,
  0x85,0x01,
  0x05,0x07, 0x19,0xE0, 0x29,0xE7,
  0x15,0x00, 0x25,0x01, 0x75,0x01, 0x95,0x08,
  0x81,0x02,
  0x95,0x01, 0x75,0x08, 0x81,0x01,
  0x95,0x06, 0x75,0x08,
  0x15,0x00, 0x25,0x65,
  0x05,0x07, 0x19,0x00, 0x29,0x65,
  0x81,0x00,
  0xC0,
  0x05,0x0C, 0x09,0x01, 0xA1,0x01,
  0x85,0x02,
  0x15,0x00, 0x26,0xFF,0x03,
  0x19,0x00, 0x2A,0xFF,0x03,
  0x75,0x10, 0x95,0x01,
  0x81,0x00,
  0xC0,
};

// ════════════════════════════════════════════════
//  NIMBLE BLE GLOBALS
// ════════════════════════════════════════════════
NimBLEServer*         pServer   = nullptr;
NimBLEHIDDevice*      pHID      = nullptr;
NimBLECharacteristic* pKbReport = nullptr;
NimBLECharacteristic* pCcReport = nullptr;
bool bleConnected = false;

class ServerCB : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer* s, NimBLEConnInfo& info) override {
    bleConnected = true;
    s->updateConnParams(info.getConnHandle(), 6, 12, 0, 400);
    NimBLEDevice::stopAdvertising();
  }
  void onDisconnect(NimBLEServer* s, NimBLEConnInfo& info, int reason) override {
    bleConnected = false;
    delay(200);
    NimBLEAdvertising* pAdv = NimBLEDevice::getAdvertising();
    NimBLEAdvertisementData advData;
    advData.setFlags(0x06);
    advData.setAppearance(0x03C1);
    if(pHID) advData.addServiceUUID(pHID->getHidService()->getUUID());
    pAdv->setAdvertisementData(advData);
    NimBLEAdvertisementData scanData;
    scanData.setName("ESP32 MacroPad");
    pAdv->setScanResponseData(scanData);
    NimBLEDevice::startAdvertising();
  }
};

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

uint8_t heldMod = 0;
void holdMod(uint8_t mod) {
  if (!bleConnected || !pKbReport) return;
  heldMod = mod;
  uint8_t report[8] = {mod, 0, 0, 0, 0, 0, 0, 0};
  pKbReport->setValue(report, 8);
  pKbReport->notify();
}

void sendKeyWithHeld(uint8_t extraMod, uint8_t key) {
  if (!bleConnected || !pKbReport) return;
  uint8_t report[8] = {(uint8_t)(heldMod | extraMod), 0, key, 0, 0, 0, 0, 0};
  pKbReport->setValue(report, 8);
  pKbReport->notify();
  delay(40);
  uint8_t held[8] = {heldMod, 0, 0, 0, 0, 0, 0, 0};
  pKbReport->setValue(held, 8);
  pKbReport->notify();
  delay(20);
}

void releaseAll() {
  heldMod = 0;
  if (!pKbReport) return;
  uint8_t rel[8] = {};
  pKbReport->setValue(rel, 8);
  pKbReport->notify();
  uint8_t rel2[2] = {};
  if (pCcReport) { pCcReport->setValue(rel2, 2); pCcReport->notify(); }
}

// ════════════════════════════════════════════════
//  ESP-NOW
// ════════════════════════════════════════════════
// Paste your media remote MAC address here
uint8_t SENDER_MAC[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

#define MSG_VOLUME      0
#define MSG_PLAYPAUSE   1
#define MSG_NEXT        2
#define MSG_PREV        3
#define MSG_SENS_CHANGE 4

struct MediaMsg {
  uint8_t type;
  int8_t  ticks;
  uint8_t sensitivity;
};

volatile bool    remoteVolPending  = false;
volatile int8_t  remoteVolTicks    = 0;
volatile bool    remotePlayPending = false;
volatile bool    remoteNextPending = false;
volatile bool    remotePrevPending = false;
volatile bool    remoteSensPending = false;
volatile uint8_t remoteSensLevel   = 5;

void onMediaReceive(const esp_now_recv_info_t* info,
                    const uint8_t* data, int len) {
  if (len != sizeof(MediaMsg)) return;
  MediaMsg* m = (MediaMsg*)data;
  switch (m->type) {
    case MSG_VOLUME:      remoteVolTicks = m->ticks; remoteVolPending  = true; break;
    case MSG_PLAYPAUSE:   remotePlayPending = true; break;
    case MSG_NEXT:        remoteNextPending = true; break;
    case MSG_PREV:        remotePrevPending = true; break;
    case MSG_SENS_CHANGE: remoteSensLevel = m->sensitivity; remoteSensPending = true; break;
  }
}

void handleRemoteMedia() {
  if (!bleConnected) {
    remoteVolPending = remotePlayPending =
    remoteNextPending = remotePrevPending = false;
    return;
  }
  if (remoteVolPending) {
    remoteVolPending = false;
    int ticks = remoteVolTicks;
    int dir = (ticks > 0) ? 1 : -1;
    int n   = abs(ticks);
    for (int i = 0; i < n; i++)
      sendConsumer(dir > 0 ? CONSUMER_VOL_UP : CONSUMER_VOL_DOWN);
  }
  if (remotePlayPending) {
    remotePlayPending = false;
    sendConsumer(CONSUMER_PLAY_PAUSE);
  }
  if (remoteNextPending) {
    remoteNextPending = false;
    sendConsumer(CONSUMER_NEXT);
  }
  if (remotePrevPending) {
    remotePrevPending = false;
    sendConsumer(CONSUMER_PREV);
  }
  if (remoteSensPending) {
    remoteSensPending = false;
    // Show sensitivity on display for 1s
    spr.fillSprite(C_BG);
    spr.fillRect(0,0,128,18,C_SURF);
    spr.drawFastHLine(0,18,128,C_AMBER);
    spr.setTextColor(C_AMBER,C_SURF); spr.setTextSize(1);
    spr.setCursor(6,5); spr.print("REMOTE SENS");
    spr.setTextSize(3);
    spr.setTextColor(C_CYAN,C_BG);
    spr.setCursor(44,50); spr.print(remoteSensLevel);
    spr.setTextSize(1);
    spr.setTextColor(C_DIM,C_BG);
    spr.setCursor(10,100); spr.print("1=fine   10=coarse");
    spr.fillRoundRect(10,120,108,10,3,C_SURF2);
    int bw = (int)(108.0f * remoteSensLevel / 10.0f);
    spr.fillRoundRect(10,120,bw,10,3,C_AMBER);
    spr.drawRoundRect(10,120,108,10,3,C_BORDER);
    spr.pushSprite(0,0);
    delay(900);
    redraw();
  }
}

void setupESPNow() {
  Serial.println("ESP-NOW: init WiFi...");
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  Serial.println("ESP-NOW: esp_now_init...");
  if (esp_now_init() != ESP_OK) {
    Serial.println("ESP-NOW init failed"); return;
  }
  Serial.println("ESP-NOW: register cb...");
  esp_now_register_recv_cb(onMediaReceive);
  Serial.println("ESP-NOW: add peer...");
  esp_now_peer_info_t peer;
  memset(&peer, 0, sizeof(peer));
  memcpy(peer.peer_addr, SENDER_MAC, 6);
  peer.channel = 0;
  peer.encrypt = false;
  if (esp_now_add_peer(&peer) != ESP_OK) {
    Serial.println("ESP-NOW: peer failed");
  } else {
    Serial.println("ESP-NOW: ready");
  }
  espNowRunning = true;
  Serial.println("ESP-NOW: done");
}

// ════════════════════════════════════════════════
//  ACTION IDS
// ════════════════════════════════════════════════
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
#define A_LTS_MOVE   190
#define A_LTS_GND    191
#define A_LTS_VCC    192
#define A_LTS_RES    193
#define A_LTS_CAP    194
#define A_LTS_COMP   195
#define A_LTS_WIRE   196
#define A_LTS_RUN    197
#define A_LX_TERMINAL    212
#define A_LX_ZOOM_IN     213
#define A_LX_ZOOM_OUT    214
#define A_LX_ZOOM_RST    215
#define A_LX_SPLIT_H     217
#define A_LX_SPLIT_V     218
#define A_LX_NEW_TERM    219
#define A_LX_MAXIMIZE    220
#define A_LX_HALF_L      221
#define A_LX_HALF_R      222
#define A_LX_MOVE_WS1    223
#define A_LX_MOVE_WS2    224
#define KEY_EQUAL     0x2E

struct Action { const char* label; int id; };
const Action ACTION_LIB[] = {
  {"Play/Pause",A_PLAY},{"Next",A_NEXT},{"Prev",A_PREV},{"Mute",A_MUTE},
  {"Spotify",A_SPOTIFY},{"Vol Up",A_VOLUP},{"Vol Down",A_VOLDN},
  {"Lock PC",A_LOCK},{"Screenshot",A_SNIP},{"Full SS",A_SS_FULL},
  {"Task View",A_TASK},{"Min All",A_MINALL},{"Desk Right",A_DESK_R},
  {"Desk Left",A_DESK_L},{"Calculator",A_CALC},{"Explorer",A_EXPLORER},
  {"Win Sett.",A_SETTINGS_W},
  {"Undo",A_UNDO},{"Redo",A_REDO},{"Save",A_SAVE},{"Copy",A_COPY},
  {"Paste",A_PASTE},{"Cut",A_CUT},{"Select All",A_SELALL},
  {"Close Win",A_CLOSE},{"Find",A_FIND},{"Replace",A_REPLACE},
  {"New File",A_NEWFILE},{"Open File",A_OPENFILE},
  {"OS: Fit",A_OS_FIT},{"OS: Front",A_OS_FRONT},{"OS: Top",A_OS_TOP},
  {"OS: Right",A_OS_RIGHT},{"OS: Iso",A_OS_ISO},{"OS: ZFit",A_OS_ZOOM_FIT},
  {"OS: Extrude",A_OS_EXTRUDE},{"OS: Sketch",A_OS_SKETCH},
  {"OS: Mate",A_OS_MATE},{"OS: Assem.",A_OS_ASSEMBLY},
  {"KC: Route",A_KC_ROUTE},{"KC: AddNet",A_KC_ADD_NET},
  {"KC: ZFit",A_KC_ZOOM_FIT},{"KC: DRC",A_KC_DRC},{"KC: 3D",A_KC_3D},
  {"KC: Copper",A_KC_COPPER},{"KC: Gerber",A_KC_GERBER},
  {"KC: Rats",A_KC_RATSNEST},
  {"PTT",A_PUSH_TO_TALK},{"Reload",A_RELOAD},{"Map",A_MAP},
  {"Scoreboard",A_SCORE},{"Fullscreen",A_FULLSCREEN},
  {"OBS Rec",A_OBS_REC},{"OBS Stream",A_OBS_STREAM},{"Discord",A_DISCORD},
  {"New Tab",A_NEW_TAB},{"Close Tab",A_CLOSE_TAB},{"Reopen Tab",A_RETAB},
  {"Back",A_BACK},{"Forward",A_FORWARD},{"Refresh",A_REFRESH},
  {"Addr Bar",A_ADDR_BAR},
  {"LTS Move",A_LTS_MOVE},{"LTS GND",A_LTS_GND},{"LTS VCC",A_LTS_VCC},
  {"LTS Res",A_LTS_RES},{"LTS Cap",A_LTS_CAP},{"LTS Comp",A_LTS_COMP},
  {"LTS Wire",A_LTS_WIRE},{"LTS Run",A_LTS_RUN},
  {"LX:Term",A_LX_TERMINAL},{"LX:ZoomIn",A_LX_ZOOM_IN},
  {"LX:ZoomOt",A_LX_ZOOM_OUT},{"LX:ZmRst",A_LX_ZOOM_RST},
  {"LX:SplitH",A_LX_SPLIT_H},{"LX:SplitV",A_LX_SPLIT_V},
  {"LX:NewTrm",A_LX_NEW_TERM},{"LX:MaxWin",A_LX_MAXIMIZE},
  {"LX:HalfL",A_LX_HALF_L},{"LX:HalfR",A_LX_HALF_R},
  {"LX:Mv WS1",A_LX_MOVE_WS1},{"LX:Mv WS2",A_LX_MOVE_WS2},
};
const int ACTION_LIB_SIZE = sizeof(ACTION_LIB)/sizeof(Action);

// ════════════════════════════════════════════════
//  PRESETS
// ════════════════════════════════════════════════
#define NUM_PRESETS  8
#define MAX_BTNS     8

struct ButtonAction { char label[9]; int id; };
struct Preset { char name[10]; int btnCount; ButtonAction btns[MAX_BTNS]; };

Preset presets[NUM_PRESETS] = {
  {"ONSHAPE",8,{{"Fit",A_OS_FIT},{"Front",A_OS_FRONT},{"Top",A_OS_TOP},{"Right",A_OS_RIGHT},{"Iso",A_OS_ISO},{"Extrude",A_OS_EXTRUDE},{"Sketch",A_OS_SKETCH},{"Mate",A_OS_MATE}}},
  {"KICAD",8,{{"Route",A_KC_ROUTE},{"ZFit",A_KC_ZOOM_FIT},{"DRC",A_KC_DRC},{"3D",A_KC_3D},{"Copper",A_KC_COPPER},{"Gerber",A_KC_GERBER},{"Rats",A_KC_RATSNEST},{"AddNet",A_KC_ADD_NET}}},
  {"MUSIC",8,{{"Play",A_PLAY},{"Next",A_NEXT},{"Prev",A_PREV},{"Mute",A_MUTE},{"Vol Up",A_VOLUP},{"Vol Dn",A_VOLDN},{"New Tab",A_NEW_TAB},{"Close",A_CLOSE_TAB}}},
  {"LTSPICE",8,{{"Move",A_LTS_MOVE},{"GND",A_LTS_GND},{"VCC",A_LTS_VCC},{"Res",A_LTS_RES},{"Cap",A_LTS_CAP},{"AddComp",A_LTS_COMP},{"Wire",A_LTS_WIRE},{"Run",A_LTS_RUN}}},
  {"GAMING",8,{{"PTT",A_PUSH_TO_TALK},{"Reload",A_RELOAD},{"Map",A_MAP},{"Score",A_SCORE},{"FullScr",A_FULLSCREEN},{"OBSRec",A_OBS_REC},{"OBSStr",A_OBS_STREAM},{"Discord",A_DISCORD}}},
  {"SYS",8,{{"Term",A_LX_TERMINAL},{"Lock",A_LOCK},{"Snip",A_SNIP},{"Files",A_EXPLORER},{"MaxWin",A_LX_MAXIMIZE},{"HalfL",A_LX_HALF_L},{"HalfR",A_LX_HALF_R},{"Show Dk",A_MINALL}}},
  {"DEV",8,{{"Term",A_LX_TERMINAL},{"NewTab",A_LX_NEW_TERM},{"SplitH",A_LX_SPLIT_H},{"SplitV",A_LX_SPLIT_V},{"Desk R",A_DESK_R},{"Desk L",A_DESK_L},{"Zoom In",A_LX_ZOOM_IN},{"ZoomOut",A_LX_ZOOM_OUT}}},
  {"EDIT",8,{{"Undo",A_UNDO},{"Redo",A_REDO},{"Save",A_SAVE},{"Copy",A_COPY},{"Paste",A_PASTE},{"Find",A_FIND},{"Rplace",A_REPLACE},{"SelAll",A_SELALL}}},
};

// ════════════════════════════════════════════════
//  SCREEN / STATE
// ════════════════════════════════════════════════
enum Screen { SCR_MAIN, SCR_BUILD, SCR_BUILD_COUNT, SCR_BUILD_SLOT, SCR_BUILD_ACTION, SCR_SETTINGS };
Screen currentScreen = SCR_MAIN;

enum EncMode { MODE_VOLUME=0, MODE_SCROLL, MODE_ZOOM, MODE_ALTTAB, _MODE_COUNT };
const char*    modeNames[_MODE_COUNT]  = {"VOL","SCROLL","ZOOM","ALT-TAB"};
const uint16_t modeColors[_MODE_COUNT] = {C_CYAN, C_AMBER, C_MAGENTA, C_YELLOW};

int  activePreset  = 0;
int  encMode       = MODE_VOLUME;
bool locked        = false;

int  lastFlashBtn  = -1;
unsigned long flashUntil = 0;
unsigned long altReleaseAt = 0;
bool          altHeld      = false;

int  settingsSel = 0;
bool settingsAdj = false;
int  tmpBright   = 180;
int  tmpSleepMin = 5;
int  tmpSens     = 3;
bool tmpLinux    = false;
bool tmpEspNow   = false;

// Settings — 6 items now including ESP-NOW
const char* settingsItems[] = {
  "Brightness",
  "Sleep(min)",
  "Enc.Sens",
  "OS Layout",
  "ESP-NOW",
  "Exit+Save"
};
const int SETTINGS_COUNT = 6;

int  buildPreset    = 0;
int  buildCount     = 4;
int  buildSlot      = 0;
int  buildActSel    = 0;
int  buildActScroll = 0;

long encPos      = 0;
long lastEncPos  = 0;
unsigned long lastEncTime = 0;
long menuAccum   = 0;

bool lastBleConn = false;

static int  wakeKeyBtnIdx  = WAKEKEY_NONE;
static bool wakeKeyPending = false;
static unsigned long wakeTimeMs = 0;

unsigned long lastActivityMs = 0;
static inline void recordActivity() { lastActivityMs = millis(); }

// ════════════════════════════════════════════════
//  STATUS BAR
// ════════════════════════════════════════════════
void drawStatusBar(uint16_t accent=C_CYAN) {
  spr.fillRect(0,0,128,18,C_SURF);
  spr.drawFastHLine(0,18,128,C_BORDER);
  spr.fillCircle(6,9,4,bleConnected?C_GREEN:C_RED);
  spr.setTextSize(1);
  spr.setTextColor(bleConnected?C_GREEN:C_RED,C_SURF);
  spr.setCursor(13,5); spr.print(bleConnected?"BLE":"---");
  spr.setTextColor(accent,C_SURF);
  int nx=44-(strlen(presets[activePreset].name)*3);
  spr.setCursor(max(36,nx),5); spr.print(presets[activePreset].name);
  spr.setTextColor(linuxLayout?C_AMBER:C_DIM,C_SURF);
  spr.setCursor(84,5); spr.print(linuxLayout?"LX":"W");
  // ESP-NOW indicator
  if(espNowRunning){
    spr.setTextColor(C_GREEN,C_SURF);
    spr.setCursor(100,5); spr.print("EN");
  }
  if(locked){
    spr.fillRoundRect(114,3,12,9,2,C_YELLOW);
    spr.fillRect(116,1,6,5,C_SURF);
    spr.fillRoundRect(117,1,4,5,2,C_YELLOW);
  }
}

// ════════════════════════════════════════════════
//  DRAW MAIN
// ════════════════════════════════════════════════
void drawMain() {
  spr.fillSprite(C_BG);
  uint16_t ac=PRESET_COL[activePreset];
  for(int x=4;x<128;x+=8) for(int y=22;y<160;y+=8) spr.drawPixel(x,y,C_GRID);
  drawStatusBar(ac);

  if(!locked) {
    spr.fillRoundRect(16,24,96,20,8,C_SURF);
    spr.drawRoundRect(16,24,96,20,8,modeColors[encMode]);
    spr.fillCircle(26,34,4,modeColors[encMode]);
    spr.setTextColor(modeColors[encMode],C_SURF);
    spr.setTextSize(1);
    spr.setCursor(34,29); spr.print(modeNames[encMode]);
    spr.fillTriangle(10,32,10,40,5,36,modeColors[encMode]);
    spr.fillTriangle(118,32,118,40,123,36,modeColors[encMode]);

    int cx=64,cy=84;
    uint16_t ic=modeColors[encMode];
    switch(encMode){
      case MODE_VOLUME:
        spr.fillRect(cx-20,cy-12,10,24,ic);
        spr.fillTriangle(cx-10,cy-20,cx-10,cy+20,cx+18,cy,ic);
        spr.drawFastHLine(cx+22,cy-10,5,ic);
        spr.drawFastHLine(cx+22,cy,5,ic);
        spr.drawFastHLine(cx+22,cy+10,5,ic);
        break;
      case MODE_SCROLL:
        spr.drawRoundRect(cx-8,cy-26,16,52,7,ic);
        spr.fillRoundRect(cx-5,cy-14,10,28,4,ic);
        spr.fillTriangle(cx,cy-30,cx-5,cy-22,cx+5,cy-22,ic);
        spr.fillTriangle(cx,cy+30,cx-5,cy+22,cx+5,cy+22,ic);
        break;
      case MODE_ZOOM:
        spr.drawCircle(cx-4,cy-4,18,ic);
        spr.drawLine(cx+11,cy+11,cx+22,cy+22,ic);
        spr.drawFastHLine(cx-14,cy-4,20,ic);
        spr.drawFastVLine(cx-4,cy-14,20,ic);
        break;
      case MODE_ALTTAB:
        for(int i=0;i<3;i++) spr.drawRoundRect(cx-22+i*6,cy-14+i*5,30,22,3,ic);
        spr.fillRoundRect(cx-10,cy-6,30,22,3,ic);
        spr.setTextColor(C_BG,ic); spr.setCursor(cx-4,cy+1); spr.print("ALT");
        break;
    }
    spr.setTextSize(1);
    spr.setTextColor(C_DIM,C_BG);
    spr.setCursor(6,118); spr.print("Turn="); spr.setTextColor(modeColors[encMode],C_BG); spr.print(modeNames[encMode]);
    spr.setTextColor(C_DIM,C_BG);
    spr.setCursor(6,129); spr.print("Enc=Lock  HoldEnc=Menu");
    spr.drawFastHLine(0,141,128,C_BORDER);
    int page=activePreset/4;
    for(int p=0;p<4;p++){
      int idx=page*4+p;
      if(idx>=NUM_PRESETS) break;
      int tx=2+p*31;
      bool act=(idx==activePreset);
      uint16_t pc=PRESET_COL[idx];
      if(act){ spr.fillRoundRect(tx,143,29,15,3,pc); spr.setTextColor(C_BG,pc); }
      else   { spr.drawRoundRect(tx,143,29,15,3,C_BORDER); spr.setTextColor(C_DIM,C_BG); }
      spr.setCursor(tx+3,148);
      char ab[5]; strncpy(ab,presets[idx].name,4); ab[4]='\0';
      spr.print(ab);
    }
    for(int pg=0;pg<2;pg++){
      bool cur=(pg==page);
      if(cur) spr.fillCircle(120+pg*5,150,2,ac);
      else    spr.drawCircle(120+pg*5,150,2,C_DIM);
    }
  } else {
    spr.fillRect(0,20,128,14,C_SURF);
    spr.setTextSize(1);
    spr.setTextColor(C_YELLOW,C_SURF);
    spr.setCursor(4,24); spr.print("LOCKED ");
    spr.setTextColor(modeColors[encMode],C_SURF);
    spr.print(modeNames[encMode]);
    spr.drawFastHLine(0,34,128,C_BORDER);
    int cnt=presets[activePreset].btnCount;
    int rows=(cnt+1)/2;
    int bw=58, bh=min(32,(125-(rows-1)*4)/rows);
    for(int i=0;i<cnt;i++){
      int col=i%2, row=i/2;
      int x=4+col*(bw+6);
      int y=37+row*(bh+4);
      bool flash=(i==lastFlashBtn && millis()<flashUntil);
      uint16_t bg=flash?ac:C_SURF2;
      uint16_t fg=flash?C_BG:C_WHITE;
      uint16_t br=flash?C_WHITE:C_BORDER;
      spr.fillRoundRect(x,y,bw,bh,4,bg);
      spr.drawRoundRect(x,y,bw,bh,4,br);
      spr.setTextColor(flash?bg:C_DIM,bg);
      spr.setCursor(x+3,y+2); spr.print(i+1);
      spr.setTextColor(fg,bg);
      const char* lbl=presets[activePreset].btns[i].label;
      int lx=x+max(0,(bw-(int)strlen(lbl)*6)/2);
      spr.setCursor(lx,y+(bh/2)-3); spr.print(lbl);
    }
    if(encMode==MODE_ALTTAB && altHeld){
      spr.fillRect(0,152,128,8,C_BG);
      spr.setTextColor(C_YELLOW,C_BG);
      spr.setCursor(14,153); spr.print("< ALT SELECTING... >");
    }
  }
  spr.pushSprite(0,0);
}

// ════════════════════════════════════════════════
//  DRAW BUILD
// ════════════════════════════════════════════════
void drawBuild() {
  spr.fillSprite(C_BG);
  drawStatusBar(C_BUILD);
  spr.fillRect(0,20,128,18,C_SURF);
  spr.drawFastHLine(0,38,128,C_BUILD);
  spr.setTextColor(C_BUILD,C_SURF); spr.setTextSize(1);
  spr.setCursor(6,26); spr.print("BUILD MODE");
  spr.setTextColor(C_DIM,C_SURF); spr.setCursor(76,26); spr.print("LEGO");
  spr.setTextColor(C_WHITE,C_BG); spr.setCursor(6,46); spr.print("Editing:");
  spr.setTextColor(PRESET_COL[buildPreset],C_BG); spr.setCursor(6,58); spr.print(presets[buildPreset].name);
  spr.setTextColor(C_DIM,C_BG); spr.setCursor(6,74); spr.print("Active slots:");
  spr.setTextColor(C_WHITE,C_BG); spr.setCursor(84,74); spr.print(presets[buildPreset].btnCount);
  int pc=presets[buildPreset].btnCount;
  for(int i=0;i<pc && i<6;i++){
    int px=6+(i%3)*40, py=86+(i/3)*16;
    spr.fillRoundRect(px,py,36,13,3,C_SURF2);
    spr.setTextColor(C_WHITE,C_SURF2); spr.setCursor(px+3,py+3);
    char tmp[6]; strncpy(tmp,presets[buildPreset].btns[i].label,5); tmp[5]='\0';
    spr.print(tmp);
  }
  spr.drawFastHLine(0,130,128,C_BORDER);
  spr.setTextColor(C_DIM,C_BG); spr.setCursor(6,136); spr.print("Turn=pick preset");
  spr.setTextColor(C_BUILD,C_BG); spr.setCursor(6,148); spr.print("Press=START EDITING");
  spr.pushSprite(0,0);
}

void drawBuildCount() {
  spr.fillSprite(C_BG);
  drawStatusBar(C_BUILD);
  spr.fillRect(0,20,128,18,C_SURF);
  spr.drawFastHLine(0,38,128,C_BUILD);
  spr.setTextColor(C_BUILD,C_SURF); spr.setTextSize(1);
  spr.setCursor(6,26); spr.print("STEP 1: HOW MANY BTNS?");
  spr.setTextColor(C_DIM,C_BG); spr.setCursor(6,44); spr.print("Turn encoder to choose");
  spr.setTextSize(4);
  spr.setTextColor(C_BUILD,C_BG);
  char buf[3]; sprintf(buf,"%d",buildCount);
  spr.setCursor(64-(strlen(buf)*12),58); spr.print(buf);
  spr.setTextSize(1);
  spr.setTextColor(C_DIM,C_BG); spr.setCursor(28,100); spr.print("buttons active");
  for(int i=0;i<MAX_BTNS;i++){
    int dx=18+i*12,dy=116;
    if(i<buildCount) spr.fillCircle(dx,dy,5,C_BUILD);
    else             spr.drawCircle(dx,dy,5,C_BORDER);
  }
  spr.setTextColor(C_BUILD,C_BG); spr.setCursor(6,154); spr.print("Press=confirm");
  spr.pushSprite(0,0);
}

void drawBuildSlot() {
  spr.fillSprite(C_BG);
  drawStatusBar(C_BUILD);
  spr.fillRect(0,20,128,18,C_SURF);
  spr.drawFastHLine(0,38,128,C_BUILD);
  spr.setTextColor(C_BUILD,C_SURF); spr.setTextSize(1);
  spr.setCursor(6,26); spr.print("STEP 2: PICK SLOT");
  int cnt=buildCount;
  int rows=(cnt+1)/2;
  int bw=56, bh=min(32,(120-(rows-1)*4)/rows);
  for(int i=0;i<cnt;i++){
    int col=i%2,row=i/2;
    int x=4+col*(bw+6), y=42+row*(bh+4);
    bool sel=(i==buildSlot);
    spr.fillRoundRect(x,y,bw,bh,4,sel?C_BUILD:C_SURF);
    spr.drawRoundRect(x,y,bw,bh,4,sel?C_WHITE:C_BORDER);
    spr.setTextColor(sel?C_BG:C_DIM,sel?C_BUILD:C_SURF); spr.setCursor(x+3,y+2); spr.print(i+1);
    spr.setTextColor(sel?C_WHITE:C_DIM,sel?C_BUILD:C_SURF);
    const char* lbl=presets[buildPreset].btns[i].label;
    spr.setCursor(x+max(0,(bw-(int)strlen(lbl)*6)/2),y+(bh/2)-3); spr.print(lbl);
  }
  spr.setTextColor(C_BUILD,C_BG); spr.setCursor(6,155); spr.print("Press=assign action");
  spr.pushSprite(0,0);
}

void drawBuildAction() {
  spr.fillSprite(C_BG);
  drawStatusBar(C_BUILD);
  spr.fillRect(0,20,128,18,C_SURF);
  spr.drawFastHLine(0,38,128,C_BUILD);
  spr.setTextColor(C_BUILD,C_SURF); spr.setTextSize(1);
  char hdr[22]; sprintf(hdr,"SLOT %d: PICK ACTION",buildSlot+1);
  spr.setCursor(6,26); spr.print(hdr);
  const int VISIBLE=6;
  for(int i=0;i<VISIBLE;i++){
    int idx=buildActScroll+i;
    if(idx>=ACTION_LIB_SIZE) break;
    bool sel=(idx==buildActSel);
    int ry=40+i*19;
    if(sel){ spr.fillRoundRect(2,ry,116,17,3,C_BUILD); spr.setTextColor(C_WHITE,C_BUILD); }
    else   { spr.setTextColor(C_DIM,C_BG); }
    spr.setCursor(8,ry+5); spr.print(ACTION_LIB[idx].label);
    if(sel){ spr.setCursor(110,ry+5); spr.print(">"); }
  }
  int sbH=6*19;
  int sbPos=40+(buildActSel*(sbH-8))/max(1,ACTION_LIB_SIZE-1);
  spr.fillRect(122,40,4,sbH,C_BORDER);
  spr.fillRect(122,sbPos,4,8,C_BUILD);
  spr.setTextColor(C_BUILD,C_BG); spr.setCursor(6,156); spr.print("Press=assign  Hold=back");
  spr.pushSprite(0,0);
}

// ════════════════════════════════════════════════
//  DRAW SETTINGS — 6 items with ESP-NOW toggle
// ════════════════════════════════════════════════
void drawSettings() {
  spr.fillSprite(C_BG);
  drawStatusBar(C_CYAN);
  spr.fillRect(0,20,128,18,C_SURF);
  spr.drawFastHLine(0,38,128,C_CYAN);
  spr.setTextColor(C_CYAN,C_SURF); spr.setTextSize(1);
  spr.setCursor(6,26); spr.print("SETTINGS");
  spr.setTextColor(C_DIM,C_SURF); spr.setCursor(76,26); spr.print("enc=nav");

  // 6 items, 18px each to fit
  for(int i=0;i<SETTINGS_COUNT;i++){
    bool sel=(i==settingsSel);
    int ry=40+i*19;
    if(sel){ spr.fillRoundRect(2,ry,124,17,3,C_SURF2); spr.drawRoundRect(2,ry,124,17,3,C_CYAN); }
    spr.setTextColor(sel?C_WHITE:C_DIM, sel?C_SURF2:C_BG);
    spr.setCursor(8,ry+5); spr.print(settingsItems[i]);

    char vb[12];
    if(i==0){
      sprintf(vb,"%d",tmpBright);
      spr.setTextColor(sel?C_CYAN:C_BORDER,sel?C_SURF2:C_BG);
      spr.setCursor(96,ry+5); spr.print(vb);
    } else if(i==1){
      spr.setTextColor(tmpSleepMin==0?(sel?C_GREEN:C_DIM):(sel?C_CYAN:C_BORDER),sel?C_SURF2:C_BG);
      if(tmpSleepMin==0){ spr.setCursor(88,ry+5); spr.print("OFF"); }
      else { sprintf(vb,"%dm",tmpSleepMin); spr.setCursor(92,ry+5); spr.print(vb); }
    } else if(i==2){
      sprintf(vb,"%d",tmpSens);
      spr.setTextColor(sel?C_CYAN:C_BORDER,sel?C_SURF2:C_BG);
      spr.setCursor(104,ry+5); spr.print(vb);
    } else if(i==3){
      spr.setTextColor(tmpLinux?C_AMBER:C_LTBLUE,sel?C_SURF2:C_BG);
      spr.setCursor(80,ry+5); spr.print(tmpLinux?"LINUX":"WIN");
    } else if(i==4){
      // ESP-NOW toggle
      spr.setTextColor(tmpEspNow?C_GREEN:C_DIM,sel?C_SURF2:C_BG);
      spr.setCursor(88,ry+5); spr.print(tmpEspNow?"ON":"OFF");
      if(tmpEspNow && espNowRunning){
        spr.setTextColor(C_GREEN,sel?C_SURF2:C_BG);
        spr.setCursor(104,ry+5); spr.print("*");
      }
    } else if(i==5){
      spr.setTextColor(C_GREEN,sel?C_SURF2:C_BG);
      spr.setCursor(96,ry+5); spr.print("SAVE");
    }
    if(sel && settingsAdj && i<3){
      spr.setTextColor(C_AMBER,C_SURF2); spr.setCursor(116,ry+5); spr.print("<");
    }
  }
  spr.setTextColor(C_DIM,C_BG); spr.setCursor(2,154); spr.print("Press=select  Hold=back");
  spr.pushSprite(0,0);
}

// ════════════════════════════════════════════════
//  REDRAW
// ════════════════════════════════════════════════
void redraw(){
  switch(currentScreen){
    case SCR_MAIN:         drawMain();        break;
    case SCR_BUILD:        drawBuild();       break;
    case SCR_BUILD_COUNT:  drawBuildCount();  break;
    case SCR_BUILD_SLOT:   drawBuildSlot();   break;
    case SCR_BUILD_ACTION: drawBuildAction(); break;
    case SCR_SETTINGS:     drawSettings();    break;
  }
}

// ════════════════════════════════════════════════
//  RECONNECT HUD
// ════════════════════════════════════════════════
void drawReconnectHUD(const char* keyName) {
  spr.fillSprite(C_BG);
  spr.fillRect(0,0,128,18,C_SURF);
  spr.drawFastHLine(0,18,128,C_AMBER);
  spr.setTextColor(C_AMBER,C_SURF); spr.setTextSize(1);
  spr.setCursor(10,5); spr.print("RECONNECTING...");
  uint16_t pulse=((millis()/300)%2)?C_AMBER:C_SURF2;
  spr.fillCircle(64,62,22,C_SURF2);
  spr.drawCircle(64,62,22,pulse);
  spr.setTextColor(C_AMBER,C_SURF2); spr.setTextSize(2);
  spr.setCursor(58,53); spr.print("B");
  spr.setTextSize(1);
  spr.fillRoundRect(10,95,108,28,6,C_SURF2);
  spr.drawRoundRect(10,95,108,28,6,C_AMBER);
  spr.setTextColor(C_DIM,C_SURF2); spr.setCursor(16,100); spr.print("Queued:");
  spr.setTextColor(C_WHITE,C_SURF2);
  int kx=64-(strlen(keyName)*3);
  spr.setCursor(max(16,kx),112); spr.print(keyName);
  spr.setTextColor(C_DIM,C_BG);
  spr.setCursor(8,135); spr.print("Will fire when connected");
  spr.pushSprite(0,0);
}

// ════════════════════════════════════════════════
//  ENCODER READ
// ════════════════════════════════════════════════
static const int8_t ENC_TABLE[]={0,-1,1,0,1,0,0,-1,-1,0,0,1,0,1,-1,0};
void updateEncoder() {
  static uint8_t prev=0;
  uint8_t ab=0;
  if(digitalRead(ENC_CLK)) ab|=0x02;
  if(digitalRead(ENC_DT))  ab|=0x01;
  prev=(prev<<2)|ab;
  int8_t d=ENC_TABLE[prev&0x0F];
  if(d!=0){
    unsigned long now=millis();
    if(now-lastEncTime<ENC_DEBOUNCE_MS) return;
    lastEncTime=now;
    encPos+=d*ENC_DIR;
  }
}

// ════════════════════════════════════════════════
//  FIRE ACTION
// ════════════════════════════════════════════════
void fireAction(int id){
  if(!bleConnected) return;
  switch(id){
    case A_PLAY:      sendConsumer(CONSUMER_PLAY_PAUSE); break;
    case A_NEXT:      sendConsumer(CONSUMER_NEXT);       break;
    case A_PREV:      sendConsumer(CONSUMER_PREV);       break;
    case A_MUTE:      sendConsumer(CONSUMER_MUTE);       break;
    case A_VOLUP:     sendConsumer(CONSUMER_VOL_UP);     break;
    case A_VOLDN:     sendConsumer(CONSUMER_VOL_DOWN);   break;
    case A_UNDO:      sendKey(MOD_LCTRL, KEY_Z); break;
    case A_REDO:      if(linuxLayout) sendKey(MOD_LCTRL|MOD_LSHIFT,KEY_Z); else sendKey(MOD_LCTRL,KEY_Y); break;
    case A_SAVE:      sendKey(MOD_LCTRL, KEY_S); break;
    case A_COPY:      sendKey(MOD_LCTRL, KEY_C); break;
    case A_PASTE:     sendKey(MOD_LCTRL, KEY_V); break;
    case A_CUT:       sendKey(MOD_LCTRL, KEY_X); break;
    case A_SELALL:    sendKey(MOD_LCTRL, KEY_A); break;
    case A_FIND:      sendKey(MOD_LCTRL, KEY_F); break;
    case A_REPLACE:   sendKey(MOD_LCTRL, KEY_H); break;
    case A_NEWFILE:   sendKey(MOD_LCTRL, KEY_N); break;
    case A_OPENFILE:  sendKey(MOD_LCTRL, KEY_O); break;
    case A_CLOSE:     sendKey(MOD_LALT,  KEY_F4); break;
    case A_LOCK:      sendKey(MOD_LGUI,  KEY_L); break;
    case A_SNIP:      if(linuxLayout) sendKey(MOD_LSHIFT,KEY_PRTSC); else sendKey(MOD_LGUI|MOD_LSHIFT,KEY_S); break;
    case A_SS_FULL:   if(linuxLayout) sendKey(0,KEY_PRTSC); else sendKey(MOD_LGUI,KEY_PRTSC); break;
    case A_TASK:      sendKey(MOD_LGUI, KEY_TAB); break;
    case A_MINALL:    if(linuxLayout) sendKey(MOD_LGUI,KEY_D); else sendKey(MOD_LGUI,KEY_M); break;
    case A_DESK_R:    if(linuxLayout) sendKey(MOD_LCTRL|MOD_LALT,KEY_RIGHT_ARR); else sendKey(MOD_LCTRL|MOD_LGUI,KEY_RIGHT_ARR); break;
    case A_DESK_L:    if(linuxLayout) sendKey(MOD_LCTRL|MOD_LALT,KEY_LEFT_ARR);  else sendKey(MOD_LCTRL|MOD_LGUI,KEY_LEFT_ARR);  break;
    case A_EXPLORER:  sendKey(MOD_LGUI, KEY_E); break;
    case A_SETTINGS_W:sendKey(MOD_LGUI, KEY_I); break;
    case A_SPOTIFY:   sendKey(MOD_LGUI, KEY_3); break;
    case A_OS_FIT:    sendKey(0, KEY_F); break;
    case A_OS_FRONT:  sendKey(MOD_LSHIFT, KEY_1); break;
    case A_OS_TOP:    sendKey(MOD_LSHIFT, KEY_2); break;
    case A_OS_RIGHT:  sendKey(MOD_LSHIFT, KEY_3); break;
    case A_OS_ISO:    sendKey(MOD_LSHIFT, KEY_7); break;
    case A_OS_ZOOM_FIT: sendKey(MOD_LCTRL|MOD_LSHIFT, KEY_F); break;
    case A_OS_EXTRUDE:  sendKey(MOD_LSHIFT, KEY_E); break;
    case A_OS_SKETCH:   sendKey(0, KEY_S); break;
    case A_OS_MATE:     sendKey(MOD_LCTRL, KEY_M); break;
    case A_OS_ASSEMBLY: sendKey(MOD_LALT,  KEY_A); break;
    case A_KC_ROUTE:    sendKey(0, KEY_X); break;
    case A_KC_ADD_NET:  sendKey(0, KEY_W); break;
    case A_KC_ZOOM_FIT: sendKey(0, KEY_5); break;
    case A_KC_DRC:      sendKey(MOD_LALT, KEY_3); break;
    case A_KC_3D:       sendKey(0, KEY_3); break;
    case A_KC_COPPER:   sendKey(MOD_LCTRL, KEY_K); break;
    case A_KC_GERBER:   sendKey(MOD_LCTRL, KEY_P); break;
    case A_KC_RATSNEST: sendKey(0, KEY_BACKTICK); break;
    case A_PUSH_TO_TALK:sendKey(0, KEY_V); break;
    case A_RELOAD:      sendKey(0, KEY_R); break;
    case A_MAP:         sendKey(0, KEY_M); break;
    case A_SCORE:       sendKey(0, KEY_TAB); break;
    case A_FULLSCREEN:  sendKey(0, KEY_F11); break;
    case A_OBS_REC:     sendKey(MOD_LCTRL|MOD_LALT, KEY_R); break;
    case A_OBS_STREAM:  sendKey(MOD_LCTRL|MOD_LALT, KEY_S); break;
    case A_DISCORD:     sendKey(MOD_LCTRL|MOD_LALT, KEY_A); break;
    case A_NEW_TAB:     sendKey(MOD_LCTRL, KEY_T); break;
    case A_CLOSE_TAB:   sendKey(MOD_LCTRL, KEY_W); break;
    case A_RETAB:       sendKey(MOD_LCTRL|MOD_LSHIFT, KEY_T); break;
    case A_BACK:        sendKey(MOD_LALT, KEY_LEFT_ARR); break;
    case A_FORWARD:     sendKey(MOD_LALT, KEY_RIGHT_ARR); break;
    case A_REFRESH:     sendKey(MOD_LCTRL, KEY_R); break;
    case A_ADDR_BAR:    sendKey(MOD_LCTRL, KEY_L); break;
    case A_LTS_MOVE:    sendKey(0, KEY_M); break;
    case A_LTS_GND:     sendKey(0, KEY_G); break;
    case A_LTS_VCC:     sendKey(0, KEY_V); break;
    case A_LTS_RES:     sendKey(0, KEY_R); break;
    case A_LTS_CAP:     sendKey(0, KEY_C); break;
    case A_LTS_COMP:    sendKey(0, KEY_P); break;
    case A_LTS_WIRE:    sendKey(0, KEY_W); break;
    case A_LTS_RUN:     sendKey(MOD_LALT, KEY_R); break;
    case A_LX_TERMINAL: sendKey(MOD_LCTRL|MOD_LALT, KEY_T); break;
    case A_LX_ZOOM_IN:  sendKey(MOD_LCTRL, KEY_EQUAL); break;
    case A_LX_ZOOM_OUT: sendKey(MOD_LCTRL, KEY_MINUS); break;
    case A_LX_ZOOM_RST: sendKey(MOD_LCTRL, KEY_0); break;
    case A_LX_SPLIT_H:  sendKey(MOD_LCTRL|MOD_LSHIFT, KEY_E); break;
    case A_LX_SPLIT_V:  sendKey(MOD_LCTRL|MOD_LSHIFT, KEY_O); break;
    case A_LX_NEW_TERM: sendKey(MOD_LCTRL|MOD_LSHIFT, KEY_T); break;
    case A_LX_MAXIMIZE: sendKey(MOD_LGUI, KEY_UP_ARR); break;
    case A_LX_HALF_L:   sendKey(MOD_LGUI, KEY_LEFT_ARR); break;
    case A_LX_HALF_R:   sendKey(MOD_LGUI, KEY_RIGHT_ARR); break;
    case A_LX_MOVE_WS1: sendKey(MOD_LGUI|MOD_LSHIFT, KEY_1); break;
    case A_LX_MOVE_WS2: sendKey(MOD_LGUI|MOD_LSHIFT, KEY_2); break;
  }
}

// ════════════════════════════════════════════════
//  ENCODER ACTION
// ════════════════════════════════════════════════
void handleEncoderAction(){
  if(encPos==lastEncPos) return;
  long delta=encPos-lastEncPos;
  lastEncPos=encPos;
  recordActivity();

  auto menuStep=[&](int thresh=2)->int{
    menuAccum+=delta;
    if(menuAccum>=thresh){ menuAccum=0; return 1; }
    if(menuAccum<=-thresh){ menuAccum=0; return -1; }
    return 0;
  };

  if(currentScreen==SCR_SETTINGS){
    int s=menuStep(); if(!s) return;
    if(settingsAdj){
      if(settingsSel==0) tmpBright  =constrain(tmpBright  +s*10,0,255);
      if(settingsSel==1) tmpSleepMin=constrain(tmpSleepMin+s,0,60);
      if(settingsSel==2) tmpSens    =constrain(tmpSens    +s,1,8);
      if(settingsSel==3) tmpLinux   =!tmpLinux;
      if(settingsSel==4) tmpEspNow  =!tmpEspNow;
    } else settingsSel=constrain(settingsSel+s,0,SETTINGS_COUNT-1);
    drawSettings(); return;
  }
  if(currentScreen==SCR_BUILD){ int s=menuStep(); if(!s) return; buildPreset=constrain(buildPreset+s,0,NUM_PRESETS-1); drawBuild(); return; }
  if(currentScreen==SCR_BUILD_COUNT){ int s=menuStep(); if(!s) return; buildCount=constrain(buildCount+s,1,MAX_BTNS); drawBuildCount(); return; }
  if(currentScreen==SCR_BUILD_SLOT){ int s=menuStep(); if(!s) return; buildSlot=constrain(buildSlot+s,0,buildCount-1); drawBuildSlot(); return; }
  if(currentScreen==SCR_BUILD_ACTION){
    int s=menuStep(); if(!s) return;
    buildActSel=constrain(buildActSel+s,0,ACTION_LIB_SIZE-1);
    const int VIS=6;
    if(buildActSel<buildActScroll) buildActScroll=buildActSel;
    if(buildActSel>=buildActScroll+VIS) buildActScroll=buildActSel-VIS+1;
    drawBuildAction(); return;
  }
  if(currentScreen==SCR_MAIN && !locked){
    int s=menuStep(5); if(!s) return;
    encMode=(encMode+s+_MODE_COUNT)%_MODE_COUNT;
    drawMain(); return;
  }
  if(currentScreen==SCR_MAIN && locked){
    if(!bleConnected) return;
    static long acc=0;
    acc+=delta;
    if(abs(acc)<encoderSensitivity) return;
    int steps=acc/encoderSensitivity;
    acc%=encoderSensitivity;
    int dir=(steps>0)?1:-1;
    int n=abs(steps);
    switch(encMode){
      case MODE_VOLUME:  for(int i=0;i<n;i++) sendConsumer(dir>0?CONSUMER_VOL_UP:CONSUMER_VOL_DOWN); break;
      case MODE_SCROLL:  for(int i=0;i<n;i++) sendKey(0,dir>0?KEY_DOWN_ARR:KEY_UP_ARR); break;
      case MODE_ZOOM:    for(int i=0;i<n;i++) sendKey(MOD_LCTRL,dir>0?KEY_EQUAL:KEY_MINUS); break;
      case MODE_ALTTAB:
        if(!altHeld){ holdMod(MOD_LALT); altHeld=true; delay(60); }
        for(int i=0;i<n;i++){
          if(dir>0) sendKeyWithHeld(0,KEY_TAB);
          else      sendKeyWithHeld(MOD_LSHIFT,KEY_TAB);
          delay(30);
        }
        altReleaseAt=millis()+ALT_HOLD_MS;
        drawMain();
        break;
    }
  }
}

// ════════════════════════════════════════════════
//  BUTTON HANDLER
// ════════════════════════════════════════════════
void onButtonPressed(int idx){
  recordActivity();
  if(idx==9){
    if(currentScreen==SCR_MAIN){
      tmpBright  =backlightBrightness;
      tmpSleepMin=(sleepTimeoutMs==0)?0:(int)(sleepTimeoutMs/60000UL);
      tmpSens    =encoderSensitivity;
      tmpLinux   =linuxLayout;
      tmpEspNow  =espNowEnabled;
      settingsSel=0; settingsAdj=false;
      currentScreen=SCR_SETTINGS; drawSettings();
    }
    else if(currentScreen==SCR_SETTINGS){ currentScreen=SCR_MAIN; drawMain(); }
    else if(currentScreen==SCR_BUILD){ currentScreen=SCR_MAIN; drawMain(); }
    else if(currentScreen==SCR_BUILD_COUNT){ currentScreen=SCR_BUILD; drawBuild(); }
    else if(currentScreen==SCR_BUILD_SLOT){ currentScreen=SCR_BUILD_COUNT; drawBuildCount(); }
    else if(currentScreen==SCR_BUILD_ACTION){ currentScreen=SCR_BUILD_SLOT; drawBuildSlot(); }
    return;
  }

  if(currentScreen==SCR_SETTINGS){
    if(idx!=8) return;
    if(settingsSel==SETTINGS_COUNT-1){
      // Exit+Save
      backlightBrightness=tmpBright;
      ledcWrite(TFT_BL,backlightBrightness);
      sleepTimeoutMs=(tmpSleepMin==0)?0UL:(unsigned long)tmpSleepMin*60000UL;
      encoderSensitivity=tmpSens;
      linuxLayout=tmpLinux;
      espNowEnabled=tmpEspNow;
      // Start ESP-NOW if just enabled and not already running
      if(espNowEnabled && !espNowRunning){
        setupESPNow();
      }
      saveSettings();
      settingsAdj=false;
      currentScreen=SCR_MAIN;
      drawMain();
    } else if(settingsSel==3){
      tmpLinux=!tmpLinux; drawSettings();
    } else if(settingsSel==4){
      // ESP-NOW toggle
      tmpEspNow=!tmpEspNow; drawSettings();
    } else {
      settingsAdj=!settingsAdj;
      if(!settingsAdj && settingsSel==0){ backlightBrightness=tmpBright; ledcWrite(TFT_BL,backlightBrightness); }
      drawSettings();
    }
    return;
  }

  if(currentScreen==SCR_BUILD){ if(idx!=8) return; buildCount=presets[buildPreset].btnCount; currentScreen=SCR_BUILD_COUNT; drawBuildCount(); return; }
  if(currentScreen==SCR_BUILD_COUNT){ if(idx!=8) return; presets[buildPreset].btnCount=buildCount; buildSlot=0; currentScreen=SCR_BUILD_SLOT; drawBuildSlot(); return; }
  if(currentScreen==SCR_BUILD_SLOT){ if(idx!=8) return; buildActSel=0; buildActScroll=0; currentScreen=SCR_BUILD_ACTION; drawBuildAction(); return; }
  if(currentScreen==SCR_BUILD_ACTION){
    if(idx!=8) return;
    strncpy(presets[buildPreset].btns[buildSlot].label,ACTION_LIB[buildActSel].label,8);
    presets[buildPreset].btns[buildSlot].label[8]='\0';
    presets[buildPreset].btns[buildSlot].id=ACTION_LIB[buildActSel].id;
    if(buildSlot<buildCount-1){ buildSlot++; buildActSel=0; buildActScroll=0; currentScreen=SCR_BUILD_SLOT; drawBuildSlot(); }
    else { activePreset=buildPreset; currentScreen=SCR_MAIN; drawMain(); }
    return;
  }
  if(idx==8){
    locked=!locked;
    if(!locked){ releaseAll(); altHeld=false; altReleaseAt=0; }
    drawMain(); return;
  }
  if(!locked){ activePreset=idx; drawMain(); return; }
  if(idx<presets[activePreset].btnCount && bleConnected){
    lastFlashBtn=idx; flashUntil=millis()+FLASH_MS;
    drawMain();
    fireAction(presets[activePreset].btns[idx].id);
  }
}

// ════════════════════════════════════════════════
//  SLEEP / WAKE
// ════════════════════════════════════════════════
void maybeEnterSleep(){
  if(sleepTimeoutMs==0) return;
  if(millis()-lastActivityMs < sleepTimeoutMs) return;
  for(int i=0;i<8;i++) if(digitalRead(BTN_PINS[i])==LOW){ lastActivityMs=millis(); return; }
  if(digitalRead(ENC_SW)==LOW){ lastActivityMs=millis(); return; }

  ledcWrite(TFT_BL,0);
  spr.fillSprite(0x0000); spr.pushSprite(0,0);
  releaseAll();
  NimBLEDevice::stopAdvertising();
  delay(200);

  esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
  delay(50);
  for(int i=0;i<8;i++) gpio_wakeup_enable((gpio_num_t)BTN_PINS[i], GPIO_INTR_LOW_LEVEL);
  gpio_wakeup_enable((gpio_num_t)ENC_SW, GPIO_INTR_LOW_LEVEL);
  esp_sleep_enable_gpio_wakeup();
  Serial.flush();
  esp_light_sleep_start();

  wakeTimeMs=millis();
  lastActivityMs=wakeTimeMs;
  ledcAttach(TFT_BL, 5000, 8);
  ledcWrite(TFT_BL, backlightBrightness);
  delay(50);

  wakeKeyBtnIdx=WAKEKEY_NONE; wakeKeyPending=false;
  for(int i=0;i<8;i++){
    if(digitalRead(BTN_PINS[i])==LOW){
      if(locked && i<presets[activePreset].btnCount){ wakeKeyBtnIdx=i; wakeKeyPending=true; }
      break;
    }
  }
  if(wakeKeyPending) drawReconnectHUD(presets[activePreset].btns[wakeKeyBtnIdx].label);
  else redraw();
}

// ════════════════════════════════════════════════
//  SETUP
// ════════════════════════════════════════════════
void setup(){
  Serial.begin(115200);
  delay(500);
  Serial.println("Booting...");

  for(int i=0;i<8;i++) pinMode(BTN_PINS[i],INPUT_PULLUP);
  pinMode(ENC_SW,INPUT_PULLUP);
  pinMode(ENC_CLK,INPUT_PULLUP);
  pinMode(ENC_DT,INPUT_PULLUP);

  // Load settings from NVS first
  loadSettings();

  // TFT init — before NimBLE, before backlight
  tft.init(); tft.setRotation(2);
  spr.setColorDepth(16); spr.createSprite(128,160);
  Serial.println("TFT init done");

  // NimBLE init — resets LEDC, so backlight must come AFTER
  NimBLEDevice::init("ESP32 MacroPad");
  NimBLEDevice::setPower(ESP_PWR_LVL_P9);
  NimBLEDevice::setSecurityAuth(BLE_SM_PAIR_AUTHREQ_BOND);
  NimBLEDevice::setSecurityIOCap(BLE_HS_IO_NO_INPUT_OUTPUT);
  NimBLEDevice::setSecurityInitKey(BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID);
  NimBLEDevice::setSecurityRespKey(BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID);

  pServer = NimBLEDevice::createServer();
  pServer->setCallbacks(new ServerCB());
  pHID = new NimBLEHIDDevice(pServer);
  pHID->setManufacturer("DIY");
  pHID->setPnp(0x02, 0x05AC, 0x0220, 0x0100);
  pHID->setHidInfo(0x00, 0x01);
  pHID->setReportMap((uint8_t*)hidReportMap, sizeof(hidReportMap));
  pKbReport = pHID->getInputReport(1);
  pCcReport = pHID->getInputReport(2);
  pHID->startServices();
  Serial.println("NimBLE init done");

  NimBLEAdvertising* pAdv = NimBLEDevice::getAdvertising();
  NimBLEAdvertisementData advData;
  advData.setFlags(0x06);
  advData.setAppearance(0x03C1);
  advData.addServiceUUID(pHID->getHidService()->getUUID());
  pAdv->setAdvertisementData(advData);
  NimBLEAdvertisementData scanData;
  scanData.setName("ESP32 MacroPad");
  pAdv->setScanResponseData(scanData);
  pAdv->enableScanResponse(true);
  pAdv->setMinInterval(32);
  pAdv->setMaxInterval(48);
  pAdv->start();
  Serial.println("BLE advertising");

  // ESP-NOW — only init if enabled in settings
  // Keeping WiFi off by default avoids LEDC/memory conflicts
  if(espNowEnabled){
    Serial.println("ESP-NOW enabled — initialising...");
    setupESPNow();
  } else {
    Serial.println("ESP-NOW disabled — pure BLE mode");
  }

  // Backlight — AFTER all radio inits
  gpio_set_direction(GPIO_NUM_12, GPIO_MODE_OUTPUT);
  gpio_set_level(GPIO_NUM_12, 0);
  delay(10);
  ledcAttach(TFT_BL, 5000, 8);
  ledcWrite(TFT_BL, backlightBrightness);
  Serial.println("Backlight on");

  // Boot splash
  spr.fillSprite(C_BG);
  spr.setTextColor(C_CYAN,C_BG); spr.setTextSize(2);
  spr.setCursor(10,20); spr.print("MACRO");
  spr.setCursor(10,42); spr.print("PAD v4");
  spr.setTextSize(1);
  spr.setTextColor(espNowEnabled?C_GREEN:C_DIM,C_BG);
  spr.setCursor(10,70); spr.print(espNowEnabled?"ESP-NOW: ON":"ESP-NOW: OFF");
  spr.setTextColor(C_DIM,C_BG);
  spr.setCursor(10,85); spr.print("NimBLE stack");
  spr.setCursor(10,97); spr.print("8 Presets + BUILD MODE");
  spr.setCursor(10,109); spr.print("Settings > ESP-NOW > ON");
  spr.pushSprite(0,0);
  Serial.println("Splash drawn — boot complete");

  tmpBright  =backlightBrightness;
  tmpSleepMin=(sleepTimeoutMs==0)?0:(int)(sleepTimeoutMs/60000UL);
  tmpSens    =encoderSensitivity;
  tmpLinux   =linuxLayout;
  tmpEspNow  =espNowEnabled;
  lastActivityMs=millis();

  delay(1500);
  drawMain();
}

// ════════════════════════════════════════════════
//  LOOP
// ════════════════════════════════════════════════
void loop(){
  updateEncoder();
  handleEncoderAction();

  // Handle remote media only if ESP-NOW is running
  if(espNowRunning) handleRemoteMedia();

  unsigned long now=millis();

  if(altHeld && altReleaseAt!=0 && now>=altReleaseAt){
    releaseAll(); altHeld=false; altReleaseAt=0;
    if(currentScreen==SCR_MAIN) drawMain();
  }

  if(bleConnected != lastBleConn){
    lastBleConn=bleConnected;
    if(bleConnected){
      if(wakeKeyPending && wakeKeyBtnIdx!=WAKEKEY_NONE){
        wakeKeyPending=false;
        delay(150);
        lastFlashBtn=wakeKeyBtnIdx; flashUntil=now+FLASH_MS;
        drawMain();
        fireAction(presets[activePreset].btns[wakeKeyBtnIdx].id);
        wakeKeyBtnIdx=WAKEKEY_NONE;
      } else { redraw(); }
    } else {
      if(wakeKeyPending && wakeKeyBtnIdx!=WAKEKEY_NONE)
        drawReconnectHUD(presets[activePreset].btns[wakeKeyBtnIdx].label);
      else redraw();
    }
  }

  if(!bleConnected && wakeKeyPending && wakeKeyBtnIdx!=WAKEKEY_NONE){
    static unsigned long lastHud=0;
    if(now-lastHud>400){
      lastHud=now;
      if(now-wakeTimeMs < RECONNECT_TIMEOUT_MS)
        drawReconnectHUD(presets[activePreset].btns[wakeKeyBtnIdx].label);
      else { wakeKeyPending=false; wakeKeyBtnIdx=WAKEKEY_NONE; redraw(); }
    }
  }

  if(lastFlashBtn>=0 && now>=flashUntil){
    lastFlashBtn=-1;
    if(currentScreen==SCR_MAIN) drawMain();
  }

  for(int i=0;i<8;i++){
    static bool lr[8]={};
    static unsigned long lc[8]={};
    static bool st[8]={};
    static unsigned long ps[8]={};
    bool r=(digitalRead(BTN_PINS[i])==LOW);
    if(r!=lr[i]){ lr[i]=r; lc[i]=now; }
    if((now-lc[i])>=BTN_DEBOUNCE_MS && r!=st[i]){
      st[i]=r;
      if(r) ps[i]=now;
      else if((now-ps[i])>=BTN_MIN_PRESS_MS){
        recordActivity();
        if(currentScreen==SCR_BUILD && i<NUM_PRESETS){ buildPreset=i; drawBuild(); }
        else onButtonPressed(i);
      }
    }
  }

  static unsigned long encDown=0;
  static bool encWas=false;
  static bool buildTriggered=false;
  static bool bondClearTriggered=false;
  bool encNow=(digitalRead(ENC_SW)==LOW);
  if(encNow && !encWas){ encDown=now; encWas=true; buildTriggered=false; bondClearTriggered=false; recordActivity(); }
  if(encNow && encWas && !bondClearTriggered && (now-encDown>3000)){
    bondClearTriggered=true;
    bleConnected=false;
    NimBLEDevice::deleteAllBonds();
    NimBLEDevice::stopAdvertising();
    delay(100);
    NimBLEDevice::startAdvertising();
    spr.fillSprite(C_BG);
    spr.fillRect(0,0,128,18,C_SURF);
    spr.drawFastHLine(0,18,128,C_RED);
    spr.setTextColor(C_RED,C_SURF); spr.setTextSize(1);
    spr.setCursor(6,5); spr.print("BONDS CLEARED");
    spr.setTextColor(C_WHITE,C_BG); spr.setCursor(6,48); spr.print("Now re-pair your");
    spr.setCursor(6,60); spr.print("device normally.");
    spr.setTextColor(C_CYAN,C_BG); spr.setCursor(6,80); spr.print("Advertising...");
    spr.pushSprite(0,0);
    delay(2500);
    redraw();
  }
  if(encNow && encWas && !buildTriggered && !bondClearTriggered && currentScreen==SCR_MAIN && (now-encDown>1500)){
    buildPreset=activePreset; buildCount=presets[buildPreset].btnCount;
    currentScreen=SCR_BUILD; drawBuild(); buildTriggered=true;
  }
  else if(!encNow && encWas){
    unsigned long held=now-encDown;
    if(!buildTriggered && !bondClearTriggered){
      if(held>=700)     onButtonPressed(9);
      else if(held>=40) onButtonPressed(8);
    }
    encWas=false;
  }

  maybeEnterSleep();
  delay(4);
}
