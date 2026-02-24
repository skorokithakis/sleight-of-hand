#include <Arduino.h>
#include <ArduinoOTA.h>
#include <Preferences.h>
#include <PubSubClient.h>
#include <WiFi.h>
#include <WiFiManager.h>
#include <WiFiUdp.h>
#include <driver/gpio.h>
#include <time.h>

constexpr int PIN_COIL_A = 5;
constexpr int PIN_COIL_B = 6;
constexpr uint16_t PULSES_PER_REVOLUTION = 60;

constexpr uint32_t PULSE_MS = 31;

constexpr uint32_t SPRINT_DEFAULT_MS = 300;
constexpr uint32_t CRAWL_DEFAULT_MS = 2000;
constexpr uint32_t CALIBRATE_SPRINT_MS = 200;

constexpr uint8_t TICK_COUNT = 59;

// Vetinari template values are total wall-clock durations (gap + PULSE_MS).
// Sorted ascending so that after a Fisher-Yates shuffle the distribution is
// unpredictable but the total always fits within ~58 s, leaving headroom for
// the NTP wait.
constexpr uint16_t VETINARI_TEMPLATE[TICK_COUNT] = {
     534,  550,  552,  561,  565,  574,  574,  619,  641,  649,
     685,  686,  687,  693,  694,  697,  700,  742,  743,  744,
     797,  804,  816,  828,  863,  866,  874,  874,  883,  906,
     920,  957,  981,  984, 1061, 1077, 1096, 1108, 1129, 1190,
    1192, 1204, 1211, 1227, 1252, 1268, 1310, 1381, 1381, 1387,
    1410, 1424, 1488, 1629, 1645, 1684, 1729, 1773, 2001,
};

// Filled at the start of each minute by fillTickDurations(). Each value is
// the total wall-clock time from one tick to the next; the loop subtracts
// PULSE_MS to get the delay after the pulse fires.
uint16_t tick_durations[TICK_COUNT];

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
  vetinari,
  hesitate,
  stumble,
  sprint,
  crawl,
};

TickMode current_mode = TickMode::vetinari;
TickMode pending_mode = TickMode::vetinari;
bool mode_change_pending = false;

// Tracks the last timekeeping mode that was active, so that start_at_minute
// can fall back to it if current_mode is a positioning mode when the minute
// boundary fires. Vetinari is the default because it's the power-on mode.
TickMode last_timekeeping_mode = TickMode::vetinari;

// Set on every sprint/crawl activation; no default needed.
uint32_t positioning_tick_ms = 0;

// --- State ---

bool polarity = false;
uint16_t pulse_index = 0;

// When stopped, the loop does nothing. Used to manually position the hand
// before restarting at a minute boundary.
bool stopped = false;

// When true, the clock will start at the next minute boundary
// (i.e. when getMsIntoMinute() wraps past 0).
bool start_at_minute_pending = false;

// When true, the clock will stop after the current revolution completes
// (at pulse 60, i.e. the hand is at 12 o'clock).
bool stop_at_top_pending = false;

// Set when a calibrate sprint is active. Calibrate sprints set pulse_index to
// position + 1 (one ahead of the actual hand position), so the early-stop
// check at pulse_index == PULSES_PER_REVOLUTION - 1 would fire one pulse too
// early (leaving the hand at p58 instead of p59). When this flag is set, the
// early-stop check is skipped and the existing pulse_index >= PULSES_PER_REVOLUTION
// wrap handles the revolution end correctly (hand lands at p59).
bool is_calibrate_sprint = false;

// --- Logging ---

