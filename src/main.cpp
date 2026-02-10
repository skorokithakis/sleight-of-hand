#include <Arduino.h>
#include <Preferences.h>
#include <PubSubClient.h>
#include <WiFi.h>
#include <WiFiManager.h>
#include <driver/gpio.h>
#include <time.h>

constexpr int PIN_COIL_A = 4;
constexpr int PIN_COIL_B = 5;
constexpr uint32_t PULSE_MS = 200;
constexpr uint32_t MINUTE_MS = 60000;
constexpr uint8_t TICKS_PER_MINUTE = 60;

// Catch-up ticks run faster to reach the current second quickly on boot.
constexpr uint32_t CATCHUP_TICK_MS = 50;

constexpr char NTP_SERVER[] = "pool.ntp.org";
// UTC offset in seconds. Doesn't matter for tick timing, but needed for
// correct minute-boundary alignment if you care about local time.
constexpr long UTC_OFFSET_SECONDS = 0;

// --- MQTT ---

constexpr char MQTT_TOPIC_MODE_SET[] = "clock/mode/set";
constexpr char MQTT_TOPIC_MODE_STATE[] = "clock/mode/state";
constexpr uint16_t MQTT_DEFAULT_PORT = 1883;
constexpr uint32_t MQTT_RECONNECT_INTERVAL_MS = 5000;

// Broker host/port are stored in flash via Preferences and configured through
// WiFiManager's captive portal on first boot.
char mqtt_host[64] = "";
uint16_t mqtt_port = MQTT_DEFAULT_PORT;

WiFiClient wifi_client;
PubSubClient mqtt_client(wifi_client);
Preferences preferences;
uint32_t last_mqtt_reconnect_attempt_ms = 0;

// --- Mode selection ---

enum class TickMode : uint8_t {
  steady,
  random_balanced,
  rush_then_wait,
};

TickMode current_mode = TickMode::random_balanced;
// Set by the MQTT callback, applied at the next minute boundary.
TickMode pending_mode = TickMode::random_balanced;
bool mode_change_pending = false;

// --- Random balanced mode config ---

constexpr uint32_t MIN_INTERVAL_MS = 500;
constexpr uint32_t MAX_INTERVAL_MS = 1500;

// --- Rush-then-wait mode config ---

constexpr uint32_t RUSH_INTERVAL_MS = 750;

// --- State ---

bool polarity = false;
uint32_t next_tick_ms = 0;
uint8_t tick_index = 0;
uint32_t intervals[TICKS_PER_MINUTE];

// Tracks where the second hand currently points (0-59).
uint8_t hand_position = 0;

// --- Mode name helpers ---

static const char* modeToString(TickMode mode) {
  switch (mode) {
    case TickMode::steady:
      return "steady";
    case TickMode::random_balanced:
      return "random_balanced";
    case TickMode::rush_then_wait:
      return "rush_then_wait";
  }
  return "unknown";
}

static bool stringToMode(const char* str, TickMode& out) {
  if (strcmp(str, "steady") == 0) {
    out = TickMode::steady;
    return true;
  }
  if (strcmp(str, "random_balanced") == 0) {
    out = TickMode::random_balanced;
    return true;
  }
  if (strcmp(str, "rush_then_wait") == 0) {
    out = TickMode::rush_then_wait;
    return true;
  }
  return false;
}

// --- Coil drive ---

static void setCoilIdle() {
  digitalWrite(PIN_COIL_A, LOW);
  digitalWrite(PIN_COIL_B, LOW);
}

static void tickOnce() {
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
  hand_position = (hand_position + 1) % 60;
}

// --- Interval generation ---

static void generateSteadyIntervals() {
  for (uint8_t i = 0; i < TICKS_PER_MINUTE; i++) {
    intervals[i] = 1000;
  }
}

