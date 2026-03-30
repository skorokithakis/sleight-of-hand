#include <Arduino.h>
#include <ArduinoOTA.h>
#include <Preferences.h>
#include <PubSubClient.h>
#include <WiFi.h>
#include <WiFiManager.h>
#include <WiFiUdp.h>
#include <driver/gpio.h>
#include <math.h>
#include <time.h>

constexpr int PIN_COIL_A = 5;
constexpr int PIN_COIL_B = 6;
constexpr uint16_t PULSES_PER_REVOLUTION = 60;

constexpr uint32_t PULSE_MS = 31;

constexpr uint32_t CALIBRATE_SPRINT_MS = 200;
constexpr uint16_t RUSH_WAIT_DEFAULT_MS = 700;

// Sentinel value for calibration_target_minute meaning "no WAIT target set"
// (i.e. SPRINT path). UINT32_MAX is used rather than 0 because 0 is a valid
// 12h-cycle minute (12:00:00).
constexpr uint32_t NO_CALIBRATION_TARGET = UINT32_MAX;

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
// All internal time is UTC. `calibrate H:MM:SS` expects UTC, not local wall-clock time.
constexpr long UTC_OFFSET_SECONDS = 0;

// --- MQTT ---

constexpr char MQTT_TOPIC_MODE_SET[] = "clock/mode/set";
constexpr char MQTT_TOPIC_MODE_STATE[] = "clock/mode/state";
constexpr uint16_t MQTT_DEFAULT_PORT = 1883;
constexpr uint32_t MQTT_RECONNECT_INTERVAL_STOPPED_MS = 5000;
constexpr uint32_t MQTT_RECONNECT_INTERVAL_RUNNING_MS = 60000;
constexpr uint32_t WIFI_RECONNECT_INTERVAL_MS = 30000;

constexpr uint16_t UDP_LOG_PORT = 37243;

char mqtt_host[64] = "";
uint16_t mqtt_port = MQTT_DEFAULT_PORT;

WiFiClient wifi_client;
PubSubClient mqtt_client(wifi_client);
Preferences preferences;
uint32_t last_mqtt_reconnect_attempt_ms = 0;
uint32_t last_wifi_reconnect_attempt_ms = 0;

// --- Mode selection ---

enum class TickMode : uint8_t {
  steady,
  rush_wait,
  vetinari,
  hesitate,
  stumble,
  gravity,
};

TickMode current_mode = TickMode::vetinari;
TickMode pending_mode = TickMode::vetinari;
bool mode_change_pending = false;

// All timekeeping modes in one place. Used by selectRandomTimekeepingMode() so
// that adding a new timekeeping mode only requires updating this array and the
// enum — the random picker stays correct automatically.
constexpr TickMode TIMEKEEPING_MODES[] = {
  TickMode::steady,
  TickMode::rush_wait,
  TickMode::vetinari,
  TickMode::hesitate,
  TickMode::stumble,
  TickMode::gravity,
};
constexpr uint8_t TIMEKEEPING_MODE_COUNT =
    sizeof(TIMEKEEPING_MODES) / sizeof(TIMEKEEPING_MODES[0]);

// Per-tick duration for rush_wait mode. Adjusted via "rush_wait <ms>" MQTT
// command; bare "rush_wait" resets it to the default.
uint16_t rush_wait_tick_ms = RUSH_WAIT_DEFAULT_MS;

// --- State ---

bool polarity = false;

// Seconds past 12:00:00 in a 12-hour cycle (0–43199). Incremented mod 43200
// on every pulseOnce() call. The second-hand position within the current
// minute is (displayed_time % 60). Starts at 0 on boot (wrong, but corrected
// at the first NTP minute boundary via start_at_minute_pending).
uint32_t displayed_time = 0;

// When stopped, the loop does nothing. Used to manually position the hand
// before restarting at a minute boundary.
bool stopped = false;

// When true, the clock will start at the next minute boundary
// (i.e. when getMsIntoMinute() wraps past 0).
bool start_at_minute_pending = false;

// True while a calibration sprint is in progress. The main loop pulses at
// CALIBRATE_SPRINT_MS until displayed_time catches up to NTP (SPRINT path)
// or until p59 is reached (WAIT path).
bool is_calibrating = false;

