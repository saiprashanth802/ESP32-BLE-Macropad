/*********************************************************************
 * AS5600 Media Remote — Fixed version
 * No external pullups needed — uses internal pullup pins
 *
 * Hardware:
 *   AS5600 SDA → GPIO21
 *   AS5600 SCL → GPIO22
 *   Btn1 (Play/Pause) → GPIO27  (has internal pullup)
 *   Btn2 (Next)       → GPIO26  (has internal pullup)
 *   Btn3 (Prev)       → GPIO25  (has internal pullup)
 *   LED               → GPIO2   (onboard LED)
 *
 *   All buttons wire: one leg to GPIO, other leg to GND
 *   No external resistors needed
 *
 * Controls:
 *   Wheel turn             = volume up/down
 *   Btn1 tap               = play/pause
 *   Btn2 tap               = next track
 *   Btn3 tap               = previous track
 *   Btn1 hold + turn wheel = adjust sensitivity
 *   Btn1 + Btn3 hold 1.5s  = switch ESP-NOW <-> BLE
 *
 * Libraries: NimBLE-Arduino, Wire, Preferences
 *********************************************************************/

#include <Wire.h>
#include <esp_now.h>
#include <WiFi.h>
#include <Preferences.h>
#include <NimBLEDevice.h>
#include <NimBLEServer.h>
#include <NimBLEHIDDevice.h>

// ════════════════════════════════════════════════
//  PINS — all have internal pullup
// ════════════════════════════════════════════════
#define AS5600_SDA  21
#define AS5600_SCL  22
#define BTN1_PIN    27   // Play/Pause — internal pullup OK
#define BTN2_PIN    26   // Next       — internal pullup OK
#define BTN3_PIN    25   // Prev       — internal pullup OK
#define LED_PIN      2   // onboard LED

// ════════════════════════════════════════════════
//  AS5600
// ════════════════════════════════════════════════
#define AS5600_ADDR  0x36
#define REG_ANGLE_H  0x0E
#define REG_STATUS   0x0B

// ════════════════════════════════════════════════
//  ESP-NOW
// ════════════════════════════════════════════════
uint8_t macropadMAC[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

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

// ════════════════════════════════════════════════
//  BUTTON STATE
// ════════════════════════════════════════════════
struct BtnState {
  bool          last;
  bool          state;
  unsigned long changeMs;
  unsigned long pressMs;
  bool          holdFired;
};

// ════════════════════════════════════════════════
//  MODES
// ════════════════════════════════════════════════
#define MODE_ESPNOW  0
#define MODE_BLE     1

// ════════════════════════════════════════════════
//  GLOBALS
// ════════════════════════════════════════════════
int  currentMode     = MODE_BLE;  // default BLE — no macropad MAC set
int  sensitivity     = 5;
bool sensitivityMode = false;
bool bleConnected    = false;

int16_t       lastAngle   = 0;
int32_t       accumulator = 0;
unsigned long lastReadMs  = 0;

BtnState btn1 = {false, false, 0, 0, false};
BtnState btn2 = {false, false, 0, 0, false};
BtnState btn3 = {false, false, 0, 0, false};

unsigned long bothHeldStart = 0;
bool          bothWasHeld   = false;
bool          modeSwitched  = false;

const int   THRESH_BASE  = 30;
const float ACCEL_FACTOR = 2.0f;
const int   MAX_MULT     = 8;

// ════════════════════════════════════════════════
//  BLE HID
// ════════════════════════════════════════════════
NimBLEServer*         pServer   = nullptr;
NimBLEHIDDevice*      pHID      = nullptr;
NimBLECharacteristic* pCcReport = nullptr;

static const uint8_t hidReportMap[] = {
  0x05,0x0C, 0x09,0x01, 0xA1,0x01,
  0x85,0x01,
  0x15,0x00, 0x26,0xFF,0x03,
  0x19,0x00, 0x2A,0xFF,0x03,
  0x75,0x10, 0x95,0x01,
  0x81,0x00,
  0xC0,
};

#define CONSUMER_PLAY_PAUSE  0x00CD
#define CONSUMER_NEXT        0x00B5
#define CONSUMER_PREV        0x00B6
#define CONSUMER_VOL_UP      0x00E9
#define CONSUMER_VOL_DOWN    0x00EA

class ServerCB : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer* s, NimBLEConnInfo& info) override {
    bleConnected = true;
    s->updateConnParams(info.getConnHandle(), 6, 12, 0, 400);
    NimBLEDevice::stopAdvertising();
    Serial.println("BLE connected!");
    // Blink fast to confirm connection
    for(int i=0;i<5;i++){
      digitalWrite(LED_PIN,HIGH); delay(80);
      digitalWrite(LED_PIN,LOW);  delay(80);
    }
  }
  void onDisconnect(NimBLEServer* s, NimBLEConnInfo& info, int reason) override {
    bleConnected = false;
    Serial.print("BLE disconnected, reason: ");
    Serial.println(reason);
    if (currentMode == MODE_BLE) NimBLEDevice::startAdvertising();
  }
};

