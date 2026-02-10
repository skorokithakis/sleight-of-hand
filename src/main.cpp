#include <Arduino.h>
#include <Preferences.h>
#include <PubSubClient.h>
#include <WiFi.h>
#include <WiFiManager.h>
#include <WiFiUdp.h>
#include <driver/gpio.h>
#include <time.h>

constexpr int PIN_COIL_A = 4;
constexpr int PIN_COIL_B = 5;
constexpr uint32_t PULSE_MS = 30;
constexpr uint16_t PULSES_PER_REVOLUTION = 960;
constexpr uint16_t PULSES_PER_SECOND = PULSES_PER_REVOLUTION / 60;

// Total time for one pulse cycle (energize + pause) per mode.
constexpr uint32_t STEADY_CYCLE_MS = 62;
constexpr uint32_t RUSH_CYCLE_MS = 58;

// Catch-up pulses run as fast as reliably possible.
constexpr uint32_t CATCHUP_CYCLE_MS = 32;

constexpr char NTP_SERVER[] = "pool.ntp.org";
constexpr long UTC_OFFSET_SECONDS = 0;

// --- MQTT ---

constexpr char MQTT_TOPIC_MODE_SET[] = "clock/mode/set";
constexpr char MQTT_TOPIC_MODE_STATE[] = "clock/mode/state";
constexpr uint16_t MQTT_DEFAULT_PORT = 1883;
constexpr uint32_t MQTT_RECONNECT_INTERVAL_MS = 5000;

constexpr uint16_t UDP_LOG_PORT = 37243;

char mqtt_host[64] = "";
uint16_t mqtt_port = MQTT_DEFAULT_PORT;

WiFiClient wifi_client;
PubSubClient mqtt_client(wifi_client);
Preferences preferences;
uint32_t last_mqtt_reconnect_attempt_ms = 0;

// --- Mode selection ---

enum class TickMode : uint8_t {
  steady,
  rush_wait,
};

TickMode current_mode = TickMode::rush_wait;
TickMode pending_mode = TickMode::rush_wait;
bool mode_change_pending = false;

// --- State ---

bool polarity = false;
uint16_t pulse_index = 0;

// When stopped, the loop does nothing. Used to manually position the hand
// before restarting at a minute boundary.
bool stopped = false;

// When true, the clock will start sweeping at the next minute boundary
// (i.e. when getMsIntoMinute() wraps past 0).
bool start_at_minute_pending = false;

// When true, the clock will stop after the current revolution completes
// (at pulse 960, i.e. the hand is at 12 o'clock).
bool stop_at_top_pending = false;

// The millis() timestamp of the current minute boundary. All pulse scheduling
// is relative to this, so timing stays locked to NTP regardless of loop jitter.
uint32_t minute_start_ms = 0;

// True while we're in the idle gap between finishing the revolution and the
// next minute boundary.
bool waiting_for_minute = false;

// --- Logging ---

static void logMessage(const char* message) {
  Serial.println(message);

  if (WiFi.status() != WL_CONNECTED) {
    return;
  }

  WiFiUDP udp;
  IPAddress broadcast_ip(255, 255, 255, 255);
  String packet = "(" + String(millis()) + " - " +
                  WiFi.localIP().toString() + "): " + message;
  udp.beginPacket(broadcast_ip, UDP_LOG_PORT);
  udp.write((const uint8_t*)packet.c_str(), packet.length());
  udp.endPacket();
}

static void logMessagef(const char* format, ...) {
  char buffer[256];
  va_list args;
  va_start(args, format);
  vsnprintf(buffer, sizeof(buffer), format, args);
  va_end(args);
  logMessage(buffer);
}

// --- Mode name helpers ---

static const char* modeToString(TickMode mode) {
  switch (mode) {
    case TickMode::steady:
      return "steady";
    case TickMode::rush_wait:
      return "rush_wait";
  }
  return "unknown";
}

static bool stringToMode(const char* str, TickMode& out) {
  if (strcmp(str, "steady") == 0) {
    out = TickMode::steady;
    return true;
  }
  if (strcmp(str, "rush_wait") == 0) {
    out = TickMode::rush_wait;
    return true;
  }
  return false;
}

// --- Coil drive ---

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
  pulse_index++;
}