// For the WAIT calibration path: the NTP time (as seconds in the 12h cycle,
// with seconds component = 0) that start_at_minute_pending must match before
// firing. NO_CALIBRATION_TARGET means SPRINT path (fire at the next minute
// boundary as usual).
uint32_t calibration_target_minute = NO_CALIBRATION_TARGET;

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
    case TickMode::gravity:
      return "gravity";
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
  if (strcmp(str, "gravity") == 0) {
    out = TickMode::gravity;
    return true;
  }
  return false;
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
  displayed_time = (displayed_time + 1) % 43200;
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
        tick_durations[i] = rush_wait_tick_ms;
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
    case TickMode::gravity: {
      // Pendulum physics: angular velocity at angle θ from the top is
      // proportional to sqrt(1 + k - cos(θ)), so tick duration is the
      // inverse. The energy excess k prevents the singularity at the top
      // (where a pendulum with exactly the minimum energy would take infinite
      // time). k=0.05 gives a ~6.3x ratio between the slowest tick (at 12,
      // ~3150ms) and the fastest (at 6, ~500ms).
      constexpr float ENERGY_EXCESS = 0.05f;
      constexpr float BUDGET_MS = 59000.0f;
      float raw_times[TICK_COUNT];
      float raw_sum = 0;
      for (uint8_t i = 0; i < TICK_COUNT; i++) {
        float theta = 2.0f * M_PI * (i + 0.5f) / 60.0f;
        raw_times[i] = 1.0f / sqrtf(1.0f + ENERGY_EXCESS - cosf(theta));
        raw_sum += raw_times[i];
      }
      float scale = BUDGET_MS / raw_sum;
      for (uint8_t i = 0; i < TICK_COUNT; i++) {
        tick_durations[i] = (uint16_t)(raw_times[i] * scale + 0.5f);
      }
      break;
    }
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

// Returns the current NTP time as seconds past 12:00:00 in a 12-hour cycle
// (0–43199). Used by calibration to compare against displayed_time.
static uint32_t getNtpTimeIn12hCycle() {
  struct timeval tv;
  gettimeofday(&tv, nullptr);
  struct tm timeinfo;
  localtime_r(&tv.tv_sec, &timeinfo);
  return (uint32_t)((timeinfo.tm_hour % 12) * 3600 +
                    timeinfo.tm_min * 60 +
                    timeinfo.tm_sec);
}

// --- MQTT ---

static void publishCurrentMode() {
  if (mqtt_client.connected()) {
    mqtt_client.publish(MQTT_TOPIC_MODE_STATE, modeToString(current_mode),
                        true);
  }
}

