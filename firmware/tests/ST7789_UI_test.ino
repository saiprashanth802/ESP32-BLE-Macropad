/*********************************************************************
 * ST7789 240×320 UI Test v2 — MacroPad v5 style
 * Board  : NodeMCU ESP32S v1.1
 * Library: TFT_eSPI
 *
 * Rendering: direct TFT drawing (no full sprite — RAM constraint)
 * Small sprite (240×24) used only for status bar flicker-free update
 *
 * Wiring:
 *   SDA → P23 (MOSI)
 *   SCL → P18 (SCLK)
 *   CS  → P27
 *   DC  → P17
 *   RST → P16
 *   EN  → P19 (backlight PWM)
 *   VCC → 3.3V
 *   GND → GND
 *   TE  → leave unconnected
 *********************************************************************/

#include <TFT_eSPI.h>
#include <SPI.h>

// ── Palette ──────────────────────────────────────────
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

#define TFT_BL 19

TFT_eSPI tft = TFT_eSPI();

// Small sprite — only 240×24px = 11.5KB, always fits
TFT_eSprite sprBar = TFT_eSprite(&tft);

// Key sprite — 72×60px = 8.6KB, reused for each key
TFT_eSprite sprKey = TFT_eSprite(&tft);

int testScreen = 0;
unsigned long lastSwitch = 0;
const unsigned long SCREEN_DURATION = 4000;

// ════════════════════════════════════════════════════
//  STATUS BAR — uses sprBar sprite, flicker free
// ════════════════════════════════════════════════════
void drawStatusBar(uint16_t accent, const char* preset, bool connected) {
  sprBar.fillSprite(C_SURF);
  sprBar.drawFastHLine(0, 23, 240, accent);

  // BLE dot
  sprBar.fillCircle(10, 11, 5, connected ? C_GREEN : C_RED);
  sprBar.setTextColor(connected ? C_GREEN : C_RED, C_SURF);
  sprBar.setTextSize(1);
  sprBar.setCursor(18, 7);
  sprBar.print(connected ? "BLE" : "---");

  // Preset name centered
  sprBar.setTextColor(accent, C_SURF);
  int px = 120 - (strlen(preset) * 3);
  sprBar.setCursor(px, 7);
  sprBar.print(preset);

  // OS indicator
  sprBar.setTextColor(C_DIM, C_SURF);
  sprBar.setCursor(180, 7);
  sprBar.print("WIN");

  // Lock icon
  sprBar.fillRoundRect(214, 4, 14, 10, 2, C_YELLOW);
  sprBar.fillRect(216, 2, 8, 6, C_SURF);
  sprBar.fillRoundRect(217, 2, 6, 6, 2, C_YELLOW);

  sprBar.pushSprite(0, 0);
}

// ════════════════════════════════════════════════════
//  DRAW ONE KEY — uses sprKey, pushed at position
// ════════════════════════════════════════════════════
void drawKey(int col, int row, const char* label, uint16_t rowCol,
             bool flash, bool showNum, int num, int yStart) {
  int x = 4   + col * 78;
  int y = yStart + row * 66;

  uint16_t bg = flash ? rowCol : C_SURF2;
  uint16_t fg = flash ? C_BG   : C_WHITE;
  uint16_t br = flash ? C_WHITE : C_BORDER;

  sprKey.fillSprite(C_BG);
  sprKey.fillRoundRect(0, 0, 72, 60, 6, bg);
  sprKey.drawRoundRect(0, 0, 72, 60, 6, br);

  // key number
  if (showNum) {
    sprKey.setTextColor(flash ? bg : C_DIM, bg);
    sprKey.setTextSize(1);
    sprKey.setCursor(4, 4);
    sprKey.print(num);
  }

  // label centered
  sprKey.setTextColor(fg, bg);
  sprKey.setTextSize(1);
  int lx = max(2, (72 - (int)strlen(label) * 6) / 2);
  sprKey.setCursor(lx, 26);
  sprKey.print(label);

  // row colour dot
  sprKey.fillCircle(62, 6, 3, rowCol);

  sprKey.pushSprite(x, y);
}

// ════════════════════════════════════════════════════
//  SCREEN 1 — COLOUR PALETTE
// ════════════════════════════════════════════════════
void drawPaletteTest() {
  tft.fillScreen(C_BG);
  tft.setTextColor(C_WHITE, C_BG);
  tft.setTextSize(1);
  tft.setCursor(60, 6); tft.print("COLOUR PALETTE v5");

  uint16_t cols[] = {C_CYAN, C_AMBER, C_MAGENTA, C_GREEN,
                     C_RED, C_YELLOW, C_PINK, C_LTBLUE,
                     C_ORANGE, C_BUILD, C_WHITE, C_DIM};
  const char* names[] = {"CYAN","AMBER","MGNTA","GREEN",
                          "RED","YELLW","PINK","LTBLU",
                          "ORNGE","BUILD","WHITE","DIM"};

  for (int i = 0; i < 12; i++) {
    int col = i % 3;
    int row = i / 3;
    int x = 6  + col * 78;
    int y = 24 + row * 72;
    tft.fillRoundRect(x, y, 72, 64, 6, cols[i]);
    tft.setTextColor(C_BG, cols[i]);
    tft.setCursor(x + 6, y + 22); tft.print(names[i]);
  }

  tft.setTextColor(C_DIM, C_BG);
  tft.setCursor(44, 314); tft.print("Palette — screen 1/5");
}