// ════════════════════════════════════════════════
//  NVS
// ════════════════════════════════════════════════
Preferences prefs;

void savePrefs() {
  prefs.begin("remote", false);
  prefs.putInt("sens", sensitivity);
  prefs.putInt("mode", currentMode);
  prefs.end();
}

void loadPrefs() {
  prefs.begin("remote", true);
  sensitivity  = constrain(prefs.getInt("sens", 5), 1, 10);
  currentMode  = constrain(prefs.getInt("mode", MODE_BLE), 0, 1);
  prefs.end();
}

// ════════════════════════════════════════════════
//  AS5600
// ════════════════════════════════════════════════
uint16_t readAngle() {
  Wire.beginTransmission(AS5600_ADDR);
  Wire.write(REG_ANGLE_H);
  Wire.endTransmission(false);
  Wire.requestFrom(AS5600_ADDR, (uint8_t)2);
  if (Wire.available() >= 2) {
    uint16_t v = (Wire.read() & 0x0F) << 8;
    v |= Wire.read();
    return v;
  }
  return 0;
}

bool magnetOK() {
  Wire.beginTransmission(AS5600_ADDR);
  Wire.write(REG_STATUS);
  Wire.endTransmission(false);
  Wire.requestFrom(AS5600_ADDR, (uint8_t)1);
  if (Wire.available()) return (Wire.read() & (1 << 5));
  return false;
}

int getThreshold() {
  return max(4, THRESH_BASE * (11 - sensitivity) / 5);
}

// ════════════════════════════════════════════════
//  SEND HELPERS
// ════════════════════════════════════════════════
void sendConsumerBLE(uint16_t usage) {
  if (!bleConnected || !pCcReport) return;
  uint8_t report[2] = {(uint8_t)(usage & 0xFF), (uint8_t)(usage >> 8)};
  pCcReport->setValue(report, 2);
  pCcReport->notify();
  delay(15);
  uint8_t rel[2] = {};
  pCcReport->setValue(rel, 2);
  pCcReport->notify();
  delay(10);
}

void sendVolBLE(int ticks) {
  int dir = (ticks > 0) ? 1 : -1;
  int n   = abs(ticks);
  for (int i = 0; i < n; i++)
    sendConsumerBLE(dir > 0 ? CONSUMER_VOL_UP : CONSUMER_VOL_DOWN);
}

void sendESPNow(uint8_t type, int8_t ticks = 0) {
  MediaMsg m;
  m.type        = type;
  m.ticks       = ticks;
  m.sensitivity = (uint8_t)sensitivity;
  esp_now_send(macropadMAC, (uint8_t*)&m, sizeof(m));
}

// ════════════════════════════════════════════════
//  BUTTON DEBOUNCE
//  Returns: 1=tap, 2=hold, 0=nothing
// ════════════════════════════════════════════════
int updateBtn(BtnState& b, int pin, unsigned long now) {
  bool raw = (digitalRead(pin) == LOW);  // active LOW with pullup
  if (raw != b.last) { b.last = raw; b.changeMs = now; }
  if ((now - b.changeMs) < 15) return 0;
  if (raw != b.state) {
    b.state = raw;
    if (raw) {
      b.pressMs   = now;
      b.holdFired = false;
    } else if (!b.holdFired && (now - b.pressMs) >= 40) {
      return 1; // tap
    }
  }
  if (b.state && !b.holdFired && (now - b.pressMs) >= 700) {
    b.holdFired = true;
    return 2; // hold
  }
  return 0;
}