// Picks a random timekeeping mode, applies it to current_mode, resets
// rush_wait_tick_ms if needed, logs the selection, and publishes via MQTT.
// Does not touch displayed_time.
static void selectRandomTimekeepingMode() {
  uint8_t index = (uint8_t)(esp_random() % TIMEKEEPING_MODE_COUNT);
  TickMode chosen = TIMEKEEPING_MODES[index];
  current_mode = chosen;
  if (chosen == TickMode::rush_wait) {
    // Mirror what the bare "rush_wait" MQTT command does: reset to the default
    // tick duration so the randomly-selected mode behaves predictably.
    rush_wait_tick_ms = RUSH_WAIT_DEFAULT_MS;
  }
  logMessagef("Random mode selected: %s", modeToString(chosen));
  publishCurrentMode();
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
    is_calibrating = false;
    calibration_target_minute = NO_CALIBRATION_TARGET;
    logMessage("Clock stopped.");
    return;
  }

  if (strcmp(buffer, "start") == 0) {
    stopped = false;
    start_at_minute_pending = false;
    displayed_time = 0;
    is_calibrating = false;
    calibration_target_minute = NO_CALIBRATION_TARGET;
    logMessage("Clock started immediately.");
    return;
  }

  if (strcmp(buffer, "start_at_minute") == 0) {
    // Cancelling calibration here is intentional: if the hand isn't at p59,
    // letting the boundary pulse fire from the wrong position would violate the
    // p59 invariant. The user is explicitly overriding whatever was in progress.
    is_calibrating = false;
    calibration_target_minute = NO_CALIBRATION_TARGET;
    start_at_minute_pending = true;
    logMessage("Clock will start at next minute boundary.");
    return;
  }

  if (strncmp(buffer, "calibrate ", 10) == 0) {
    const char* arg = buffer + 10;

    // Try to parse as H:MM:SS (flexible hour: 1 or 2 digits, mandatory 2-digit
    // MM and SS). If that fails, fall through to the legacy position parser.
    uint32_t parsed_hour = 0;
    uint32_t parsed_min = 0;
    uint32_t parsed_sec = 0;
    char* endptr_h;
    parsed_hour = (uint32_t)strtoul(arg, &endptr_h, 10);
    bool is_hmmss = false;
    if (endptr_h != arg && *endptr_h == ':') {
      char* endptr_m;
      parsed_min = (uint32_t)strtoul(endptr_h + 1, &endptr_m, 10);
      if (endptr_m != endptr_h + 1 && *endptr_m == ':') {
        char* endptr_s;
        parsed_sec = (uint32_t)strtoul(endptr_m + 1, &endptr_s, 10);
        if (endptr_s != endptr_m + 1 && *endptr_s == '\0') {
          // Validate ranges.
          if (parsed_min < 60 && parsed_sec < 60) {
            is_hmmss = true;
          }
        }
      }
    }

    if (is_hmmss) {
      // Cancel any in-progress calibration unconditionally before doing
      // anything else, including the early-return for d==0. This ensures a
      // new calibrate command always supersedes the previous one cleanly.
      is_calibrating = false;
      calibration_target_minute = NO_CALIBRATION_TARGET;

      // Set displayed_time from the parsed H:MM:SS, anchored to the 12h cycle.
      displayed_time = (uint32_t)((parsed_hour % 12) * 3600 +
                                  parsed_min * 60 +
                                  parsed_sec);

      uint32_t ntp_time = getNtpTimeIn12hCycle();
      uint32_t forward_distance = (ntp_time - displayed_time + 43200) % 43200;

      if (forward_distance == 0) {
        logMessage("Calibrate H:MM:SS: already correct, no calibration needed.");
        return;
      }

      // Start fresh calibration.
      is_calibrating = true;
      mode_change_pending = false;
      stopped = false;
      start_at_minute_pending = false;

      if (forward_distance <= 10800) {
        // SPRINT path: sprint forward until displayed_time catches up to NTP.
        // calibration_target_minute stays NO_CALIBRATION_TARGET.
        logMessagef("Calibrate H:MM:SS: sprinting to NTP (distance=%lus).",
                    (unsigned long)forward_distance);
      } else {
        // WAIT path: sprint to p59 within the current minute, then wait for
        // the specific NTP minute boundary where displayed_time will match.
        // The target is the minute after the one we'll be at when we reach p59.
        // Since p59 is second 59 of the current displayed minute, the next
        // minute boundary is (displayed_time/60 + 1)*60.
        calibration_target_minute = ((displayed_time / 60 + 1) * 60) % 43200;
        logMessagef("Calibrate H:MM:SS: WAIT path, sprinting to p59 then waiting for minute %lus.",
                    (unsigned long)calibration_target_minute);
      }
      publishCurrentMode();
      return;
    }

    // Legacy position parser: "calibrate <position>".
    char* endptr;
    uint32_t position = (uint32_t)strtoul(arg, &endptr, 10);
    if (endptr == arg) {
      logMessagef("Unknown command: %s", buffer);
      return;
    }
    if (position >= 60) {
      logMessagef("Unknown command: %s", buffer);
      return;
    }

    // Set displayed_time anchored to the current NTP hour and minute.
    struct timeval calibrate_tv;
    gettimeofday(&calibrate_tv, nullptr);
    struct tm calibrate_tm;
    localtime_r(&calibrate_tv.tv_sec, &calibrate_tm);

    // Cancel any in-progress calibration unconditionally before doing anything
    // else. This ensures a new calibrate command always supersedes the previous
    // one cleanly, including the position-59 early-return path below.
    is_calibrating = false;
    calibration_target_minute = NO_CALIBRATION_TARGET;

    if (position == 59) {
      // Already at p59, which is the desired pre-boundary calibrate position.
      displayed_time = (uint32_t)((calibrate_tm.tm_hour % 12) * 3600 +
                                  calibrate_tm.tm_min * 60 + 59);
      stopped = true;
      start_at_minute_pending = true;
      mode_change_pending = false;
      logMessage("Calibrate: at p59, waiting for minute boundary.");
      return;
    }

    // For positions 0–58: set displayed_time so that displayed_time % 60 ==
    // position. The calibration branch stops at displayed_time % 60 == 59
    // WITHOUT pulsing, so starting at position fires exactly (59 - position)
    // pulses to land on p59.
    displayed_time = (uint32_t)((calibrate_tm.tm_hour % 12) * 3600 +
                                calibrate_tm.tm_min * 60 + position);

    uint32_t ntp_time = getNtpTimeIn12hCycle();
    uint32_t forward_distance = (ntp_time - displayed_time + 43200) % 43200;

    // Start fresh calibration.
    is_calibrating = true;
    mode_change_pending = false;
    stopped = false;
    start_at_minute_pending = false;

    if (forward_distance <= 10800) {
      // calibration_target_minute stays NO_CALIBRATION_TARGET (SPRINT path).
      logMessagef("Calibrate p%02u: sprinting to NTP (distance=%lus).",
                  position, (unsigned long)forward_distance);
    } else {
      calibration_target_minute = ((displayed_time / 60 + 1) * 60) % 43200;
      logMessagef("Calibrate p%02u: WAIT path, sprinting to p59 then waiting for minute %lus.",
                  position, (unsigned long)calibration_target_minute);
    }
    publishCurrentMode();
    return;
  }

  if (strncmp(buffer, "rush_wait ", 10) == 0) {
    // rush_wait is a timekeeping mode, so it must queue at revolution
    // boundaries rather than activate immediately.
    uint32_t requested_ms = (uint32_t)strtoul(buffer + 10, nullptr, 10);
    rush_wait_tick_ms = (uint16_t)(requested_ms < 200 ? 200 : requested_ms);
    if (stopped) {
      current_mode = TickMode::rush_wait;
      mode_change_pending = false;
      start_at_minute_pending = true;
      logMessagef("Mode changed to: rush_wait (starting at next minute boundary, tick=%ums)",
                  rush_wait_tick_ms);
      publishCurrentMode();
    } else {
      pending_mode = TickMode::rush_wait;
      mode_change_pending = true;
      logMessagef("Mode change queued: rush_wait (applies at next revolution, tick=%ums)",
                  rush_wait_tick_ms);
    }
    return;
  }

  TickMode requested;
  if (stringToMode(buffer, requested)) {
    if (stopped) {
      // No revolution to wait for, so apply the mode immediately and wait for
      // the next minute boundary to start synchronized.
      if (requested == TickMode::rush_wait) {
        // Bare "rush_wait" always reverts to the default tick duration.
        rush_wait_tick_ms = RUSH_WAIT_DEFAULT_MS;
      }
      current_mode = requested;
      mode_change_pending = false;
      start_at_minute_pending = true;
      logMessagef("Mode changed to: %s (starting at next minute boundary)",
                   modeToString(requested));
      publishCurrentMode();
    } else {
      if (requested == TickMode::rush_wait) {
        // Bare "rush_wait" always reverts to the default tick duration.
        rush_wait_tick_ms = RUSH_WAIT_DEFAULT_MS;
      }
      pending_mode = requested;
      mode_change_pending = true;
      logMessagef("Mode change queued: %s (applies at next revolution)",
                   buffer);
    }
  } else {
    logMessagef("Unknown command: %s", buffer);
  }
}

