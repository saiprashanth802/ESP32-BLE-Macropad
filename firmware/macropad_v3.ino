/*********************************************************************
 * ESP32 BLE HID Macro Pad  —  v3
 *
 * Hardware:
 *   Encoder : CLK=2, DT=4, SW=15
 *   TFT     : CS=5, DC=17, RST=16, BL=12  (128×160 ST7735)
 *   Buttons : GPIO 13,25,26,21,19,22,14,35
 *
 * Features:
 *   ▸ 8 presets: OnShape, KiCad, Music, Gaming, LTspice, Custom×3
 *   ▸ BUILD MODE — lego-style: choose active button count (1-8),
 *     assign any action to each slot, live on-screen, no settings
 *   ▸ Encoder modes: VOL / SCROLL / ZOOM / ALT-TAB
 *   ▸ Alt held across turns, released after 1 s idle
 *   ▸ Reversed encoder direction
 *   ▸ Clean settings menu (brightness, sleep, sensitivity)
 *********************************************************************/

#include <BleKeyboard.h>
#include <TFT_eSPI.h>
#include <SPI.h>

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
#define C_BUILD    0x9F1F   // purple — BUILD mode accent

// Preset accent colours (one per preset)
const uint16_t PRESET_COL[8] = {
  C_CYAN,    // OnShape
  C_GREEN,   // KiCad
  C_AMBER,   // Music
  C_MAGENTA, // Gaming
  C_ORANGE,  // LTspice
  C_LTBLUE,  // Custom 2
  C_PINK,    // Custom 3
  C_YELLOW,  // Custom 4
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

const uint16_t  BTN_DEBOUNCE_MS  = 15;
const uint16_t  BTN_MIN_PRESS_MS = 40;
const uint8_t   ENC_DEBOUNCE_MS  = 10;
const unsigned long FLASH_MS     = 600;
const unsigned long ALT_HOLD_MS  = 1000;
const int ENC_DIR                = -1;   // reversed

// ════════════════════════════════════════════════
//  ACTION IDs
// ════════════════════════════════════════════════
// Media
#define A_PLAY       101
#define A_NEXT       102
#define A_PREV       103
#define A_MUTE       104
#define A_SPOTIFY    105
// System
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
// Edit
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
// OnShape specific
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
// KiCad specific
#define A_KC_ROUTE   150
#define A_KC_ADD_NET 151
#define A_KC_ZOOM_FIT 152
#define A_KC_DRC     153
#define A_KC_3D      154
#define A_KC_COPPER  155
#define A_KC_GERBER  156
#define A_KC_RATSNEST 157
// Gaming
#define A_PUSH_TO_TALK 160
#define A_RELOAD     161
#define A_MAP        162
#define A_SCORE      163
#define A_FULLSCREEN 164
#define A_OBS_REC    165
#define A_OBS_STREAM 166
#define A_DISCORD    167
// Browser / Web
#define A_NEW_TAB    170
#define A_CLOSE_TAB  171
#define A_RETAB      172
#define A_BACK       173
#define A_FORWARD    174
#define A_REFRESH    175
#define A_ADDR_BAR   176
// Volume
#define A_VOLUP      180
#define A_VOLDN      181
// LTspice
#define A_LTS_MOVE   190
#define A_LTS_GND    191
#define A_LTS_VCC    192
#define A_LTS_RES    193
#define A_LTS_CAP    194
#define A_LTS_COMP   195
#define A_LTS_WIRE   196
#define A_LTS_RUN    197

struct Action { const char* label; int id; };

const Action ACTION_LIB[] = {
  // ── Media ──────────────────────────
  {"Play/Pause", A_PLAY},
  {"Next Track", A_NEXT},
  {"Prev Track", A_PREV},
  {"Mute",       A_MUTE},
  {"Spotify",    A_SPOTIFY},
  {"Vol Up",     A_VOLUP},
  {"Vol Down",   A_VOLDN},
  // ── System ─────────────────────────
  {"Lock PC",    A_LOCK},
  {"Screenshot", A_SNIP},
  {"Full SS",    A_SS_FULL},
  {"Task View",  A_TASK},
  {"Min All",    A_MINALL},
  {"Desk Right", A_DESK_R},
  {"Desk Left",  A_DESK_L},
  {"Calculator", A_CALC},
  {"Explorer",   A_EXPLORER},
  {"Win Sett.",  A_SETTINGS_W},
  // ── Edit ───────────────────────────
  {"Undo",       A_UNDO},
  {"Redo",       A_REDO},
  {"Save",       A_SAVE},
  {"Copy",       A_COPY},
  {"Paste",      A_PASTE},
  {"Cut",        A_CUT},
  {"Select All", A_SELALL},
  {"Close Win",  A_CLOSE},
  {"Find",       A_FIND},
  {"Replace",    A_REPLACE},
  {"New File",   A_NEWFILE},
  {"Open File",  A_OPENFILE},
  // ── OnShape ────────────────────────
  {"OS: Fit",    A_OS_FIT},
  {"OS: Front",  A_OS_FRONT},
  {"OS: Top",    A_OS_TOP},
  {"OS: Right",  A_OS_RIGHT},
  {"OS: Iso",    A_OS_ISO},
  {"OS: ZFit",   A_OS_ZOOM_FIT},
  {"OS: Extrude",A_OS_EXTRUDE},
  {"OS: Sketch", A_OS_SKETCH},
  {"OS: Mate",   A_OS_MATE},
  {"OS: Assem.", A_OS_ASSEMBLY},
  // ── KiCad ──────────────────────────
  {"KC: Route",  A_KC_ROUTE},
  {"KC: AddNet", A_KC_ADD_NET},
  {"KC: ZFit",   A_KC_ZOOM_FIT},
  {"KC: DRC",    A_KC_DRC},
  {"KC: 3D",     A_KC_3D},
  {"KC: Copper", A_KC_COPPER},
  {"KC: Gerber", A_KC_GERBER},
  {"KC: Rats",   A_KC_RATSNEST},
  // ── Gaming ─────────────────────────
  {"PTT",        A_PUSH_TO_TALK},
  {"Reload",     A_RELOAD},
  {"Map",        A_MAP},
  {"Scoreboard", A_SCORE},
  {"Fullscreen", A_FULLSCREEN},
  {"OBS Rec",    A_OBS_REC},
  {"OBS Stream", A_OBS_STREAM},
  {"Discord",    A_DISCORD},
  // ── Browser ────────────────────────
  {"New Tab",    A_NEW_TAB},
  {"Close Tab",  A_CLOSE_TAB},
  {"Reopen Tab", A_RETAB},
  {"Back",       A_BACK},
  {"Forward",    A_FORWARD},
  {"Refresh",    A_REFRESH},
  {"Addr Bar",   A_ADDR_BAR},
  // ── LTspice ────────────────────────
  {"LTS Move",   A_LTS_MOVE},
  {"LTS GND",    A_LTS_GND},
  {"LTS VCC",    A_LTS_VCC},
  {"LTS Res",    A_LTS_RES},
  {"LTS Cap",    A_LTS_CAP},
  {"LTS Comp",   A_LTS_COMP},
  {"LTS Wire",   A_LTS_WIRE},
  {"LTS Run",    A_LTS_RUN},
};
const int ACTION_LIB_SIZE = sizeof(ACTION_LIB)/sizeof(Action);

// ════════════════════════════════════════════════
//  PRESET SYSTEM  — 8 presets, variable button count
// ════════════════════════════════════════════════
#define NUM_PRESETS      8
#define MAX_BTNS         8

struct ButtonAction {
  char label[9];
  int  id;
};

struct Preset {
  char         name[10];
  int          btnCount;
  ButtonAction btns[MAX_BTNS];
};

Preset presets[NUM_PRESETS] = {
  // 0 — OnShape (8 buttons)
  {"ONSHAPE", 8, {
    {"Fit",    A_OS_FIT},
    {"Front",  A_OS_FRONT},
    {"Top",    A_OS_TOP},
    {"Right",  A_OS_RIGHT},
    {"Iso",    A_OS_ISO},
    {"Extrude",A_OS_EXTRUDE},
    {"Sketch", A_OS_SKETCH},
    {"Mate",   A_OS_MATE},
  }},
  // 1 — KiCad (8 buttons)
  {"KICAD", 8, {
    {"Route",  A_KC_ROUTE},
    {"ZFit",   A_KC_ZOOM_FIT},
    {"DRC",    A_KC_DRC},
    {"3D",     A_KC_3D},
    {"Copper", A_KC_COPPER},
    {"Gerber", A_KC_GERBER},
    {"Rats",   A_KC_RATSNEST},
    {"AddNet", A_KC_ADD_NET},
  }},
  // 2 — Music (8 buttons)
  {"MUSIC", 6, {
    {"Play",   A_PLAY},
    {"Next",   A_NEXT},
    {"Prev",   A_PREV},
    {"Mute",   A_MUTE},
    {"Spotify",A_SPOTIFY},
    {"MinAll", A_MINALL},
    {"Lock",   A_LOCK},
    {"Find",   A_FIND},
  }},
  // 3 — LTspice (8 buttons)
  {"LTSPICE", 8, {
    {"Move",   A_LTS_MOVE},
    {"GND",    A_LTS_GND},
    {"VCC",    A_LTS_VCC},
    {"Res",    A_LTS_RES},
    {"Cap",    A_LTS_CAP},
    {"AddComp",A_LTS_COMP},
    {"Wire",   A_LTS_WIRE},
    {"Run",    A_LTS_RUN},
  }},

  // 4 — Gaming (8 buttons)
  {"GAMING", 8, {
    {"PTT",    A_PUSH_TO_TALK},
    {"Reload", A_RELOAD},
    {"Map",    A_MAP},
    {"Score",  A_SCORE},
    {"FullScr",A_FULLSCREEN},
    {"OBSRec", A_OBS_REC},
    {"OBSStr", A_OBS_STREAM},
    {"Discord",A_DISCORD},
  }},


  // 5 — Custom 2
  {"CUSTOM1", 4, {
    {"Lock",   A_LOCK},
    {"Snip",   A_SNIP},
    {"Task",   A_TASK},
    {"MinAll", A_MINALL},
    {"",0},{"",0},{"",0},{"",0},
  }},
  // 6 — Custom 3
  {"CUSTOM2", 2, {
    {"Save",   A_SAVE},
    {"Close",  A_CLOSE},
    {"",0},{"",0},{"",0},{"",0},{"",0},{"",0},
  }},
  // 7 — Custom 4
  {"CUSTOM3", 8, {
    {"Undo",   A_UNDO},
    {"Redo",   A_REDO},
    {"Save",   A_SAVE},
    {"Copy",   A_COPY},
    {"Paste",  A_PASTE},
    {"Find",   A_FIND},
    {"Rplace", A_REPLACE},
    {"SelAll", A_SELALL},
  }},
};

// ════════════════════════════════════════════════
//  SCREEN / STATE MACHINE
// ════════════════════════════════════════════════
enum Screen {
  SCR_MAIN,
  SCR_BUILD,
  SCR_BUILD_COUNT,
  SCR_BUILD_SLOT,
  SCR_BUILD_ACTION,
  SCR_SETTINGS,
};
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
const char* settingsItems[] = {"Brightness","Sleep(min)","Enc.Sens","Exit+Save"};
const int   SETTINGS_COUNT  = 4;

int  buildPreset  = 0;
int  buildCount   = 4;
int  buildSlot    = 0;
int  buildActSel  = 0;
int  buildActScroll=0;

long encPos      = 0;
long lastEncPos  = 0;
unsigned long lastEncTime = 0;
long menuAccum   = 0;

bool lastBtConn  = false;

// ════════════════════════════════════════════════
//  BLE + TFT
// ════════════════════════════════════════════════
BleKeyboard bleKeyboard("ESP32 MacroPad", "DIY", 100);
TFT_eSPI    tft = TFT_eSPI();
TFT_eSprite spr = TFT_eSprite(&tft);

// ════════════════════════════════════════════════
//  FORWARD DECLARATIONS
// ════════════════════════════════════════════════
void drawMain();
void drawBuild();
void drawBuildCount();
void drawBuildSlot();
void drawBuildAction();
void drawSettings();
void redraw();
void fireAction(int id);
void onButtonPressed(int idx);
void handleEncoderAction();

// ════════════════════════════════════════════════
//  ENCODER READ
// ════════════════════════════════════════════════
static const int8_t ENC_TABLE[] = {0,-1,1,0,1,0,0,-1,-1,0,0,1,0,1,-1,0};
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
//  STATUS BAR
// ════════════════════════════════════════════════
void drawStatusBar(uint16_t accent=C_CYAN) {
  spr.fillRect(0,0,128,18,C_SURF);
  spr.drawFastHLine(0,18,128,C_BORDER);

  bool conn=bleKeyboard.isConnected();
  spr.fillCircle(6,9,4,conn?C_GREEN:C_RED);
  spr.setTextSize(1);
  spr.setTextColor(conn?C_GREEN:C_RED,C_SURF);
  spr.setCursor(13,5); spr.print(conn?"BLE":"---");

  spr.setTextColor(accent,C_SURF);
  int nx=44-(strlen(presets[activePreset].name)*3);
  spr.setCursor(max(36,nx),5);
  spr.print(presets[activePreset].name);

  if(locked){
    spr.fillRoundRect(108,3,12,9,2,C_YELLOW);
    spr.fillRect(110,1,6,5,C_BG);
    spr.fillRoundRect(111,1,4,5,2,C_YELLOW);
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
    int page = activePreset/4;
    for(int p=0;p<4;p++){
      int idx = page*4+p;
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

    int cnt = presets[activePreset].btnCount;

    int rows  = (cnt+1)/2;
    int bw=58, bh=0;
    int totalGap=(rows-1)*4;
    bh = (125-totalGap)/rows;
    bh = min(bh,32);

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
      spr.setCursor(lx, y+(bh/2)-3);
      spr.print(lbl);
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
  spr.setTextColor(C_DIM,C_SURF);
  spr.setCursor(76,26); spr.print("LEGO");

  spr.setTextColor(C_WHITE,C_BG); spr.setCursor(6,46); spr.print("Editing:");
  spr.setTextColor(PRESET_COL[buildPreset],C_BG);
  spr.setCursor(6,58); spr.print(presets[buildPreset].name);

  spr.setTextColor(C_DIM,C_BG);
  spr.setCursor(6,74); spr.print("Active slots:");
  spr.setTextColor(C_WHITE,C_BG);
  spr.setCursor(84,74); spr.print(presets[buildPreset].btnCount);

  int pc=presets[buildPreset].btnCount;
  for(int i=0;i<pc && i<6;i++){
    int px=6+(i%3)*40, py=86+(i/3)*16;
    spr.fillRoundRect(px,py,36,13,3,C_SURF2);
    spr.setTextColor(C_WHITE,C_SURF2);
    spr.setCursor(px+3,py+3);
    char tmp[6]; strncpy(tmp,presets[buildPreset].btns[i].label,5); tmp[5]='\0';
    spr.print(tmp);
  }
  if(pc>6){ spr.setTextColor(C_DIM,C_BG); spr.setCursor(6,118); spr.print("+more..."); }

  spr.drawFastHLine(0,130,128,C_BORDER);
  spr.setTextColor(C_DIM,C_BG); spr.setCursor(6,136); spr.print("Turn=pick preset");
  spr.setTextColor(C_BUILD,C_BG); spr.setCursor(6,148); spr.print("Press=START EDITING");

  spr.pushSprite(0,0);
}

// ════════════════════════════════════════════════
//  DRAW BUILD COUNT
// ════════════════════════════════════════════════
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
  int bx=64-(strlen(buf)*12);
  spr.setCursor(bx,58); spr.print(buf);
  spr.setTextSize(1);

  spr.setTextColor(C_DIM,C_BG); spr.setCursor(28,100); spr.print("buttons active");

  for(int i=0;i<MAX_BTNS;i++){
    int dx=18+i*12, dy=116;
    if(i<buildCount) spr.fillCircle(dx,dy,5,C_BUILD);
    else             spr.drawCircle(dx,dy,5,C_BORDER);
  }

  spr.drawFastHLine(0,128,128,C_BORDER);
  spr.setTextColor(C_DIM,C_BG); spr.setCursor(6,134); spr.print("Layout preview:");
  int rows=(buildCount+1)/2;
  int bw=24,bh=8,gap=2;
  for(int i=0;i<buildCount;i++){
    int col=i%2, row=i/2;
    int px=46+col*(bw+3);
    int py=134+row*(bh+gap);
    spr.fillRoundRect(px,py,bw,bh,2,C_SURF2);
  }

  spr.setTextColor(C_BUILD,C_BG); spr.setCursor(6,154); spr.print("Press=confirm");

  spr.pushSprite(0,0);
}

// ════════════════════════════════════════════════
//  DRAW BUILD SLOT
// ════════════════════════════════════════════════
void drawBuildSlot() {
  spr.fillSprite(C_BG);
  drawStatusBar(C_BUILD);

  spr.fillRect(0,20,128,18,C_SURF);
  spr.drawFastHLine(0,38,128,C_BUILD);
  spr.setTextColor(C_BUILD,C_SURF); spr.setTextSize(1);
  spr.setCursor(6,26); spr.print("STEP 2: PICK SLOT");

  int cnt=buildCount;
  int rows=(cnt+1)/2;
  int bw=56,bh=0;
  int totalGap=(rows-1)*4;
  bh=min(32,(120-totalGap)/rows);

  for(int i=0;i<cnt;i++){
    int col=i%2, row=i/2;
    int x=4+col*(bw+6);
    int y=42+row*(bh+4);
    bool sel=(i==buildSlot);
    uint16_t bg=sel?C_BUILD:C_SURF;
    uint16_t fg=sel?C_WHITE:C_DIM;
    spr.fillRoundRect(x,y,bw,bh,4,bg);
    spr.drawRoundRect(x,y,bw,bh,4,sel?C_WHITE:C_BORDER);
    spr.setTextColor(sel?C_BG:C_DIM,bg); spr.setCursor(x+3,y+2); spr.print(i+1);
    spr.setTextColor(fg,bg);
    const char* lbl=presets[buildPreset].btns[i].label;
    int lx=x+max(0,(bw-(int)strlen(lbl)*6)/2);
    spr.setCursor(lx,y+(bh/2)-3); spr.print(lbl);
  }

  spr.setTextColor(C_BUILD,C_BG);
  spr.setCursor(6,155); spr.print("Press=assign action");

  spr.pushSprite(0,0);
}

// ════════════════════════════════════════════════
//  DRAW BUILD ACTION
// ════════════════════════════════════════════════
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
    if(sel){
      spr.fillRoundRect(2,ry,116,17,3,C_BUILD);
      spr.setTextColor(C_WHITE,C_BUILD);
    } else {
      spr.setTextColor(C_DIM,C_BG);
    }
    spr.setCursor(8,ry+5); spr.print(ACTION_LIB[idx].label);
    if(sel){ spr.setTextColor(C_WHITE,C_BUILD); spr.setCursor(110,ry+5); spr.print(">"); }
  }

  int sbH=6*19;
  int sbPos=40+(buildActSel*(sbH-8))/max(1,ACTION_LIB_SIZE-1);
  spr.fillRect(122,40,4,sbH,C_BORDER);
  spr.fillRect(122,sbPos,4,8,C_BUILD);

  spr.setTextColor(C_BUILD,C_BG); spr.setCursor(6,156); spr.print("Press=assign  Hold=back");

  spr.pushSprite(0,0);
}

