#include <Arduino.h>
#include <driver/gpio.h>

constexpr int PIN_COIL_A = 4;
constexpr int PIN_COIL_B = 5;
constexpr uint32_t MAXIMUM_MS = 30;
constexpr uint32_t MINIMUM_MS = 15;
constexpr uint32_t STEP_MS = 5;
// How long to run each setting before moving to the next.
constexpr uint32_t DWELL_MS = 5000;

bool polarity = true;
uint32_t timing_ms = MAXIMUM_MS;
uint32_t last_change_ms = 0;

static void setCoilIdle() {
  // Keep both pins high-impedance between pulses.
  digitalWrite(PIN_COIL_A, LOW);
  digitalWrite(PIN_COIL_B, LOW);
  pinMode(PIN_COIL_A, INPUT);
  pinMode(PIN_COIL_B, INPUT);
}

static void tickOnce() {
  pinMode(PIN_COIL_A, OUTPUT);
  pinMode(PIN_COIL_B, OUTPUT);

  if (polarity) {
    digitalWrite(PIN_COIL_A, HIGH);
    digitalWrite(PIN_COIL_B, LOW);
  } else {
    digitalWrite(PIN_COIL_A, LOW);
    digitalWrite(PIN_COIL_B, HIGH);
  }

  delay(timing_ms);

  setCoilIdle();
  polarity = !polarity;
}

void setup() {
  Serial.begin(115200);

  setCoilIdle();
  gpio_set_drive_capability((gpio_num_t)PIN_COIL_A, GPIO_DRIVE_CAP_2);
  gpio_set_drive_capability((gpio_num_t)PIN_COIL_B, GPIO_DRIVE_CAP_2);

  delay(2000);

  last_change_ms = millis();
  Serial.printf("timing_ms=%lu\r\n", timing_ms);
}

void loop() {
  uint32_t now = millis();
  if (now - last_change_ms >= DWELL_MS) {
    last_change_ms = now;
    if (timing_ms > MINIMUM_MS) {
      timing_ms -= STEP_MS;
    } else {
      timing_ms = MAXIMUM_MS;
    }
    Serial.printf("timing_ms=%lu\r\n", timing_ms);
  }

  uint32_t tick_start = millis();
  tickOnce();

  uint32_t elapsed = millis() - tick_start;
  if (elapsed < timing_ms * 2) {
    delay(timing_ms * 2 - elapsed);
  }
}
