#include <Arduino.h>
#include <driver/gpio.h>

constexpr int PIN_COIL_A = 4;
constexpr int PIN_COIL_B = 5;
constexpr uint32_t PULSE_MS = 30;

bool polarity = true;
bool running = false;
uint32_t pulse_count = 0;
uint32_t start_ms = 0;

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
  pulse_count++;
}

void setup() {
  Serial.begin(115200);

  pinMode(PIN_COIL_A, OUTPUT);
  pinMode(PIN_COIL_B, OUTPUT);
  setCoilIdle();
  gpio_set_drive_capability((gpio_num_t)PIN_COIL_A, GPIO_DRIVE_CAP_0);
  gpio_set_drive_capability((gpio_num_t)PIN_COIL_B, GPIO_DRIVE_CAP_0);

  delay(2000);

    Serial.println("Press Enter to start, Enter again to stop.");
}

void loop() {
  if (Serial.available()) {
    // Consume all available bytes so a single keypress doesn't trigger twice.
    while (Serial.available()) {
      Serial.read();
    }

    if (!running) {
      running = true;
      pulse_count = 0;
      start_ms = millis();
      Serial.println("Started. Press Enter after several full revolutions.");
    } else {
      running = false;
      uint32_t elapsed_ms = millis() - start_ms;
      Serial.printf("Stopped. Pulses: %lu, elapsed: %lu ms\r\n",
                     pulse_count, elapsed_ms);
      Serial.println("Divide pulses by the number of full revolutions you counted.");
    }
  }

  if (running) {
    pulseOnce();
    delay(PULSE_MS);
  }
}