// ════════════════════════════════════════════════
//  DRAW SETTINGS
// ════════════════════════════════════════════════
void drawSettings() {
  spr.fillSprite(C_BG);
  drawStatusBar(C_CYAN);

  spr.fillRect(0,20,128,18,C_SURF);
  spr.drawFastHLine(0,38,128,C_CYAN);
  spr.setTextColor(C_CYAN,C_SURF); spr.setTextSize(1);
  spr.setCursor(6,26); spr.print("SETTINGS");
  spr.setTextColor(C_DIM,C_SURF); spr.setCursor(76,26); spr.print("enc=nav");

  for(int i=0;i<SETTINGS_COUNT;i++){
    bool sel=(i==settingsSel);
    int ry=44+i*27;
    if(sel){ spr.fillRoundRect(2,ry,124,23,4,C_SURF2); spr.drawRoundRect(2,ry,124,23,4,C_CYAN); }
    spr.setTextColor(sel?C_WHITE:C_DIM, sel?C_SURF2:C_BG);
    spr.setCursor(10,ry+8); spr.print(settingsItems[i]);

    char vb[10];
    spr.setTextColor(sel?C_CYAN:C_BORDER, sel?C_SURF2:C_BG);
    if(i==0){ sprintf(vb,"%d",tmpBright); spr.setCursor(96,ry+8); spr.print(vb); }
    else if(i==1){ sprintf(vb,"%dmin",tmpSleepMin); spr.setCursor(88,ry+8); spr.print(vb); }
    else if(i==2){ sprintf(vb,"%d",tmpSens); spr.setCursor(104,ry+8); spr.print(vb); }
    else if(i==3){ spr.setTextColor(C_GREEN,sel?C_SURF2:C_BG); spr.setCursor(96,ry+8); spr.print("SAVE"); }

    if(sel && settingsAdj && i<3){ spr.setTextColor(C_AMBER,C_SURF2); spr.setCursor(116,ry+8); spr.print("<"); }
  }

  spr.setTextColor(C_DIM,C_BG); spr.setCursor(2,157); spr.print("Press=select  Hold=back");
  spr.pushSprite(0,0);
}

