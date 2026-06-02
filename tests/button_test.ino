/*
 * Button test — prints which button is pressed
 * Uses INPUT_PULLUP — buttons wire to GND
 * 
 * Check Serial Monitor at 115200
 */

// v4 button pins
const uint8_t BTN_PINS[8] = {14, 13, 26, 25, 22, 21, 35, 19};

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("Button test — press any button");
  Serial.println("Pins: 14,13,26,25,22,21,35,19");
  Serial.println("All should read HIGH when not pressed");
  Serial.println("-----------------------------------");

  // GPIO 34,35,36,39 are input-only — no internal pullup
  // Use INPUT not INPUT_PULLUP for those
  for (int i = 0; i < 8; i++) {
    uint8_t pin = BTN_PINS[i];
    if (pin >= 34) {
      pinMode(pin, INPUT);  // input-only pins — need external pullup
    } else {
      pinMode(pin, INPUT_PULLUP);
    }
  }

  // Print initial state of all pins
  Serial.println("Initial pin states:");
  for (int i = 0; i < 8; i++) {
    Serial.print("  GPIO"); Serial.print(BTN_PINS[i]);
    Serial.print(" = "); Serial.println(digitalRead(BTN_PINS[i]) ? "HIGH" : "LOW");
  }
  Serial.println("-----------------------------------");
}

void loop() {
  for (int i = 0; i < 8; i++) {
    if (digitalRead(BTN_PINS[i]) == LOW) {
      Serial.print("BTN "); Serial.print(i + 1);
      Serial.print(" pressed (GPIO"); Serial.print(BTN_PINS[i]); Serial.println(")");
      delay(200);  // simple debounce for test
    }
  }
  delay(10);
}