static uint32_t getCycleMs() {
  switch (current_mode) {
    case TickMode::steady:
      return STEADY_CYCLE_MS;
    case TickMode::rush_wait:
      return RUSH_CYCLE_MS;
  }
  return STEADY_CYCLE_MS;
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

// Returns how many milliseconds have elapsed since the top of the current
// minute, according to NTP.
static uint32_t getMsIntoMinute() {
  struct timeval tv;
  gettimeofday(&tv, nullptr);
  struct tm timeinfo;
  localtime_r(&tv.tv_sec, &timeinfo);
  return (uint32_t)timeinfo.tm_sec * 1000 + (uint32_t)(tv.tv_usec / 1000);
}

// Rapidly pulse the motor to advance the hand to the target pulse position.
// Used at boot after NTP sync so the hand catches up to real time.
static void catchUpToPulse(uint16_t target_pulse) {
  while (pulse_index < target_pulse) {
    pulseOnce();
    uint32_t pause_ms = CATCHUP_CYCLE_MS > PULSE_MS ? CATCHUP_CYCLE_MS - PULSE_MS : 0;
    if (pause_ms > 0) {
      delay(pause_ms);
    }
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

  if (strcmp(buffer, "stop") == 0) {
    stopped = true;
    start_at_minute_pending = false;
    logMessage("Clock stopped.");
    return;
  }

  if (strcmp(buffer, "start") == 0) {
    stopped = false;
    start_at_minute_pending = false;
    pulse_index = 0;
    waiting_for_minute = false;
    minute_start_ms = millis();
    logMessage("Clock started immediately.");
    return;
  }

  if (strcmp(buffer, "start_at_minute") == 0) {
    start_at_minute_pending = true;
    stop_at_top_pending = false;
    logMessage("Clock will start at next minute boundary.");
    return;
  }

  if (strcmp(buffer, "stop_at_top") == 0) {
    stop_at_top_pending = true;
    start_at_minute_pending = false;
    logMessage("Clock will stop at top of next revolution.");
    return;
  }

  TickMode requested;
  if (stringToMode(buffer, requested)) {
    pending_mode = requested;
    mode_change_pending = true;
    logMessagef("Mode change queued: %s (applies at next revolution)", buffer);
  } else {
    logMessagef("Unknown command: %s", buffer);
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

  logMessagef("Connecting to MQTT %s:%d...", mqtt_host, mqtt_port);
  if (mqtt_client.connect("sleight-of-hand")) {
    logMessage("MQTT connected.");
    mqtt_client.subscribe(MQTT_TOPIC_MODE_SET);
    publishCurrentMode();
  } else {
    logMessagef("MQTT connection failed, rc=%d", mqtt_client.state());
  }
}

// --- WiFiManager save callback for custom parameters ---

static bool should_save_config = false;

static void onSaveConfig() {
  should_save_config = true;
}

// Called when the revolution completes (960 pulses done) to apply any pending
// mode change before the idle gap.
static void onRevolutionComplete() {
  if (stop_at_top_pending) {
    stop_at_top_pending = false;
    stopped = true;
    logMessage("Clock stopped at top.");
    return;
  }

  if (mode_change_pending) {
    current_mode = pending_mode;
    mode_change_pending = false;
    logMessagef("Mode changed to: %s", modeToString(current_mode));
    publishCurrentMode();
  }
}

// Called at each minute boundary to reset state for the new minute.
static void startNewMinute() {
  pulse_index = 0;
  waiting_for_minute = false;
}

// --- Arduino entrypoints ---

void setup() {
  Serial.begin(115200);

  pinMode(PIN_COIL_A, OUTPUT);
  pinMode(PIN_COIL_B, OUTPUT);
  setCoilIdle();
  gpio_set_drive_capability((gpio_num_t)PIN_COIL_A, GPIO_DRIVE_CAP_2);
  gpio_set_drive_capability((gpio_num_t)PIN_COIL_B, GPIO_DRIVE_CAP_2);

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
  wifi_manager.autoConnect("SleightOfHand");

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
    logMessagef("Saved MQTT config: %s:%d", mqtt_host, mqtt_port);
  }

  // NTP sync.
  configTime(UTC_OFFSET_SECONDS, 0, NTP_SERVER);
  logMessage("Waiting for NTP sync...");

  if (waitForNtpSync(10000)) {
    uint8_t current_second = getCurrentSecond();
    logMessagef("NTP synced, current second: %d", current_second);

    // The hand starts at 0 (12 o'clock) on boot. Advance it to match the
    // current time so minute and hour hands stay correct from the start.
    uint16_t target_pulse = (uint16_t)current_second * PULSES_PER_SECOND;
    catchUpToPulse(target_pulse);
    logMessagef("Caught up to pulse %d (second %d)", target_pulse,
                current_second);
  } else {
    logMessage("NTP sync failed, starting from 0.");
  }

  // MQTT setup.
  mqtt_client.setServer(mqtt_host, mqtt_port);
  mqtt_client.setCallback(onMqttMessage);

  randomSeed(esp_random());

  // Anchor the minute start to now minus how far we are into the current
  // minute, so pulse scheduling aligns with NTP.
  uint32_t ms_into_minute = getMsIntoMinute();
  minute_start_ms = millis() - ms_into_minute;
}

void loop() {
  // Only attempt MQTT (re)connection during the idle gap between revolutions,
  // so the blocking connect() call doesn't stall pulse timing.
  if (!mqtt_client.connected() && (waiting_for_minute || stopped)) {
    connectMqtt();
  }
  mqtt_client.loop();

  uint32_t now = millis();

  if (start_at_minute_pending) {
    // Poll NTP until the second rolls over to 0, then start.
    if (getMsIntoMinute() < 1000) {
      stopped = false;
      start_at_minute_pending = false;
      pulse_index = 0;
      waiting_for_minute = false;
      minute_start_ms = millis();
      logMessage("Minute boundary reached, clock started.");
    }
    return;
  }

  if (stopped) {
    return;
  }

  if (waiting_for_minute) {
    // All 960 pulses are done for this minute. Wait for the next minute
    // boundary before starting again.
    if (now - minute_start_ms >= 60000) {
      minute_start_ms += 60000;
      startNewMinute();
    }
    return;
  }

  // Schedule each pulse relative to the minute start, so accumulated delay()
  // error doesn't matter — only the NTP-anchored minute_start_ms matters.
  uint32_t cycle_ms = getCycleMs();
  uint32_t target_ms = minute_start_ms + (uint32_t)pulse_index * cycle_ms;

  if ((int32_t)(now - target_ms) >= 0) {
    pulseOnce();

    if (pulse_index >= PULSES_PER_REVOLUTION) {
      onRevolutionComplete();
      waiting_for_minute = true;
    }
  }
}