static void generateRandomBalancedIntervals() {
  uint32_t sum = 0;
  for (uint8_t i = 0; i < TICKS_PER_MINUTE; i++) {
    intervals[i] = random(MIN_INTERVAL_MS, MAX_INTERVAL_MS + 1);
    sum += intervals[i];
  }

  // Distribute the error across random ticks so the sum is exactly 60000 ms.
  int32_t error = (int32_t)sum - (int32_t)MINUTE_MS;
  while (error != 0) {
    uint8_t index = random(TICKS_PER_MINUTE);
    if (error > 0) {
      uint32_t reduction = min((uint32_t)error,
                               intervals[index] - MIN_INTERVAL_MS);
      intervals[index] -= reduction;
      error -= reduction;
    } else {
      uint32_t increase = min((uint32_t)(-error),
                              MAX_INTERVAL_MS - intervals[index]);
      intervals[index] += increase;
      error += increase;
    }
  }
}

static void generateRushThenWaitIntervals() {
  uint32_t rush_total = 0;
  for (uint8_t i = 0; i < TICKS_PER_MINUTE - 1; i++) {
    intervals[i] = RUSH_INTERVAL_MS;
    rush_total += RUSH_INTERVAL_MS;
  }
  intervals[TICKS_PER_MINUTE - 1] = MINUTE_MS - rush_total;
}

static void generateIntervals() {
  switch (current_mode) {
    case TickMode::steady:
      generateSteadyIntervals();
      break;
    case TickMode::random_balanced:
      generateRandomBalancedIntervals();
      break;
    case TickMode::rush_then_wait:
      generateRushThenWaitIntervals();
      break;
  }
}

// --- NTP ---

static bool waitForNtpSync(uint32_t timeout_ms) {
  uint32_t start = millis();
  while (millis() - start < timeout_ms) {
    time_t now = time(nullptr);
    // Time is considered synced once it's past 2020-01-01.
    if (now > 1577836800) {
      return true;
    }
    delay(100);
  }
  return false;
}

static uint8_t getCurrentSecond() {
  struct tm timeinfo;
  getLocalTime(&timeinfo);
  return timeinfo.tm_sec;
}

// Rapidly tick the motor to advance the hand from its current position to the
// target second. This runs at boot after NTP sync so the hand catches up to
// real time.
static void catchUpToSecond(uint8_t target_second) {
  while (hand_position != target_second) {
    tickOnce();
    delay(CATCHUP_TICK_MS);
  }
}

// --- MQTT ---

static void publishCurrentMode() {
  if (mqtt_client.connected()) {
    mqtt_client.publish(MQTT_TOPIC_MODE_STATE, modeToString(current_mode),
                        true);
  }
}

static void onMqttMessage(char* topic, byte* payload, unsigned int length) {
  if (strcmp(topic, MQTT_TOPIC_MODE_SET) != 0) {
    return;
  }

  char buffer[32];
  unsigned int copy_length = min(length, (unsigned int)(sizeof(buffer) - 1));
  memcpy(buffer, payload, copy_length);
  buffer[copy_length] = '\0';

  TickMode requested;
  if (stringToMode(buffer, requested)) {
    pending_mode = requested;
    mode_change_pending = true;
    Serial.printf("Mode change queued: %s (applies at next minute)\n", buffer);
  } else {
    Serial.printf("Unknown mode: %s\n", buffer);
  }
}

static void connectMqtt() {
  if (strlen(mqtt_host) == 0) {
    return;
  }

  uint32_t now = millis();
  if (now - last_mqtt_reconnect_attempt_ms < MQTT_RECONNECT_INTERVAL_MS) {
    return;
  }
  last_mqtt_reconnect_attempt_ms = now;

  Serial.printf("Connecting to MQTT %s:%d...\n", mqtt_host, mqtt_port);
  if (mqtt_client.connect("sleight-of-hand")) {
    Serial.println("MQTT connected.");
    mqtt_client.subscribe(MQTT_TOPIC_MODE_SET);
    publishCurrentMode();
  } else {
    Serial.printf("MQTT connection failed, rc=%d\n", mqtt_client.state());
  }
}

// --- WiFiManager save callback for custom parameters ---

static bool should_save_config = false;

static void onSaveConfig() {
  should_save_config = true;
}

// --- Arduino entrypoints ---

