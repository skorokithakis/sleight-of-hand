#include <Arduino.h>

/*
 * ESP32 Analog Clock Driver
 * 
 * Ticks a quartz clock movement once per second.
 *
 * Hardware:
 *   GPIO 25 --[100Ω]--> Coil pin A
 *   GPIO 26 --[100Ω]--> Coil pin B
 */

const int PIN_COIL_A = 25;
const int PIN_COIL_B = 26;
const int PULSE_MS   = 30;

bool polarity = false;

void tick() {
  if (polarity) {
    digitalWrite(PIN_COIL_A, HIGH);
    delay(PULSE_MS);
    digitalWrite(PIN_COIL_A, LOW);
  } else {
    digitalWrite(PIN_COIL_B, HIGH);
    delay(PULSE_MS);
    digitalWrite(PIN_COIL_B, LOW);
  }
  polarity = !polarity;
}

void setup() {
  pinMode(PIN_COIL_A, OUTPUT);
  pinMode(PIN_COIL_B, OUTPUT);
  digitalWrite(PIN_COIL_A, LOW);
  digitalWrite(PIN_COIL_B, LOW);
}

void loop() {
  tick();
  delay(1000 - PULSE_MS);
}