// ════════════════════════════════════════════════
//  REDRAW DISPATCHER
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
//  FIRE ACTION
// ════════════════════════════════════════════════
void fireAction(int id){
  if(!bleKeyboard.isConnected()) return;
  switch(id){
    case A_PLAY:    bleKeyboard.write(KEY_MEDIA_PLAY_PAUSE); break;
    case A_NEXT:    bleKeyboard.write(KEY_MEDIA_NEXT_TRACK); break;
    case A_PREV:    bleKeyboard.write(KEY_MEDIA_PREVIOUS_TRACK); break;
    case A_MUTE:    bleKeyboard.write(KEY_MEDIA_MUTE); break;
    case A_VOLUP:   bleKeyboard.write(KEY_MEDIA_VOLUME_UP); break;
    case A_VOLDN:   bleKeyboard.write(KEY_MEDIA_VOLUME_DOWN); break;
    case A_SPOTIFY:
      bleKeyboard.press(KEY_LEFT_GUI); bleKeyboard.press('3'); delay(30); bleKeyboard.releaseAll(); break;
    case A_LOCK:
      bleKeyboard.press(KEY_LEFT_GUI); bleKeyboard.press('l'); delay(30); bleKeyboard.releaseAll(); break;
    case A_SNIP:
      bleKeyboard.press(KEY_LEFT_GUI); bleKeyboard.press(KEY_LEFT_SHIFT); bleKeyboard.press('s'); delay(30); bleKeyboard.releaseAll(); break;
    case A_SS_FULL:
      bleKeyboard.press(KEY_LEFT_GUI); bleKeyboard.press(KEY_PRTSC); delay(30); bleKeyboard.releaseAll(); break;
    case A_TASK:
      bleKeyboard.press(KEY_LEFT_GUI); bleKeyboard.press(KEY_TAB); delay(30); bleKeyboard.releaseAll(); break;
    case A_MINALL:
      bleKeyboard.press(KEY_LEFT_GUI); bleKeyboard.press('m'); delay(30); bleKeyboard.releaseAll(); break;
    case A_DESK_R:
      bleKeyboard.press(KEY_LEFT_CTRL); bleKeyboard.press(KEY_LEFT_GUI); bleKeyboard.press(KEY_RIGHT_ARROW); delay(30); bleKeyboard.releaseAll(); break;
    case A_DESK_L:
      bleKeyboard.press(KEY_LEFT_CTRL); bleKeyboard.press(KEY_LEFT_GUI); bleKeyboard.press(KEY_LEFT_ARROW); delay(30); bleKeyboard.releaseAll(); break;
    case A_CALC:
      bleKeyboard.press(KEY_LEFT_GUI); bleKeyboard.press('r'); delay(200); bleKeyboard.releaseAll(); delay(300); bleKeyboard.print("calc\n"); break;
    case A_EXPLORER:
      bleKeyboard.press(KEY_LEFT_GUI); bleKeyboard.press('e'); delay(30); bleKeyboard.releaseAll(); break;
    case A_SETTINGS_W:
      bleKeyboard.press(KEY_LEFT_GUI); bleKeyboard.press('i'); delay(30); bleKeyboard.releaseAll(); break;
    case A_UNDO:
      bleKeyboard.press(KEY_LEFT_CTRL); bleKeyboard.press('z'); delay(30); bleKeyboard.releaseAll(); break;
    case A_REDO:
      bleKeyboard.press(KEY_LEFT_CTRL); bleKeyboard.press('y'); delay(30); bleKeyboard.releaseAll(); break;
    case A_SAVE:
      bleKeyboard.press(KEY_LEFT_CTRL); bleKeyboard.press('s'); delay(30); bleKeyboard.releaseAll(); break;
    case A_COPY:
      bleKeyboard.press(KEY_LEFT_CTRL); bleKeyboard.press('c'); delay(30); bleKeyboard.releaseAll(); break;
    case A_PASTE:
      bleKeyboard.press(KEY_LEFT_CTRL); bleKeyboard.press('v'); delay(30); bleKeyboard.releaseAll(); break;
    case A_CUT:
      bleKeyboard.press(KEY_LEFT_CTRL); bleKeyboard.press('x'); delay(30); bleKeyboard.releaseAll(); break;
    case A_SELALL:
      bleKeyboard.press(KEY_LEFT_CTRL); bleKeyboard.press('a'); delay(30); bleKeyboard.releaseAll(); break;
    case A_CLOSE:
      bleKeyboard.press(KEY_LEFT_ALT); bleKeyboard.press(KEY_F4); delay(30); bleKeyboard.releaseAll(); break;
    case A_FIND:
      bleKeyboard.press(KEY_LEFT_CTRL); bleKeyboard.press('f'); delay(30); bleKeyboard.releaseAll(); break;
    case A_REPLACE:
      bleKeyboard.press(KEY_LEFT_CTRL); bleKeyboard.press('h'); delay(30); bleKeyboard.releaseAll(); break;
    case A_NEWFILE:
      bleKeyboard.press(KEY_LEFT_CTRL); bleKeyboard.press('n'); delay(30); bleKeyboard.releaseAll(); break;
    case A_OPENFILE:
      bleKeyboard.press(KEY_LEFT_CTRL); bleKeyboard.press('o'); delay(30); bleKeyboard.releaseAll(); break;
    case A_OS_FIT:   bleKeyboard.write('f'); break;
    case A_OS_FRONT: bleKeyboard.press(KEY_LEFT_SHIFT); bleKeyboard.press('1'); delay(30); bleKeyboard.releaseAll(); break;
    case A_OS_TOP:   bleKeyboard.press(KEY_LEFT_SHIFT); bleKeyboard.press('2'); delay(30); bleKeyboard.releaseAll(); break;
    case A_OS_RIGHT: bleKeyboard.press(KEY_LEFT_SHIFT); bleKeyboard.press('3'); delay(30); bleKeyboard.releaseAll(); break;
    case A_OS_ISO:   bleKeyboard.press(KEY_LEFT_SHIFT); bleKeyboard.press('7'); delay(30); bleKeyboard.releaseAll(); break;
    case A_OS_ZOOM_FIT:
      bleKeyboard.press(KEY_LEFT_CTRL); bleKeyboard.press(KEY_LEFT_SHIFT); bleKeyboard.press('f'); delay(30); bleKeyboard.releaseAll(); break;
    case A_OS_EXTRUDE: bleKeyboard.press(KEY_LEFT_SHIFT); bleKeyboard.press('e'); delay(30); bleKeyboard.releaseAll(); break;
    case A_OS_SKETCH:  bleKeyboard.write('s'); bleKeyboard.releaseAll(); break;
    case A_OS_MATE:
      bleKeyboard.press(KEY_LEFT_CTRL); bleKeyboard.press('m'); delay(30); bleKeyboard.releaseAll(); break;
    case A_OS_ASSEMBLY:
      bleKeyboard.press(KEY_LEFT_ALT); bleKeyboard.press('a'); delay(30); bleKeyboard.releaseAll(); break;
    case A_KC_ROUTE:    bleKeyboard.write('x'); break;
    case A_KC_ADD_NET:  bleKeyboard.write('w'); break;
    case A_KC_ZOOM_FIT: bleKeyboard.write('5'); break;
    case A_KC_DRC:
      bleKeyboard.press(KEY_LEFT_ALT); bleKeyboard.press('3'); delay(30); bleKeyboard.releaseAll(); break;
    case A_KC_3D:       bleKeyboard.write('3'); break;
    case A_KC_COPPER:
      bleKeyboard.press(KEY_LEFT_CTRL); bleKeyboard.press('k'); delay(30); bleKeyboard.releaseAll(); break;
    case A_KC_GERBER:
      bleKeyboard.press(KEY_LEFT_CTRL); bleKeyboard.press('p'); delay(30); bleKeyboard.releaseAll(); break;
    case A_KC_RATSNEST: bleKeyboard.write('`'); break;
    case A_PUSH_TO_TALK: bleKeyboard.write('v'); break;
    case A_RELOAD:       bleKeyboard.write('r'); break;
    case A_MAP:          bleKeyboard.write('m'); break;
    case A_SCORE:        bleKeyboard.write(KEY_TAB); break;
    case A_FULLSCREEN:   bleKeyboard.write(KEY_F11); break;
    case A_OBS_REC:
      bleKeyboard.press(KEY_LEFT_CTRL); bleKeyboard.press(KEY_LEFT_ALT); bleKeyboard.press('r'); delay(30); bleKeyboard.releaseAll(); break;
    case A_OBS_STREAM:
      bleKeyboard.press(KEY_LEFT_CTRL); bleKeyboard.press(KEY_LEFT_ALT); bleKeyboard.press('s'); delay(30); bleKeyboard.releaseAll(); break;
    case A_DISCORD:
      bleKeyboard.press(KEY_LEFT_CTRL); bleKeyboard.press(KEY_LEFT_ALT); bleKeyboard.press('d'); delay(30); bleKeyboard.releaseAll(); break;
    case A_NEW_TAB:
      bleKeyboard.press(KEY_LEFT_CTRL); bleKeyboard.press('t'); delay(30); bleKeyboard.releaseAll(); break;
    case A_CLOSE_TAB:
      bleKeyboard.press(KEY_LEFT_CTRL); bleKeyboard.press('w'); delay(30); bleKeyboard.releaseAll(); break;
    case A_RETAB:
      bleKeyboard.press(KEY_LEFT_CTRL); bleKeyboard.press(KEY_LEFT_SHIFT); bleKeyboard.press('t'); delay(30); bleKeyboard.releaseAll(); break;
    case A_BACK:
      bleKeyboard.press(KEY_LEFT_ALT); bleKeyboard.press(KEY_LEFT_ARROW); delay(30); bleKeyboard.releaseAll(); break;
    case A_FORWARD:
      bleKeyboard.press(KEY_LEFT_ALT); bleKeyboard.press(KEY_RIGHT_ARROW); delay(30); bleKeyboard.releaseAll(); break;
    case A_REFRESH:
      bleKeyboard.press(KEY_LEFT_CTRL); bleKeyboard.press('r'); delay(30); bleKeyboard.releaseAll(); break;
    case A_ADDR_BAR:
      bleKeyboard.press(KEY_LEFT_CTRL); bleKeyboard.press('l'); delay(30); bleKeyboard.releaseAll(); break;

    // ── LTspice ──────────────────────────────────
    case A_LTS_MOVE:
      bleKeyboard.write('m'); break;          // Move component
    case A_LTS_GND:
      bleKeyboard.write('g'); break;          // Place ground
    case A_LTS_VCC:
      bleKeyboard.write('v'); break;          // Place power pin (VCC)
    case A_LTS_RES:
      bleKeyboard.write('r'); break;          // Place resistor
    case A_LTS_CAP:
      bleKeyboard.write('c'); break;          // Place capacitor
    case A_LTS_COMP:
      bleKeyboard.write('p'); break;          // Open component browser
    case A_LTS_WIRE:
      bleKeyboard.write('w'); break;          // Draw wire
    case A_LTS_RUN:
      bleKeyboard.write(KEY_LEFT_ALT); bleKeyboard.press('r'); delay(30); bleKeyboard.releaseAll(); break;       // Run simulation
  }
}

