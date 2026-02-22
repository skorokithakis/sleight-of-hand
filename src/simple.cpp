#include <Arduino.h>
#include <driver/gpio.h>

constexpr int PIN_COIL_A = 4;
constexpr int PIN_COIL_B = 5;
constexpr uint32_t PULSE_MS = 31;
constexpr uint32_t CYCLE_MS = 1000;

bool polarity = false;

static void setCoilIdle() {
  digitalWrite(PIN_COIL_A, LOW);
  digitalWrite(PIN_COIL_B, LOW);
}

static void pulseOnce() {
  if (polarity) {
    digitalWrite(PIN_COIL_A, HIGH);
    digitalWrite(PIN_COIL_B, LOW);
  } else {
    digitalWrite(PIN_COIL_A, LOW);
    digitalWrite(PIN_COIL_B, HIGH);
  }

  delay(PULSE_MS);

  setCoilIdle();
  polarity = !polarity;

  Serial.printf("Tick: polarity=%s\r\n", polarity ? "A" : "B");
}

void setup() {
  Serial.begin(115200);

  pinMode(PIN_COIL_A, OUTPUT);
  pinMode(PIN_COIL_B, OUTPUT);
  setCoilIdle();
  gpio_set_drive_capability((gpio_num_t)PIN_COIL_A, GPIO_DRIVE_CAP_2);
  gpio_set_drive_capability((gpio_num_t)PIN_COIL_B, GPIO_DRIVE_CAP_2);

  delay(2000);
}

void loop() {
  uint32_t tick_start = millis();
  pulseOnce();

  uint32_t elapsed = millis() - tick_start;
  if (elapsed < CYCLE_MS) {
    delay(CYCLE_MS - elapsed);
  }
}