static void logMessage(const char* message) {
  Serial.println(message);

  if (WiFi.status() != WL_CONNECTED) {
    return;
  }

  WiFiUDP udp;
  IPAddress broadcast_ip(255, 255, 255, 255);
  String packet = "(" + String(millis()) + " - " +
                  WiFi.localIP().toString() + "): " + message + "\r\n";
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

static void logBoundaryPulse() {
  struct timeval tv;
  gettimeofday(&tv, nullptr);
  struct tm timeinfo;
  localtime_r(&tv.tv_sec, &timeinfo);
  logMessagef("boundary time=%02d:%02d:%02d.%02ld",
              timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec,
              tv.tv_usec / 10000);
}

// --- Mode name helpers ---

static const char* modeToString(TickMode mode) {
  switch (mode) {
    case TickMode::steady:
      return "steady";
    case TickMode::rush_wait:
      return "rush_wait";
    case TickMode::vetinari:
      return "vetinari";
    case TickMode::hesitate:
      return "hesitate";
    case TickMode::stumble:
      return "stumble";
    case TickMode::sprint:
      return "sprint";
    case TickMode::crawl:
      return "crawl";
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
  if (strcmp(str, "vetinari") == 0) {
    out = TickMode::vetinari;
    return true;
  }
  if (strcmp(str, "hesitate") == 0) {
    out = TickMode::hesitate;
    return true;
  }
  if (strcmp(str, "stumble") == 0) {
    out = TickMode::stumble;
    return true;
  }
  if (strcmp(str, "sprint") == 0) {
    out = TickMode::sprint;
    return true;
  }
  if (strcmp(str, "crawl") == 0) {
    out = TickMode::crawl;
    return true;
  }
  return false;
}

static bool isTimekeeping(TickMode mode) {
  return mode != TickMode::sprint && mode != TickMode::crawl;
}

// --- Coil drive ---

static void setCoilIdle() {
  digitalWrite(PIN_COIL_A, LOW);
  digitalWrite(PIN_COIL_B, LOW);
}

static void pulseOnce(uint32_t pulse_ms = PULSE_MS) {
  if (polarity) {
    digitalWrite(PIN_COIL_A, HIGH);
    digitalWrite(PIN_COIL_B, LOW);
  } else {
    digitalWrite(PIN_COIL_A, LOW);
    digitalWrite(PIN_COIL_B, HIGH);
  }

  delay(pulse_ms);
  setCoilIdle();
  polarity = !polarity;
  pulse_index++;
}

// Returns false and sets stopped=true if the sum of tick_durations exceeds
// 59800 ms, which would cause the 59 ticks to overflow into the next minute
// before the NTP boundary pulse fires.
static bool validateTickDurationsSum() {
  uint32_t sum = 0;
  for (uint8_t i = 0; i < TICK_COUNT; i++) {
    sum += tick_durations[i];
  }
  if (sum > 59800) {
    logMessagef("tick_durations sum %lu exceeds 59800 for mode %s, stopping.",
                (unsigned long)sum, modeToString(current_mode));
    stopped = true;
    return false;
  }
  return true;
}

static void fillTickDurations() {
  switch (current_mode) {
    case TickMode::steady:
      for (uint8_t i = 0; i < TICK_COUNT; i++) {
        tick_durations[i] = 1000;
      }
      break;
    case TickMode::rush_wait:
      // 59 pulses in ~55 s leaves ~5 s of idle before the NTP boundary.
      for (uint8_t i = 0; i < TICK_COUNT; i++) {
        tick_durations[i] = 932;
      }
      break;
    case TickMode::vetinari:
      memcpy(tick_durations, VETINARI_TEMPLATE, sizeof(tick_durations));
      for (int i = TICK_COUNT - 1; i > 0; i--) {
        int j = esp_random() % (i + 1);
        uint16_t temporary = tick_durations[i];
        tick_durations[i] = tick_durations[j];
        tick_durations[j] = temporary;
      }
      break;
    case TickMode::hesitate:
      // 58 ticks at 980ms, 1 tick at 2000ms. Total: 58*980 + 2000 = 58840ms.
      for (uint8_t i = 0; i < TICK_COUNT; i++) {
        tick_durations[i] = 980;
      }
      tick_durations[0] = 2000;
      for (int i = TICK_COUNT - 1; i > 0; i--) {
        int j = esp_random() % (i + 1);
        uint16_t temporary = tick_durations[i];
        tick_durations[i] = tick_durations[j];
        tick_durations[j] = temporary;
      }
      break;
    case TickMode::stumble:
      // 58 ticks at 1010ms, 1 tick at 420ms. Total: 58*1010 + 420 = 59000ms.
      for (uint8_t i = 0; i < TICK_COUNT; i++) {
        tick_durations[i] = 1010;
      }
      tick_durations[0] = 420;
      for (int i = TICK_COUNT - 1; i > 0; i--) {
        int j = esp_random() % (i + 1);
        uint16_t temporary = tick_durations[i];
        tick_durations[i] = tick_durations[j];
        tick_durations[j] = temporary;
      }
      break;
    default:
      // Positioning modes (sprint/crawl) don't use the tick_durations table.
      break;
  }
  validateTickDurationsSum();
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

// Returns how many milliseconds have elapsed since the top of the current
// minute, according to NTP.
static uint32_t getMsIntoMinute() {
  struct timeval tv;
  gettimeofday(&tv, nullptr);
  struct tm timeinfo;
  localtime_r(&tv.tv_sec, &timeinfo);
  return (uint32_t)timeinfo.tm_sec * 1000 + (uint32_t)(tv.tv_usec / 1000);
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
    is_calibrate_sprint = false;
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

  if (strncmp(buffer, "calibrate ", 10) == 0) {
    char* endptr;
    uint32_t position = (uint32_t)strtoul(buffer + 10, &endptr, 10);
    if (endptr == buffer + 10) {
      logMessagef("Unknown command: %s", buffer);
      return;
    }
    if (position >= 60) {
      logMessagef("Unknown command: %s", buffer);
      return;
    }
    if (position == 59) {
      // Already at p59, which is the desired pre-boundary calibrate position.
      stopped = true;
      start_at_minute_pending = true;
      stop_at_top_pending = false;
      mode_change_pending = false;
      is_calibrate_sprint = false;
      logMessage("Calibrate: at p59, waiting for minute boundary.");
    } else {
      // Set pulse_index to one step past the known position so the sprint loop
      // sends exactly enough pulses to land on p59 (not p00) before waiting
      // for the minute boundary tick to move to p00. This is one of
      // the four sanctioned pulse_index reset points (see ARCHITECTURE.md).
      pulse_index = (uint16_t)(position + 1);

      // Parse an optional delay_ms after the position. We store delay_ms +
      // PULSE_MS because the sprint loop does delay(positioning_tick_ms -
      // PULSE_MS), so adding PULSE_MS here cancels out and delivers the exact
      // raw inter-pulse delay the user requested.
      uint32_t delay_ms = 0;
      bool has_custom_delay = false;
      if (*endptr == ' ') {
        char* delay_endptr;
        delay_ms = (uint32_t)strtoul(endptr + 1, &delay_endptr, 10);
        has_custom_delay = (delay_endptr != endptr + 1);
      }
      positioning_tick_ms = has_custom_delay ? delay_ms + PULSE_MS : CALIBRATE_SPRINT_MS;

      current_mode = TickMode::sprint;
      stopped = false;
      start_at_minute_pending = false;
      stop_at_top_pending = false;
      pending_mode = last_timekeeping_mode;
      mode_change_pending = true;
      is_calibrate_sprint = true;
      if (has_custom_delay) {
        logMessagef("Calibrate: sprinting from p%02u to p59 at %ums delay, then resuming %s.",
                    position, delay_ms, modeToString(last_timekeeping_mode));
      } else {
        logMessagef("Calibrate: sprinting from p%02u to p59, then resuming %s.",
                    position, modeToString(last_timekeeping_mode));
      }
      publishCurrentMode();
    }
    return;
  }

  // Check for positioning modes with an optional tick-duration parameter
  // (e.g. "sprint 150" or "crawl 500"). This must happen before stringToMode()
  // so the bare name still works for all other callers of stringToMode().
  TickMode parameterized_mode;
  bool has_parameterized_mode = false;
  if (strncmp(buffer, "sprint ", 7) == 0) {
    parameterized_mode = TickMode::sprint;
    has_parameterized_mode = true;
    uint32_t requested_ms = (uint32_t)strtoul(buffer + 7, nullptr, 10);
    positioning_tick_ms = requested_ms < 100 ? 100 : requested_ms;
  } else if (strncmp(buffer, "crawl ", 6) == 0) {
    parameterized_mode = TickMode::crawl;
    has_parameterized_mode = true;
    uint32_t requested_ms = (uint32_t)strtoul(buffer + 6, nullptr, 10);
    positioning_tick_ms = requested_ms < 100 ? 100 : requested_ms;
  }

  if (has_parameterized_mode) {
    current_mode = parameterized_mode;
    mode_change_pending = false;
    is_calibrate_sprint = false;
    stopped = false;
    start_at_minute_pending = false;
    stop_at_top_pending = false;
    logMessagef("Mode changed to: %s (immediate, tick=%ums)",
                modeToString(parameterized_mode), positioning_tick_ms);
    publishCurrentMode();
    return;
  }

  TickMode requested;
  if (stringToMode(buffer, requested)) {
    if (!isTimekeeping(requested)) {
      // Positioning modes activate immediately because they don't need NTP
      // synchronization. Any pending blocking state is superseded: the user
      // explicitly chose a positioning mode, so waiting for a minute boundary
      // or a stop-at-top would prevent it from ever starting.
      positioning_tick_ms = (requested == TickMode::sprint)
                                ? SPRINT_DEFAULT_MS
                                : CRAWL_DEFAULT_MS;
      current_mode = requested;
      mode_change_pending = false;
      is_calibrate_sprint = false;
      stopped = false;
      start_at_minute_pending = false;
      stop_at_top_pending = false;
      logMessagef("Mode changed to: %s (immediate, tick=%ums)",
                  modeToString(requested), positioning_tick_ms);
      publishCurrentMode();
    } else if (stopped) {
      // No revolution to wait for, so apply the mode immediately and wait for
      // the next minute boundary to start synchronized.
      current_mode = requested;
      if (isTimekeeping(current_mode)) last_timekeeping_mode = current_mode;
      mode_change_pending = false;
      start_at_minute_pending = true;
      logMessagef("Mode changed to: %s (starting at next minute boundary)",
                   modeToString(requested));
      publishCurrentMode();
    } else {
      pending_mode = requested;
      mode_change_pending = true;
      logMessagef("Mode change queued: %s (applies at next revolution)",
                   buffer);
    }
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

// Called when the revolution completes (60 pulses done) to apply any pending
// mode change before the idle gap.
static void onRevolutionComplete() {
  is_calibrate_sprint = false;

  if (stop_at_top_pending) {
    stop_at_top_pending = false;
    stopped = true;
    logMessage("Clock stopped at top.");
    return;
  }

  if (mode_change_pending) {
    TickMode old_mode = current_mode;
    current_mode = pending_mode;
    if (isTimekeeping(current_mode)) last_timekeeping_mode = current_mode;
    mode_change_pending = false;
    logMessagef("Mode changed to: %s", modeToString(current_mode));
    publishCurrentMode();

    // When switching from a positioning mode to a timekeeping mode, wait for
    // the next minute boundary to re-sync.
    if (!isTimekeeping(old_mode) && isTimekeeping(current_mode)) {
      stopped = true;
      start_at_minute_pending = true;
      logMessage("Waiting for minute boundary to re-sync.");
    }
  }
}

// Called at each minute boundary to reset state for the new minute.
static void startNewMinute() {
  pulse_index = 0;
  fillTickDurations();
}

// --- Arduino entrypoints ---

void setup() {
  Serial.begin(115200);

  pinMode(PIN_COIL_A, OUTPUT);
  pinMode(PIN_COIL_B, OUTPUT);
  setCoilIdle();
  gpio_set_drive_capability((gpio_num_t)PIN_COIL_A, GPIO_DRIVE_CAP_1);
  gpio_set_drive_capability((gpio_num_t)PIN_COIL_B, GPIO_DRIVE_CAP_1);

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

  ArduinoOTA.setHostname("sleight-of-hand");
  ArduinoOTA.begin();

  // NTP sync.
  configTime(UTC_OFFSET_SECONDS, 0, NTP_SERVER);
  logMessage("Waiting for NTP sync...");

  if (waitForNtpSync(10000)) {
    logMessage("NTP synced, waiting for minute boundary to start.");
  } else {
    logMessage("NTP sync failed, waiting for minute boundary to start.");
  }

  // MQTT setup.
  mqtt_client.setServer(mqtt_host, mqtt_port);
  mqtt_client.setCallback(onMqttMessage);

  randomSeed(esp_random());

  // Wait for the next minute boundary before starting. The hand is assumed to
  // be at p59; start_at_minute_pending will fire the p59→p00 boundary pulse
  // and then begin the first full minute.
  stopped = true;
  start_at_minute_pending = true;
}

void loop() {
  ArduinoOTA.handle();

  // Check the minute boundary first, before any potentially-blocking MQTT
  // work. This ensures the boundary pulse fires as soon as the NTP second
  // rolls over, regardless of MQTT state.
  if (isTimekeeping(current_mode) && pulse_index == 59 && !stopped) {
    if (getMsIntoMinute() < 500) {
      pulseOnce();
      logBoundaryPulse();
      onRevolutionComplete();
      if (!stopped) {
        startNewMinute();
      }
      return;
    }
  }

  // Only attempt MQTT (re)connection when stopped. Attempting reconnection
  // while timekeeping risks connectMqtt() blocking through the p59 boundary
  // window, causing a missed pulse. mqtt_client.loop() still runs every
  // iteration so message handling is unaffected.
  if (!mqtt_client.connected() && stopped) {
    connectMqtt();
  }
  mqtt_client.loop();

  if (start_at_minute_pending) {
    // Poll NTP until the second rolls over to 0, then start.
    if (getMsIntoMinute() < 1000) {
      if (mode_change_pending) {
        current_mode = pending_mode;
        if (isTimekeeping(current_mode)) last_timekeeping_mode = current_mode;
        mode_change_pending = false;
        logMessagef("Mode changed to: %s", modeToString(current_mode));
        publishCurrentMode();
      }
      if (!isTimekeeping(current_mode)) {
        // If the user was in a positioning mode when the minute boundary fires,
        // fall back to the last timekeeping mode so the clock actually keeps
        // time rather than running in an unsynchronized positioning mode.
        current_mode = last_timekeeping_mode;
        logMessagef("Falling back to last timekeeping mode: %s",
                    modeToString(current_mode));
        publishCurrentMode();
      }
      stopped = false;
      start_at_minute_pending = false;
      // Fire the p59→p00 boundary pulse before starting the new minute.
      // The hand is always at p59 when this path runs: on boot the hand is
      // assumed to be at p59, and calibrate/positioning modes sprint to p59
      // before setting start_at_minute_pending.
      pulseOnce();
      logBoundaryPulse();
      startNewMinute(); // pulse_index = 0, fill tick_durations
      logMessage("Minute boundary reached, clock started.");
    }
    return;
  }

  if (stopped) {
    return;
  }

  if (isTimekeeping(current_mode)) {
    if (pulse_index < 59) {
      uint16_t duration = tick_durations[pulse_index];
      // Delay first so that the pulse fires at the scheduled wall-clock time,
      // then log the tick that fired. pulse_index is captured after the delay
      // but before pulseOnce() increments it, so the log is still accurate.
      delay(duration - PULSE_MS);
      uint8_t tick_index = pulse_index;
      pulseOnce();

      struct timeval tv;
      gettimeofday(&tv, nullptr);
      struct tm timeinfo;
      localtime_r(&tv.tv_sec, &timeinfo);
      logMessagef("tick %u t=%u time=%02d:%02d:%02d.%02ld",
                  tick_index, duration,
                  timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec,
                  tv.tv_usec / 10000);
    }
    // pulse_index == 59: the boundary check at the top of loop() handles this
    // case; nothing to do here.
  } else {
    // Positioning modes (sprint/crawl) run continuously without NTP sync.
    // Both modes share the same structure; only the tick duration differs,
    // and that is already stored in positioning_tick_ms.

    // When the hand is one pulse away from completing a revolution AND a
    // timekeeping mode change is pending, skip the final pulse (p59→p00) so
    // the hand stops at p59. start_at_minute_pending will then fire the
    // p59→p00 boundary pulse at the correct NTP moment. Without this check,
    // the revolution would complete to p00, violating the invariant that the
    // hand is always at p59 when start_at_minute_pending fires.
    //
    // Calibrate sprints are excluded: they set pulse_index = position + 1
    // (one ahead of the actual hand position), so this check would fire one
    // pulse too early (hand at p58 instead of p59). The existing
    // pulse_index >= PULSES_PER_REVOLUTION wrap handles calibrate sprints
    // correctly (the sprint fires exactly enough pulses to land at p59).
    if (!is_calibrate_sprint && !stop_at_top_pending &&
        pulse_index == PULSES_PER_REVOLUTION - 1 &&
        mode_change_pending && isTimekeeping(pending_mode)) {
      onRevolutionComplete();
      return;
    }

    pulseOnce();
    delay(positioning_tick_ms - PULSE_MS);
    if (pulse_index >= PULSES_PER_REVOLUTION) {
      onRevolutionComplete();
      pulse_index = 0;
    }
  }
}