static void reconnectWifi() {
  uint32_t now = millis();
  if (now - last_wifi_reconnect_attempt_ms < WIFI_RECONNECT_INTERVAL_MS) {
    return;
  }
  last_wifi_reconnect_attempt_ms = now;

  // disconnect() + begin() (with no arguments) is more reliable than
  // WiFi.reconnect() on ESP32 because it fully tears down the association
  // before re-reading saved credentials from NVS and reconnecting.
  logMessage("WiFi disconnected, attempting reconnect.");
  WiFi.disconnect();
  WiFi.begin();
}

static void connectMqtt() {
  if (strlen(mqtt_host) == 0) {
    return;
  }

  uint32_t now = millis();
  uint32_t interval = stopped ? MQTT_RECONNECT_INTERVAL_STOPPED_MS : MQTT_RECONNECT_INTERVAL_RUNNING_MS;
  if (now - last_mqtt_reconnect_attempt_ms < interval) {
    return;
  }
  last_mqtt_reconnect_attempt_ms = now;

  // Probe the broker with a throwaway TCP connection before calling
  // mqtt_client.connect(). If the broker is unreachable, connect() would block
  // for up to MQTT_SOCKET_TIMEOUT seconds, which could cause a missed boundary
  // pulse. The probe uses a 100ms timeout so it fails fast when the broker is
  // down, making it safe to attempt reconnection while the clock is running.
  WiFiClient probe_client;
  if (!probe_client.connect(mqtt_host, mqtt_port, 100)) {
    logMessagef("MQTT probe failed, broker unreachable at %s:%d", mqtt_host, mqtt_port);
    return;
  }
  probe_client.stop();

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
  if (mode_change_pending) {
    current_mode = pending_mode;
    mode_change_pending = false;
    logMessagef("Mode changed to: %s", modeToString(current_mode));
    publishCurrentMode();
  }
}