// ════════════════════════════════════════════════
//  LED BLINK
// ════════════════════════════════════════════════
void blinkMode() {
  int blinks = (currentMode == MODE_ESPNOW) ? 1 : 3;
  for (int i = 0; i < blinks; i++) {
    digitalWrite(LED_PIN, HIGH); delay(150);
    digitalWrite(LED_PIN, LOW);  delay(150);
  }
}

// ════════════════════════════════════════════════
//  BLE ADVERTISING
// ════════════════════════════════════════════════
void startBLEAdvertising() {
  NimBLEAdvertising* pAdv = NimBLEDevice::getAdvertising();
  NimBLEAdvertisementData advData;
  advData.setFlags(0x06);
  advData.setAppearance(0x03C1);
  advData.addServiceUUID(pHID->getHidService()->getUUID());
  pAdv->setAdvertisementData(advData);
  NimBLEAdvertisementData scanData;
  scanData.setName("MediaRemote");
  pAdv->setScanResponseData(scanData);
  pAdv->enableScanResponse(true);
  pAdv->setMinInterval(32);
  pAdv->setMaxInterval(48);
  pAdv->start();
  Serial.println("BLE advertising as 'MediaRemote'");
}

// ════════════════════════════════════════════════
//  MODE SWITCH
// ════════════════════════════════════════════════
void switchMode() {
  currentMode = (currentMode == MODE_ESPNOW) ? MODE_BLE : MODE_ESPNOW;
  savePrefs();
  Serial.print("Switched to: ");
  Serial.println(currentMode == MODE_ESPNOW ? "ESP-NOW" : "BLE");
  if (currentMode == MODE_BLE) {
    startBLEAdvertising();
  } else {
    NimBLEDevice::stopAdvertising();
    bleConnected = false;
    Serial.println("ESP-NOW mode");
  }
}

// ════════════════════════════════════════════════
//  ESP-NOW INIT
// ════════════════════════════════════════════════
void setupESPNow() {
  if (esp_now_init() != ESP_OK) {
    Serial.println("ESP-NOW init failed"); return;
  }
  esp_now_peer_info_t peer;
  memset(&peer, 0, sizeof(peer));
  memcpy(peer.peer_addr, macropadMAC, 6);
  peer.channel = 0;
  peer.encrypt = false;
  if (esp_now_add_peer(&peer) == ESP_OK)
    Serial.println("ESP-NOW ready");
  else
    Serial.println("ESP-NOW peer failed — set macropadMAC[]");
}

// ════════════════════════════════════════════════
//  SETUP
// ════════════════════════════════════════════════
void setup() {
  Serial.begin(115200);
  delay(500);

  // Buttons — INPUT_PULLUP, wire to GND
  pinMode(BTN1_PIN, INPUT_PULLUP);
  pinMode(BTN2_PIN, INPUT_PULLUP);
  pinMode(BTN3_PIN, INPUT_PULLUP);
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);

  Wire.begin(AS5600_SDA, AS5600_SCL);
  Wire.setClock(400000);

  loadPrefs();

  WiFi.mode(WIFI_STA);
  WiFi.disconnect();

  Serial.println("\nMedia Remote");
  Serial.println("============");
  Serial.print("MAC: "); Serial.println(WiFi.macAddress());
  Serial.print("Mode: "); Serial.println(currentMode == MODE_ESPNOW ? "ESP-NOW" : "BLE");
  Serial.print("Sensitivity: "); Serial.println(sensitivity);
  Serial.println(magnetOK() ? "Magnet OK" : "No magnet");

  Serial.println("Buttons (press each to test):");
  Serial.print("  BTN1 GPIO"); Serial.print(BTN1_PIN);
  Serial.print(" = "); Serial.println(digitalRead(BTN1_PIN) ? "HIGH(OK)" : "LOW(check wiring)");
  Serial.print("  BTN2 GPIO"); Serial.print(BTN2_PIN);
  Serial.print(" = "); Serial.println(digitalRead(BTN2_PIN) ? "HIGH(OK)" : "LOW(check wiring)");
  Serial.print("  BTN3 GPIO"); Serial.print(BTN3_PIN);
  Serial.print(" = "); Serial.println(digitalRead(BTN3_PIN) ? "HIGH(OK)" : "LOW(check wiring)");

  // BLE init
  NimBLEDevice::init("MediaRemote");
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
  pCcReport = pHID->getInputReport(1);
  pHID->startServices();

  // ESP-NOW init
  setupESPNow();

  // Start in correct mode
  if (currentMode == MODE_BLE) {
    startBLEAdvertising();
  } else {
    Serial.println("ESP-NOW mode active");
  }

  delay(200);
  blinkMode();

  lastAngle = readAngle();
  lastReadMs = millis();

  Serial.println("Ready — waiting for BLE connection...");
  Serial.println("On PC: Bluetooth settings → Add device → MediaRemote");
}