// ════════════════════════════════════════════════════
//  SCREEN 2 — MAIN SCREEN
// ════════════════════════════════════════════════════
void drawMainScreen() {
  tft.fillScreen(C_BG);

  // dot grid
  for (int x = 6; x < 240; x += 10)
    for (int y = 26; y < 310; y += 10)
      tft.drawPixel(x, y, C_BORDER);

  drawStatusBar(C_CYAN, "ONSHAPE", true);

  // Encoder mode pill
  tft.fillRoundRect(16, 30, 208, 28, 10, C_SURF);
  tft.drawRoundRect(16, 30, 208, 28, 10, C_CYAN);
  tft.fillCircle(30, 44, 5, C_CYAN);
  tft.setTextColor(C_CYAN, C_SURF);
  tft.setTextSize(1);
  tft.setCursor(40, 40); tft.print("VOL  SCROLL  ZOOM  ALT-TAB");

  // Speaker icon
  int cx = 120, cy = 140;
  tft.fillRect(cx-22, cy-14, 12, 28, C_CYAN);
  tft.fillTriangle(cx-10,cy-22, cx-10,cy+22, cx+20,cy, C_CYAN);
  tft.drawFastHLine(cx+24, cy-12, 6, C_CYAN);
  tft.drawFastHLine(cx+24, cy,    6, C_CYAN);
  tft.drawFastHLine(cx+24, cy+12, 6, C_CYAN);

  // Hints
  tft.setTextColor(C_DIM, C_BG);
  tft.setCursor(20, 200); tft.print("Turn = VOL");
  tft.setCursor(20, 215); tft.print("Press = Lock/Unlock");
  tft.setCursor(20, 230); tft.print("Hold 0.7s = Settings");

  // Preset tabs
  tft.drawFastHLine(0, 268, 240, C_BORDER);
  const char* tabs[] = {"ONSH","KICA","MUSI","LTSP"};
  uint16_t tcols[] = {C_CYAN, C_GREEN, C_AMBER, C_MAGENTA};
  for (int i = 0; i < 4; i++) {
    int tx = 4 + i * 59;
    bool active = (i == 0);
    if (active) {
      tft.fillRoundRect(tx, 272, 54, 20, 3, C_CYAN);
      tft.setTextColor(C_BG, C_CYAN);
    } else {
      tft.drawRoundRect(tx, 272, 54, 20, 3, C_BORDER);
      tft.setTextColor(C_DIM, C_BG);
    }
    tft.setCursor(tx + 6, 277); tft.print(tabs[i]);
  }

  tft.setTextColor(C_DIM, C_BG);
  tft.setCursor(38, 314); tft.print("Main screen — 2/5");
}

// ════════════════════════════════════════════════════
//  SCREEN 3 — LOCKED KEY GRID
// ════════════════════════════════════════════════════
void drawLockedGrid() {
  tft.fillScreen(C_BG);
  drawStatusBar(C_CYAN, "ONSHAPE", true);

  // Locked banner
  tft.fillRect(0, 24, 240, 18, C_SURF);
  tft.drawFastHLine(0, 42, 240, C_BORDER);
  tft.setTextColor(C_YELLOW, C_SURF);
  tft.setTextSize(1);
  tft.setCursor(6, 30); tft.print("LOCKED  ");
  tft.setTextColor(C_CYAN, C_SURF); tft.print("VOL MODE");

  const char* labels[] = {
    "Fit","Front","Top",
    "Right","Iso","Extrude",
    "Sketch","Mate","Copy",
    "Paste","Undo","Save"
  };
  uint16_t rowcols[] = {C_CYAN, C_GREEN, C_AMBER, C_MAGENTA};

  for (int i = 0; i < 12; i++) {
    int col = i % 3;
    int row = i / 3;
    bool flash = (i == 2);
    drawKey(col, row, labels[i], rowcols[row], flash, true, i+1, 44);
  }

  tft.setTextColor(C_DIM, C_BG);
  tft.setCursor(38, 314); tft.print("Key grid — 3/5");
}