// Called at each minute boundary to reset state for the new minute.
// set_displayed_time_from_ntp is true when called from the
// start_at_minute_pending path (boot or re-sync after calibration), where
// we want to anchor displayed_time to the actual NTP time. On regular per-minute
// boundaries the boundary pulse already incremented displayed_time correctly, so
// no reset is needed.
static void startNewMinute(bool set_displayed_time_from_ntp) {
  if (set_displayed_time_from_ntp) {
    // The boundary pulse just fired, so the hand is now at p00. Read NTP to
    // anchor displayed_time to the real hour and minute. Seconds are 0 because
    // this fires at a minute boundary.
    struct timeval ntp_tv;
    gettimeofday(&ntp_tv, nullptr);
    struct tm ntp_tm;
    localtime_r(&ntp_tv.tv_sec, &ntp_tm);
    displayed_time = (uint32_t)((ntp_tm.tm_hour % 12) * 3600 +
                                ntp_tm.tm_min * 60);
  }

  // At the top of every hour, pick a new random timekeeping mode before
  // filling the tick table. This means the new mode is in effect for the
  // entire new minute, with no wasted fill of the old mode's table.
  // Manual MQTT mode changes still work — they just get overridden at the
  // next hour boundary.
  struct timeval tv;
  gettimeofday(&tv, nullptr);
  struct tm timeinfo;
  localtime_r(&tv.tv_sec, &timeinfo);
  if (timeinfo.tm_min == 0) {
    selectRandomTimekeepingMode();
  }

  fillTickDurations();
}

// --- Arduino entrypoints ---