void setup() {
  Serial.begin(115200);

  pinMode(PIN_COIL_A, OUTPUT);
  pinMode(PIN_COIL_B, OUTPUT);
  setCoilIdle();
  gpio_set_drive_capability((gpio_num_t)PIN_COIL_A, GPIO_DRIVE_CAP_3);
  gpio_set_drive_capability((gpio_num_t)PIN_COIL_B, GPIO_DRIVE_CAP_3);

  delay(2000);

  // Load saved MQTT config from flash.
  preferences.begin("clock", true);
  String saved_host = preferences.getString("mqtt_host", "");
  mqtt_port = preferences.getUShort("mqtt_port", MQTT_DEFAULT_PORT);
  preferences.end();
  saved_host.toCharArray(mqtt_host, sizeof(mqtt_host));

  // WiFiManager with custom MQTT parameters.
  WiFiManagerParameter mqtt_host_param("mqtt_host", "MQTT broker host",
                                       mqtt_host, sizeof(mqtt_host));
  char port_str[6];
  snprintf(port_str, sizeof(port_str), "%d", mqtt_port);
  WiFiManagerParameter mqtt_port_param("mqtt_port", "MQTT broker port",
                                       port_str, sizeof(port_str));

  WiFiManager wifi_manager;
  wifi_manager.setSaveConfigCallback(onSaveConfig);
  wifi_manager.addParameter(&mqtt_host_param);
  wifi_manager.addParameter(&mqtt_port_param);
  wifi_manager.setConfigPortalTimeout(180);
  wifi_manager.autoConnect("ClockSetup");

  if (should_save_config) {
    strncpy(mqtt_host, mqtt_host_param.getValue(), sizeof(mqtt_host) - 1);
    mqtt_host[sizeof(mqtt_host) - 1] = '\0';
    mqtt_port = atoi(mqtt_port_param.getValue());
    if (mqtt_port == 0) {
      mqtt_port = MQTT_DEFAULT_PORT;
    }

    preferences.begin("clock", false);
    preferences.putString("mqtt_host", mqtt_host);
    preferences.putUShort("mqtt_port", mqtt_port);
    preferences.end();
    Serial.printf("Saved MQTT config: %s:%d\n", mqtt_host, mqtt_port);
  }

  // NTP sync.
  configTime(UTC_OFFSET_SECONDS, 0, NTP_SERVER);
  Serial.println("Waiting for NTP sync...");

  if (waitForNtpSync(10000)) {
    uint8_t current_second = getCurrentSecond();
    Serial.printf("NTP synced, current second: %d\n", current_second);

    // The hand starts at 0 (12 o'clock) on boot. Advance it to the current
    // second so minute and hour hands stay correct from the start.
    catchUpToSecond(current_second);
    Serial.printf("Caught up to second %d\n", current_second);
  } else {
    Serial.println("NTP sync failed, starting from 0.");
  }

  // MQTT setup.
  mqtt_client.setServer(mqtt_host, mqtt_port);
  mqtt_client.setCallback(onMqttMessage);

  randomSeed(esp_random());
  generateIntervals();

  // Align the first tick to the next whole-second boundary so the schedule
  // stays locked to real time.
  struct tm timeinfo;
  if (getLocalTime(&timeinfo)) {
    struct timeval tv;
    gettimeofday(&tv, nullptr);
    uint32_t ms_into_second = tv.tv_usec / 1000;
    next_tick_ms = millis() + (1000 - ms_into_second);
  } else {
    next_tick_ms = millis();
  }
}

void loop() {
  if (!mqtt_client.connected()) {
    connectMqtt();
  }
  mqtt_client.loop();

  uint32_t now = millis();
  if ((int32_t)(now - next_tick_ms) >= 0) {
    tickOnce();
    next_tick_ms += intervals[tick_index];
    tick_index++;

    if (tick_index >= TICKS_PER_MINUTE) {
      tick_index = 0;

      // Apply pending mode change at minute boundary.
      if (mode_change_pending) {
        current_mode = pending_mode;
        mode_change_pending = false;
        Serial.printf("Mode changed to: %s\n", modeToString(current_mode));
        publishCurrentMode();
      }

      generateIntervals();
    }
  }
}