// ════════════════════════════════════════════════════
//  SCREEN 4 — SETTINGS GRID
// ════════════════════════════════════════════════════
void drawSettingsGrid() {
  tft.fillScreen(C_BG);
  drawStatusBar(C_CYAN, "SETTINGS", true);

  tft.fillRect(0, 24, 240, 18, C_SURF);
  tft.drawFastHLine(0, 42, 240, C_CYAN);
  tft.setTextColor(C_CYAN, C_SURF);
  tft.setTextSize(1);
  tft.setCursor(8, 30); tft.print("SETTINGS");
  tft.setTextColor(C_DIM, C_SURF);
  tft.setCursor(140, 30); tft.print("press to edit");

  const char* skeys[] = {
    "Bright","Sleep","WakeSw","Sens",
    "WhlCrv","OSLayo","BLERst","BtPrst",
    "Save","","Back","Exit"
  };
  uint16_t scols[] = {
    C_CYAN,C_CYAN,C_CYAN,C_CYAN,
    C_AMBER,C_AMBER,C_RED,C_AMBER,
    C_GREEN,C_DIM,C_ORANGE,C_RED
  };

  for (int i = 0; i < 12; i++) {
    int col = i % 3;
    int row = i / 3;
    bool sel = (i == 0);

    sprKey.fillSprite(C_BG);
    sprKey.fillRoundRect(0, 0, 72, 60, 6, sel ? C_SURF2 : C_SURF);
    sprKey.drawRoundRect(0, 0, 72, 60, 6, sel ? scols[i] : C_BORDER);
    sprKey.setTextColor(C_DIM, sel ? C_SURF2 : C_SURF);
    sprKey.setTextSize(1);
    sprKey.setCursor(4, 4); sprKey.print(i+1);
    sprKey.setTextColor(scols[i], sel ? C_SURF2 : C_SURF);
    int lx = max(2, (72 - (int)strlen(skeys[i]) * 6) / 2);
    sprKey.setCursor(lx, 26); sprKey.print(skeys[i]);
    sprKey.pushSprite(4 + col * 78, 44 + row * 66);
  }

  tft.setTextColor(C_DIM, C_BG);
  tft.setCursor(30, 314); tft.print("Settings grid — 4/5");
}

// ════════════════════════════════════════════════════
//  SCREEN 5 — FULL SCREEN EDITOR (brightness)
// ════════════════════════════════════════════════════
void drawEditor() {
  tft.fillScreen(C_BG);
  drawStatusBar(C_CYAN, "SETTINGS", true);

  tft.fillRect(0, 24, 240, 18, C_SURF);
  tft.drawFastHLine(0, 42, 240, C_CYAN);
  tft.setTextColor(C_CYAN, C_SURF);
  tft.setTextSize(1);
  tft.setCursor(8, 30); tft.print("<- BRIGHTNESS");

  // Big value
  tft.setTextSize(6);
  tft.setTextColor(C_CYAN, C_BG);
  tft.setCursor(36, 70); tft.print("180");

  tft.setTextSize(1);
  tft.setTextColor(C_DIM, C_BG);
  tft.setCursor(88, 150); tft.print("out of 255");

  // Bar
  tft.fillRoundRect(20, 175, 200, 24, 6, C_SURF2);
  int fillW = (int)(200 * (180.0 / 255.0));
  tft.fillRoundRect(20, 175, fillW, 24, 6, C_CYAN);
  tft.drawRoundRect(20, 175, 200, 24, 6, C_BORDER);

  // Min/max
  tft.setTextColor(C_DIM, C_BG);
  tft.setCursor(20, 205); tft.print("0");
  tft.setCursor(208, 205); tft.print("255");

  // Instructions box
  tft.fillRoundRect(10, 240, 220, 48, 6, C_SURF);
  tft.drawRoundRect(10, 240, 220, 48, 6, C_BORDER);
  tft.setTextColor(C_AMBER, C_SURF);
  tft.setCursor(20, 252); tft.print("Wheel = adjust");
  tft.setTextColor(C_GREEN, C_SURF);
  tft.setCursor(20, 268); tft.print("Key 1 = confirm back");

  tft.setTextColor(C_DIM, C_BG);
  tft.setCursor(36, 314); tft.print("Editor mock — 5/5");
}

// ════════════════════════════════════════════════════
//  SETUP
// ════════════════════════════════════════════════════
void setup() {
  Serial.begin(115200);

  // Backlight
  ledcAttach(TFT_BL, 5000, 8);
  ledcWrite(TFT_BL, 255);

  tft.init();
  tft.setRotation(0);
  tft.fillScreen(TFT_BLACK);

  // Status bar sprite — 240×24px, always fits
  sprBar.setColorDepth(16);
  sprBar.createSprite(240, 24);
  Serial.println("sprBar created: 240x24");

  // Key sprite — 72×60px, reused per key
  sprKey.setColorDepth(16);
  sprKey.createSprite(72, 60);
  Serial.println("sprKey created: 72x60");

  Serial.print("Free heap: ");
  Serial.println(ESP.getFreeHeap());
  Serial.println("UI test starting — 5 screens x 4s each");

  lastSwitch = millis() - SCREEN_DURATION; // draw immediately
}

// ════════════════════════════════════════════════════
//  LOOP
// ════════════════════════════════════════════════════
void loop() {
  if (millis() - lastSwitch >= SCREEN_DURATION) {
    lastSwitch = millis();

    Serial.print("Drawing screen ");
    Serial.print(testScreen + 1);
    Serial.println("/5");

    switch (testScreen) {
      case 0: drawPaletteTest();  break;
      case 1: drawMainScreen();   break;
      case 2: drawLockedGrid();   break;
      case 3: drawSettingsGrid(); break;
      case 4: drawEditor();       break;
    }

    testScreen = (testScreen + 1) % 5;
  }
}