void setup() {
  Serial.begin(115200);

  pinMode(PIN_COIL_A, OUTPUT);
  pinMode(PIN_COIL_B, OUTPUT);
  setCoilIdle();
  gpio_set_drive_capability((gpio_num_t)PIN_COIL_A, GPIO_DRIVE_CAP_0);
  gpio_set_drive_capability((gpio_num_t)PIN_COIL_B, GPIO_DRIVE_CAP_0);

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
  // 500ms backstop on the underlying TCP socket so that if the probe succeeds
  // but the CONNACK is slow, the handshake still times out well within the
  // ~940ms gap between the boundary check and the next tick. MQTT_SOCKET_TIMEOUT
  // (set via build_flags) caps the PubSubClient read loop to 2s as a second layer.
  wifi_client.setTimeout(500);
  mqtt_client.setServer(mqtt_host, mqtt_port);
  mqtt_client.setCallback(onMqttMessage);

  randomSeed(esp_random());

  // Pick the initial timekeeping mode randomly so every boot starts with a
  // different feel. This runs before MQTT connects, so the published state
  // will be overwritten once MQTT connects and publishCurrentMode() is called
  // again from connectMqtt(). That's fine — the mode is already set correctly.
  selectRandomTimekeepingMode();

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
  if ((displayed_time % 60) == 59 && !stopped && !is_calibrating) {
    if (getMsIntoMinute() < 500) {
      pulseOnce();
      logBoundaryPulse();
      onRevolutionComplete();
      if (!stopped) {
        startNewMinute(false);
      }
      return;
    }
  }

  if (WiFi.status() == WL_CONNECTED) {
    // Attempt MQTT (re)connection regardless of clock state. connectMqtt() opens
    // a 100ms TCP probe first; if the broker is unreachable it returns immediately
    // without blocking, so the p59 boundary window is safe. The 60s reconnect
    // interval keeps probe overhead negligible.
    if (!mqtt_client.connected()) {
      connectMqtt();
    }
    mqtt_client.loop();
  } else {
    reconnectWifi();
  }

  if (start_at_minute_pending) {
    // Poll NTP until the second rolls over to 0, then start. When
    // calibration_target_minute is set (WAIT path), only fire at the specific
    // target minute rather than the next available minute boundary.
    if (getMsIntoMinute() < 1000) {
      if (calibration_target_minute != NO_CALIBRATION_TARGET) {
        uint32_t ntp_minute = getNtpTimeIn12hCycle();
        // Strip seconds to get the minute boundary value.
        ntp_minute = (ntp_minute / 60) * 60;
        if (ntp_minute != calibration_target_minute) {
          return;
        }
        calibration_target_minute = NO_CALIBRATION_TARGET;
      }
      if (mode_change_pending) {
        current_mode = pending_mode;
        mode_change_pending = false;
        logMessagef("Mode changed to: %s", modeToString(current_mode));
        publishCurrentMode();
      }
      stopped = false;
      start_at_minute_pending = false;
      // Fire the p59→p00 boundary pulse before starting the new minute.
      // The hand is always at p59 when this path runs: on boot the hand is
      // assumed to be at p59, and calibration sprints to p59 before setting
      // start_at_minute_pending.
      pulseOnce();
      logBoundaryPulse();
      startNewMinute(true); // set displayed_time from NTP, fill tick_durations
      logMessage("Minute boundary reached, clock started.");
    }
    return;
  }

  if (stopped) {
    return;
  }

  // Calibration sprint: pulse at CALIBRATE_SPRINT_MS until displayed_time
  // catches up to NTP (SPRINT path) or until p59 is reached (WAIT path).
  if (is_calibrating) {
    uint8_t seconds = (uint8_t)(displayed_time % 60);

    if (seconds == 59) {
      if (calibration_target_minute != NO_CALIBRATION_TARGET) {
        // WAIT path: we've reached p59. Stop and wait for the target minute.
        is_calibrating = false;
        stopped = true;
        start_at_minute_pending = true;
        logMessagef("Calibration reached p59, waiting for minute %lus.",
                    (unsigned long)calibration_target_minute);
        return;
      }

      // SPRINT path: check if displayed_time has caught up to or passed NTP.
      // A forward distance > 21600 (half the 12h cycle) means displayed_time
      // is now ahead of NTP — we've overshot. Distance == 0 means exact match.
      uint32_t ntp_time = getNtpTimeIn12hCycle();
      uint32_t forward_distance = (ntp_time - displayed_time + 43200) % 43200;
      if (forward_distance == 0 || forward_distance > 21600) {
        is_calibrating = false;
        stopped = true;
        start_at_minute_pending = true;
        logMessage("Calibration sprint complete, waiting for minute boundary.");
        return;
      }
      // Still behind NTP; continue sprinting through another revolution.
    }

    pulseOnce();
    delay(CALIBRATE_SPRINT_MS - PULSE_MS);
    return;
  }

  if ((displayed_time % 60) < 59) {
    uint16_t duration = tick_durations[displayed_time % 60];
    delay(duration - PULSE_MS);
    pulseOnce();
  }
  // (displayed_time % 60) == 59: the boundary check at the top of loop()
  // handles this case; nothing to do here.
}