// ════════════════════════════════════════════════
//  LOOP
// ════════════════════════════════════════════════
void loop() {
  unsigned long now = millis();

  // ── Encoder ──────────────────────────────────────────
  if (now - lastReadMs >= 10) {
    unsigned long dt = now - lastReadMs;
    lastReadMs = now;

    int16_t angle = (int16_t)readAngle();
    int16_t delta = angle - lastAngle;
    if (delta >  2048) delta -= 4096;
    if (delta < -2048) delta += 4096;
    lastAngle = angle;

    if (delta != 0) {
      if (sensitivityMode) {
        accumulator += delta;
        if (accumulator >= 80) {
          accumulator = 0;
          sensitivity = constrain(sensitivity + 1, 1, 10);
          savePrefs();
          Serial.print("Sens+: "); Serial.println(sensitivity);
        } else if (accumulator <= -80) {
          accumulator = 0;
          sensitivity = constrain(sensitivity - 1, 1, 10);
          savePrefs();
          Serial.print("Sens-: "); Serial.println(sensitivity);
        }
      } else {
        float velocity = (float)abs(delta) / (float)dt;
        int   mult     = (int)constrain(velocity * ACCEL_FACTOR * 80.0f, 1.0f, (float)MAX_MULT);
        accumulator += delta;
        int ticks  = 0;
        int thresh = getThreshold();
        while (accumulator >=  thresh) { accumulator -= thresh; ticks++;  }
        while (accumulator <= -thresh) { accumulator += thresh; ticks--;  }
        if (ticks != 0) {
          int finalTicks = constrain(ticks * mult, -127, 127);
          if (currentMode == MODE_ESPNOW) sendESPNow(MSG_VOLUME, (int8_t)finalTicks);
          else                            sendVolBLE(finalTicks);
          Serial.print("Vol:"); Serial.println(finalTicks);
        }
      }
    }
  }

  // ── Buttons ───────────────────────────────────────────
  int b1 = updateBtn(btn1, BTN1_PIN, now);
  int b2 = updateBtn(btn2, BTN2_PIN, now);
  int b3 = updateBtn(btn3, BTN3_PIN, now);

  // Mode switch: btn1 + btn3 held 1.5s
  bool bothHeld = btn1.state && btn3.state;
  if (bothHeld && !bothWasHeld) {
    bothHeldStart = now; modeSwitched = false; bothWasHeld = true;
  }
  if (!bothHeld) bothWasHeld = false;
  if (bothHeld && !modeSwitched && (now - bothHeldStart >= 1500)) {
    modeSwitched   = true;
    btn1.holdFired = true;
    btn3.holdFired = true;
    sensitivityMode = false;
    switchMode();
    blinkMode();
  }
  if (modeSwitched && !bothHeld) modeSwitched = false;

  // Btn1: tap=play/pause, hold=sensitivity mode
  if (b1 == 2 && !btn3.state) {
    sensitivityMode = true;
    accumulator = 0;
    Serial.println("Sensitivity mode ON — turn wheel");
  }
  if (!btn1.state && sensitivityMode) {
    sensitivityMode = false;
    Serial.println("Sensitivity mode OFF");
  }
  if (b1 == 1 && !sensitivityMode) {
    if (currentMode == MODE_ESPNOW) sendESPNow(MSG_PLAYPAUSE);
    else                            sendConsumerBLE(CONSUMER_PLAY_PAUSE);
    Serial.println("Play/Pause");
  }

  // Btn2: next
  if (b2 == 1) {
    if (currentMode == MODE_ESPNOW) sendESPNow(MSG_NEXT);
    else                            sendConsumerBLE(CONSUMER_NEXT);
    Serial.println("Next");
  }

  // Btn3: prev
  if (b3 == 1 && !btn1.state) {
    if (currentMode == MODE_ESPNOW) sendESPNow(MSG_PREV);
    else                            sendConsumerBLE(CONSUMER_PREV);
    Serial.println("Prev");
  }

  delay(4);
}