// ════════════════════════════════════════════════
//  ENCODER ACTION HANDLER
// ════════════════════════════════════════════════
void handleEncoderAction(){
  if(encPos==lastEncPos) return;
  long delta=encPos-lastEncPos;
  lastEncPos=encPos;

  auto menuStep=[&](int thresh=2)->int{
    menuAccum+=delta;
    if(menuAccum>=thresh){ menuAccum=0; return 1; }
    if(menuAccum<=-thresh){ menuAccum=0; return -1; }
    return 0;
  };

  if(currentScreen==SCR_SETTINGS){
    int s=menuStep();
    if(!s) return;
    if(settingsAdj){
      if(settingsSel==0) tmpBright  =constrain(tmpBright  +s*10,0,255);
      if(settingsSel==1) tmpSleepMin=constrain(tmpSleepMin+s,1,60);
      if(settingsSel==2) tmpSens    =constrain(tmpSens    +s,1,8);
    } else {
      settingsSel=constrain(settingsSel+s,0,SETTINGS_COUNT-1);
    }
    drawSettings(); return;
  }

  if(currentScreen==SCR_BUILD){
    int s=menuStep();
    if(!s) return;
    buildPreset=constrain(buildPreset+s,0,NUM_PRESETS-1);
    drawBuild(); return;
  }

  if(currentScreen==SCR_BUILD_COUNT){
    int s=menuStep();
    if(!s) return;
    buildCount=constrain(buildCount+s,1,MAX_BTNS);
    drawBuildCount(); return;
  }

  if(currentScreen==SCR_BUILD_SLOT){
    int s=menuStep();
    if(!s) return;
    buildSlot=constrain(buildSlot+s,0,buildCount-1);
    drawBuildSlot(); return;
  }

  if(currentScreen==SCR_BUILD_ACTION){
    int s=menuStep();
    if(!s) return;
    buildActSel=constrain(buildActSel+s,0,ACTION_LIB_SIZE-1);
    const int VIS=6;
    if(buildActSel<buildActScroll) buildActScroll=buildActSel;
    if(buildActSel>=buildActScroll+VIS) buildActScroll=buildActSel-VIS+1;
    drawBuildAction(); return;
  }

  if(currentScreen==SCR_MAIN && !locked){
    int s=menuStep(5);
    if(!s) return;
    encMode=(encMode+s+_MODE_COUNT)%_MODE_COUNT;
    drawMain(); return;
  }

  if(currentScreen==SCR_MAIN && locked){
    if(!bleKeyboard.isConnected()) return;
    static long acc=0;
    acc+=delta;
    if(abs(acc)<encoderSensitivity) return;
    int steps=acc/encoderSensitivity;
    acc%=encoderSensitivity;
    int dir=(steps>0)?1:-1;
    int n=abs(steps);

    switch(encMode){
      case MODE_VOLUME:
        for(int i=0;i<n;i++){ bleKeyboard.write(dir>0?KEY_MEDIA_VOLUME_UP:KEY_MEDIA_VOLUME_DOWN); delay(10); }
        break;
      case MODE_SCROLL:
        for(int i=0;i<n;i++){ bleKeyboard.write(dir>0?KEY_DOWN_ARROW:KEY_UP_ARROW); delay(6); }
        break;
      case MODE_ZOOM:
        bleKeyboard.press(KEY_LEFT_CTRL);
        for(int i=0;i<n;i++){ bleKeyboard.write(dir>0?'+':'-'); delay(6); }
        bleKeyboard.releaseAll();
        break;
      case MODE_ALTTAB:
        if(!altHeld){ bleKeyboard.press(KEY_LEFT_ALT); altHeld=true; delay(60); }
        for(int i=0;i<n;i++){
          if(dir>0){ bleKeyboard.press(KEY_TAB); delay(40); bleKeyboard.release(KEY_TAB); }
          else     { bleKeyboard.press(KEY_LEFT_SHIFT); bleKeyboard.press(KEY_TAB); delay(40); bleKeyboard.release(KEY_TAB); bleKeyboard.release(KEY_LEFT_SHIFT); }
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
  if(idx==9){
    if(currentScreen==SCR_MAIN){
      tmpBright=backlightBrightness;
      tmpSleepMin=(int)(sleepTimeoutMs/60000UL);
      tmpSens=encoderSensitivity;
      settingsSel=0; settingsAdj=false;
      currentScreen=SCR_SETTINGS; drawSettings();
    } else if(currentScreen==SCR_SETTINGS){
      currentScreen=SCR_MAIN; drawMain();
    } else if(currentScreen==SCR_BUILD){
      currentScreen=SCR_MAIN; drawMain();
    } else if(currentScreen==SCR_BUILD_COUNT){
      currentScreen=SCR_BUILD; drawBuild();
    } else if(currentScreen==SCR_BUILD_SLOT){
      currentScreen=SCR_BUILD_COUNT; drawBuildCount();
    } else if(currentScreen==SCR_BUILD_ACTION){
      currentScreen=SCR_BUILD_SLOT; drawBuildSlot();
    }
    return;
  }

  if(currentScreen==SCR_SETTINGS){
    if(idx!=8) return;
    if(settingsSel==SETTINGS_COUNT-1){
      backlightBrightness=tmpBright; ledcWrite(TFT_BL,backlightBrightness);
      sleepTimeoutMs=(unsigned long)tmpSleepMin*60000UL;
      encoderSensitivity=tmpSens;
      settingsAdj=false; currentScreen=SCR_MAIN; drawMain();
    } else {
      settingsAdj=!settingsAdj;
      if(!settingsAdj && settingsSel==0){ backlightBrightness=tmpBright; ledcWrite(TFT_BL,backlightBrightness); }
      drawSettings();
    }
    return;
  }

  if(currentScreen==SCR_BUILD){
    if(idx!=8) return;
    buildCount=presets[buildPreset].btnCount;
    currentScreen=SCR_BUILD_COUNT; drawBuildCount();
    return;
  }

  if(currentScreen==SCR_BUILD_COUNT){
    if(idx!=8) return;
    presets[buildPreset].btnCount=buildCount;
    buildSlot=0;
    currentScreen=SCR_BUILD_SLOT; drawBuildSlot();
    return;
  }

  if(currentScreen==SCR_BUILD_SLOT){
    if(idx!=8) return;
    buildActSel=0; buildActScroll=0;
    currentScreen=SCR_BUILD_ACTION; drawBuildAction();
    return;
  }

  if(currentScreen==SCR_BUILD_ACTION){
    if(idx!=8) return;
    strncpy(presets[buildPreset].btns[buildSlot].label, ACTION_LIB[buildActSel].label, 8);
    presets[buildPreset].btns[buildSlot].label[8]='\0';
    presets[buildPreset].btns[buildSlot].id=ACTION_LIB[buildActSel].id;
    if(buildSlot<buildCount-1){
      buildSlot++;
      buildActSel=0; buildActScroll=0;
      currentScreen=SCR_BUILD_SLOT; drawBuildSlot();
    } else {
      activePreset=buildPreset;
      currentScreen=SCR_MAIN; drawMain();
    }
    return;
  }

  if(idx==8){
    locked=!locked;
    if(!locked){ bleKeyboard.releaseAll(); altHeld=false; altReleaseAt=0; }
    drawMain(); return;
  }

  if(!locked){
    activePreset=idx;
    drawMain(); return;
  }

  if(idx < presets[activePreset].btnCount && bleKeyboard.isConnected()){
    lastFlashBtn=idx; flashUntil=millis()+FLASH_MS;
    drawMain();
    fireAction(presets[activePreset].btns[idx].id);
  }
}

// ════════════════════════════════════════════════
//  SETUP
// ════════════════════════════════════════════════
void setup(){
  Serial.begin(115200);

  for(int i=0;i<8;i++) pinMode(BTN_PINS[i],INPUT_PULLUP);
  pinMode(ENC_SW,INPUT_PULLUP);
  pinMode(ENC_CLK,INPUT_PULLUP);
  pinMode(ENC_DT,INPUT_PULLUP);

  ledcAttach(TFT_BL,5000,8);
  ledcWrite(TFT_BL,backlightBrightness);

  tft.init();
  tft.setRotation(2);
  spr.setColorDepth(16);
  spr.createSprite(128,160);

  spr.fillSprite(C_BG);
  spr.setTextColor(C_CYAN,C_BG); spr.setTextSize(2);
  spr.setCursor(10,30); spr.print("MACRO");
  spr.setCursor(10,52); spr.print("PAD v3");
  spr.setTextSize(1);
  spr.setTextColor(C_BUILD,C_BG); spr.setCursor(10,80); spr.print("BUILD MODE");
  spr.setTextColor(C_DIM,C_BG);
  spr.setCursor(10,95);  spr.print("8 Presets");
  spr.setCursor(10,107); spr.print("OnShape  KiCad");
  spr.setCursor(10,119); spr.print("Music    Gaming");
  spr.setCursor(10,131); spr.print("LTspice  Custom x3");
  spr.pushSprite(0,0);

  bleKeyboard.begin();
  delay(2000);

  tmpBright=backlightBrightness;
  tmpSleepMin=(int)(sleepTimeoutMs/60000UL);
  tmpSens=encoderSensitivity;

  drawMain();
}

// ════════════════════════════════════════════════
//  LOOP
// ════════════════════════════════════════════════
void loop(){
  updateEncoder();
  handleEncoderAction();

  unsigned long now=millis();

  if(altHeld && altReleaseAt!=0 && now>=altReleaseAt){
    bleKeyboard.release(KEY_LEFT_ALT);
    bleKeyboard.release(KEY_LEFT_SHIFT);
    altHeld=false; altReleaseAt=0;
    if(currentScreen==SCR_MAIN) drawMain();
  }

  bool conn=bleKeyboard.isConnected();
  if(conn!=lastBtConn){ lastBtConn=conn; redraw(); }

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
        if(currentScreen==SCR_BUILD && i<NUM_PRESETS){ buildPreset=i; drawBuild(); }
        else onButtonPressed(i);
      }
    }
  }

  static unsigned long encDown = 0;
  static bool encWas = false;
  static bool buildTriggered = false;

  bool encNow = (digitalRead(ENC_SW) == LOW);

  if(encNow && !encWas){
    encDown = now;
    encWas = true;
    buildTriggered = false;
  }

  if(encNow && encWas){
    if(!buildTriggered && currentScreen == SCR_MAIN && (now - encDown > 1500)){
      buildPreset = activePreset;
      buildCount  = presets[buildPreset].btnCount;
      currentScreen = SCR_BUILD;
      drawBuild();
      buildTriggered = true;
    }
  }

  else if(!encNow && encWas){
    unsigned long held = now - encDown;
    if(!buildTriggered){
      if(held >= 700)     onButtonPressed(9);
      else if(held >= 40) onButtonPressed(8);
    }
    encWas = false;
  }

  delay(4);
}