# Repository scout report

## Detected stack

- **Languages**: C++ (Arduino framework)
  - Evidence: `src/main.cpp`
- **Platform**: ESP32-C3 microcontroller (espressif32)
  - Evidence: `platformio.ini` lines 2–3, board `esp32-c3-devkitm-1`
- **Framework**: Arduino
  - Evidence: `platformio.ini` line 4, `#include <Arduino.h>` in all source files
- **Build system**: PlatformIO
  - Evidence: `platformio.ini`, `.pio/` directory
- **Libraries**:
  - WiFiManager (https://github.com/tzapu/WiFiManager.git) — WiFi configuration via captive portal
  - PubSubClient (v2.8) — MQTT client
  - Arduino core libraries: WiFi, Preferences, WiFiUdp, time.h, driver/gpio.h
  - Evidence: `platformio.ini` lines 9–10, `src/main.cpp` includes


## Conventions

### Build environments

Two active build targets defined in `platformio.ini`:
- `sleight`: Full firmware with WiFi, NTP, MQTT, and all tick modes.
- `sleight-ota`: Same as `sleight` but uploads via OTA to `sleight-of-hand.local`.

### Pulse model

The firmware drives a Lavet motor with alternating-polarity 31 ms pulses, one per second mark, for 60 pulses per full revolution of the second hand.

- `PULSES_PER_REVOLUTION` = 60 (`src/main.cpp` line 14)
- `PULSE_MS` = 31 ms (`src/main.cpp` line 16)
- `TICK_COUNT` = 59 — the number of ticks governed by the `tick_durations` table per minute (`src/main.cpp` line 23)
- `CALIBRATE_SPRINT_MS` = 200 ms total tick (fixed speed used during `calibrate` sprints; not user-configurable)
- `RUSH_WAIT_DEFAULT_MS` = 700 ms total tick (default for `rush_wait` mode; used when bare `rush_wait` is commanded)
- `rush_wait_tick_ms` (`src/main.cpp`): runtime variable holding the active tick duration for `rush_wait` mode; defaults to `RUSH_WAIT_DEFAULT_MS`, configurable via `rush_wait <ms>` MQTT command

Timekeeping modes use `tick_durations[]` (total wall-clock duration per tick, including the pulse). The gap after the pulse is `tick_durations[displayed_time % 60] - PULSE_MS`.

### Modes

| Mode | NTP-anchored | Activates |
|---|---|---|
| `steady` | Yes | At next revolution boundary |
| `rush_wait` | Yes | At next revolution boundary |
| `vetinari` | Yes | At next revolution boundary |
| `hesitate` | Yes | At next revolution boundary |
| `stumble` | Yes | At next revolution boundary |
| `gravity` | Yes | At next revolution boundary |

All modes are timekeeping modes. There are no positioning modes.

Default mode on boot: random (selected by `selectRandomTimekeepingMode()` in `setup()`).

### Tick duration table

`tick_durations[TICK_COUNT]` is a 59-element array of `uint16_t` total wall-clock durations (ms). Filled by `fillTickDurations()` at the start of each minute:

- `steady`: all 59 entries = 1000 ms
- `rush_wait`: all 59 entries = `rush_wait_tick_ms` (default 700 ms, configurable via `rush_wait <ms>` command; 59 × 700 = 41,300 ms total at default, ~18,700 ms idle before minute boundary)
- `vetinari`: Fisher-Yates shuffle of `VETINARI_TEMPLATE` (534–2001 ms, sorted ascending in the template)
- `hesitate`: 58 entries of 980 ms and 1 entry of 2000 ms, Fisher-Yates shuffled each minute
- `stumble`: 58 entries of 1010 ms and 1 entry of 420 ms, Fisher-Yates shuffled each minute
- `gravity`: 59 entries computed from pendulum physics (`1/sqrt(1 + 0.05 - cos(θ))`), scaled to sum to 59,000 ms; not shuffled (positional mapping is the point)

### Vetinari mode

59 pulses with shuffled irregular durations, plus a 60th pulse fired exactly at the NTP minute boundary.

- Template: 59 sorted `uint16_t` total-duration values (534–2001 ms) in `VETINARI_TEMPLATE` (`src/main.cpp` lines 29–36)
- Shuffled each minute via Fisher-Yates into `tick_durations[]` (`src/main.cpp` lines 288–294)
- The gap is computed inline as `tick_durations[displayed_time % 60] - PULSE_MS`
- Index 59 (the 60th pulse) never reads `tick_durations` — it waits for the NTP boundary instead

### Hesitate and stumble modes

Both modes follow the same structure as vetinari — 59 programmatically generated entries shuffled each minute — but use a simpler two-value tick table rather than a hand-crafted template.

**Hesitate**: 58 entries of 980 ms and 1 entry of 2000 ms. The single long tick lands at a random position each minute, causing the hand to pause noticeably for ~2 seconds before continuing.

**Stumble**: 58 entries of 1010 ms and 1 entry of 420 ms. The single short tick lands at a random position each minute, causing the hand to jump forward quickly as if stumbling.

Both tables sum to the same ~58 s total as the other timekeeping modes, leaving the remainder as idle time before the NTP-anchored 60th pulse.

### Gravity mode

Gravity mode simulates a clock hand driven by pendulum physics: the hand moves fast near 6 o'clock (maximum kinetic energy) and slow near 12 o'clock (minimum kinetic energy).

- Each tick duration is computed from `1 / sqrt(1 + ENERGY_EXCESS - cos(θ))`, where `θ` is the angular position of the tick centre (in radians, measured from 12 o'clock). The raw values are scaled so their sum equals `BUDGET_MS` (59,000 ms).
- `ENERGY_EXCESS = 0.05` prevents the singularity at 12 o'clock (where a pendulum with exactly the minimum energy would take infinite time) and produces a ~6.3× ratio between the slowest tick (near 12, ~3150 ms) and the fastest (near 6, ~500 ms).
- No shuffle — the positional mapping is the whole point; the speed difference must align with the physical dial position.
- Total budget: 59,000 ms, leaving ~1,000 ms idle before the NTP-anchored 60th pulse.
- Evidence: `src/main.cpp` (`fillTickDurations()` gravity case)

### Timing and minute synchronization

- All timekeeping modes produce exactly 60 pulses per minute, anchored to NTP
  - **Ticks 0–58** use a delay-first loop body: `delay(tick_durations[displayed_time % 60] - PULSE_MS)` then `pulseOnce()`. The delay fires first so the pulse lands at the scheduled wall-clock time.
  - **Pulse 59 (the boundary pulse)** is special: the loop spins until `getMsIntoMinute() < 500`, then fires `pulseOnce()`, calls `onRevolutionComplete()`, and starts the next minute via `startNewMinute(false)`. No `tick_durations` entry is consumed for the boundary pulse.
  - `startNewMinute(set_displayed_time_from_ntp)` refills `tick_durations`. When called from the `start_at_minute_pending` path (boot or re-sync after calibration), it also sets `displayed_time` from NTP. On regular per-minute boundaries the boundary pulse already incremented `displayed_time` correctly, so no reset is needed.
- `getMsIntoMinute()` reads `gettimeofday()` and returns `tm_sec * 1000 + tv_usec / 1000` (`src/main.cpp`). This is the single boundary-detection mechanism used everywhere.
- On boot, the firmware waits for `getMsIntoMinute() < 1000` (i.e. the first second of a new minute) before starting.
- `start_at_minute_pending` flag drives this wait; it is set on boot and whenever calibration completes at p59. When the boundary fires, `pulseOnce()` fires the p59→p00 boundary tick, then `startNewMinute(true)` sets `displayed_time` from NTP and fills `tick_durations`. **p59 invariant**: the hand is always at p59 when this path runs. On boot the hand is assumed to be at p59. Calibration sprints to p59 before setting `start_at_minute_pending`.

### Timezone

The firmware runs entirely on UTC (`UTC_OFFSET_SECONDS = 0`). All `localtime_r()` calls therefore return UTC, not local wall-clock time.

- `calibrate H:MM:SS` expects UTC time.
- `calibrate <position>` anchors to the UTC hour/minute from NTP.
- Hourly random mode rotation fires at the UTC top-of-hour (`tm_min == 0`).

To use a local timezone instead, replace `configTime()` in `setup()` with `configTzTime()` and pass a POSIX TZ string (e.g. `"EET-2EEST,M3.5.0/3,M10.5.0/4"`) as the first argument.

### Calibration

Calibration is triggered by either form of the `calibrate` command. It replaces the old sprint/crawl positioning modes for the purpose of re-synchronising the hand to NTP.

**State variables:**
- `is_calibrating` (`bool`): true while a calibration sprint is active.
- `calibration_target_minute` (`uint32_t`): for the WAIT path, the NTP minute boundary (seconds in 12h cycle, seconds component = 0) that `start_at_minute_pending` must match before firing. `NO_CALIBRATION_TARGET` (`UINT32_MAX`) means SPRINT path (fire at the next available minute boundary). `UINT32_MAX` is used rather than `0` because `0` is a valid 12h-cycle minute (12:00:00).

**Trigger logic (shared by both command forms):**
1. Unconditionally cancel any in-progress calibration first: set `is_calibrating = false`, `calibration_target_minute = NO_CALIBRATION_TARGET`. This ensures every `calibrate` command supersedes the previous one cleanly, including the `d == 0` early-return path.
2. Set `displayed_time` from the command argument.
3. Compute `ntp_time` = `getNtpTimeIn12hCycle()`.
4. Compute `forward_distance` = `(ntp_time - displayed_time + 43200) % 43200`.
5. If `forward_distance == 0`: already correct, no calibration needed (return; `is_calibrating` stays false).
6. If `forward_distance <= 10800` (≤ 3 h): **SPRINT path** — set `is_calibrating = true`; `calibration_target_minute` stays `NO_CALIBRATION_TARGET`.
7. If `forward_distance > 10800`: **WAIT path** — set `is_calibrating = true`, compute `calibration_target_minute = ((displayed_time / 60 + 1) * 60) % 43200` (the minute boundary after the current displayed minute, which is where p59 will be when the sprint finishes).

**Main loop calibration branch** (runs before the timekeeping tick logic, after the boundary check):
- If `displayed_time % 60 == 59`:
  - WAIT path (`calibration_target_minute != 0`): set `is_calibrating = false`, `stopped = true`, `start_at_minute_pending = true`. The `start_at_minute_pending` logic will wait for the specific target minute before firing.
  - SPRINT path: compute `forward_distance` against live NTP. If `forward_distance == 0` or `forward_distance > 21600` (overshot past the halfway point), set `is_calibrating = false`, `stopped = true`, `start_at_minute_pending = true`. Otherwise continue sprinting through another revolution.
- Otherwise: `pulseOnce()`, `delay(CALIBRATE_SPRINT_MS - PULSE_MS)`, return.

**`start_at_minute_pending` with WAIT path**: when `calibration_target_minute != NO_CALIBRATION_TARGET`, the pending logic checks `(getNtpTimeIn12hCycle() / 60) * 60 == calibration_target_minute` before firing. If the minute doesn't match, it returns without firing. When it does fire, `calibration_target_minute` is reset to `NO_CALIBRATION_TARGET`.

**Command interaction during calibration**: mode change MQTT commands queue normally via `pending_mode`/`mode_change_pending`. The pending mode is applied in `onRevolutionComplete()` when calibration completes at the minute boundary. Any new `calibrate` command unconditionally cancels the current calibration before starting fresh (including the `d == 0` no-op path). `start`, `stop`, and `start_at_minute` commands also cancel calibration.

### `calibrate <position>` command

Sets `displayed_time` to `(ntp_hour % 12) * 3600 + ntp_minute * 60 + position` for positions 0–58. The calibration branch stops at `displayed_time % 60 == 59` without pulsing, so starting at `position` fires exactly `59 - position` pulses to land on p59. Position 59 sets `displayed_time % 60 == 59` and waits for the minute boundary directly (no sprint needed). Position ≥ 60 is rejected.

After setting `displayed_time`, the SPRINT/WAIT convergence logic runs as described above.

### `calibrate H:MM:SS` command

Accepts a flexible hour (1 or 2 digits) with mandatory 2-digit MM and SS. Sets `displayed_time = (H % 12) * 3600 + MM * 60 + SS`, then runs the same SPRINT/WAIT convergence logic. The time must be in UTC (or whatever timezone is configured via `UTC_OFFSET_SECONDS`).

### `displayed_time` invariant

`displayed_time` is a `uint32_t` tracking seconds past 12:00:00 in a 12-hour cycle (range 0–43199). It is incremented mod 43200 on every `pulseOnce()` call. The second-hand position within the current minute is `(displayed_time % 60)`.

`displayed_time` must **only** be explicitly set by:
1. The `start` MQTT command — resets to 0
2. `startNewMinute(true)` at the first NTP-anchored minute boundary (boot or re-sync) — sets from NTP: `(hour % 12) * 3600 + minute * 60`
3. The `calibrate` MQTT command (either form) — sets from the command argument

It must never be explicitly set elsewhere. All other changes happen via `pulseOnce()` incrementing it.

### Hourly random mode rotation

On every boot and at every top-of-hour minute boundary, `selectRandomTimekeepingMode()` picks a random entry from `TIMEKEEPING_MODES[]` and sets `current_mode` to it. If the chosen mode is `rush_wait`, `rush_wait_tick_ms` is reset to `RUSH_WAIT_DEFAULT_MS`. The selection is logged and published via MQTT.

- `TIMEKEEPING_MODES[]` (`src/main.cpp`) — `constexpr` array of all six timekeeping `TickMode` values; the single source of truth for the random picker.
- `TIMEKEEPING_MODE_COUNT` — derived from `sizeof(TIMEKEEPING_MODES)` so it stays in sync automatically.
- On boot: called in `setup()` after `randomSeed()`, before `stopped = true; start_at_minute_pending = true`.
- Hourly: called inside `startNewMinute()` when `tm_min == 0`, before `fillTickDurations()`, so the new mode's tick table is filled without a redundant fill of the old mode.
- Manual MQTT mode changes still work as before; the next hour boundary overrides them.
- `displayed_time` is never explicitly set by this feature.

### MQTT command handling

All commands arrive on topic `clock/mode/set` via `onMqttMessage()` (`src/main.cpp`).

Control commands (handled first, before mode parsing):

| Command | Effect |
|---|---|
| `stop` | Sets `stopped = true`, clears `start_at_minute_pending`, cancels calibration |
| `start` | Sets `stopped = false`, resets `displayed_time = 0`, cancels calibration |
| `start_at_minute` | Cancels calibration, sets `start_at_minute_pending = true` |
| `calibrate <position>` | For positions 0–58, sets `displayed_time` and triggers SPRINT/WAIT convergence. Position 59 sets `displayed_time % 60 == 59` and waits for the minute boundary directly. Position ≥ 60 is rejected. |
| `calibrate H:MM:SS` | Sets `displayed_time` from parsed time and triggers SPRINT/WAIT convergence. |

Mode commands (parsed by `stringToMode()` for bare names, or by prefix matching for parameterized forms):

- `rush_wait <ms>`: sets `rush_wait_tick_ms` to the given value (minimum 200 ms), then queues or applies the mode change as a normal timekeeping mode; bare `rush_wait` reverts `rush_wait_tick_ms` to `RUSH_WAIT_DEFAULT_MS` (700 ms)
- Timekeeping modes when `stopped`: applied immediately, `start_at_minute_pending = true`
- Timekeeping modes when running: queued in `pending_mode` / `mode_change_pending`, applied at next revolution boundary via `onRevolutionComplete()`

Current mode is published retained to `clock/mode/state` after every change.

### MQTT reconnection strategy

MQTT reconnection is attempted regardless of clock state, but uses a fast-fail TCP probe to avoid blocking the boundary pulse:

1. Before calling `mqtt_client.connect()`, a throwaway `WiFiClient` probes the broker with a 100 ms timeout. If the broker is unreachable the probe fails fast and `connectMqtt()` returns immediately.
2. The underlying `WiFiClient` socket timeout is set to 500 ms in `setup()`, so even if the probe succeeds but the CONNACK is slow, the handshake times out well within the ~940 ms gap between the boundary check and the next tick.
3. The reconnect interval is 5 s when `stopped` and 60 s when running, keeping probe overhead negligible during timekeeping.
4. `mqtt_client.loop()` runs on every `loop()` iteration so message handling is always live.

Evidence: `src/main.cpp` (`connectMqtt()`, `loop()` WiFi/MQTT block).

### WiFi reconnection

When `WiFi.status() != WL_CONNECTED`, `reconnectWifi()` calls `WiFi.disconnect()` then `WiFi.begin()` (no arguments, re-reads saved credentials from NVS). Rate-limited to once every 30 s via `last_wifi_reconnect_attempt_ms`. No captive portal re-launch. Evidence: `src/main.cpp`.

### GPIO drive strength

- Both coil pins (GPIO 5 and 6) are set to `GPIO_DRIVE_CAP_0` (5 mA) — the minimum, because the 820 Ω series resistor limits current to ~4 mA at 3.3 V anyway.
- Evidence: `src/main.cpp`

### Error handling

- No exception handling (embedded C++ without exceptions)
- Defensive checks for MQTT connection state before publishing
- NTP sync has a 10 s timeout; the clock proceeds even if sync fails
- `validateTickDurationsSum()` checks that the 59-entry table sums to ≤ 59,800 ms; if it exceeds this, `stopped` is set to true and an error is logged (`src/main.cpp`)

### Logging

- Dual output: Serial (115200 baud) and UDP broadcast (port 37243)
- UDP logging only when WiFi is connected
- Format: `(millis - IP): message` with `\r\n` line ending in UDP payloads
- Helpers: `logMessage()` and `logMessagef()` (`src/main.cpp`)
- Boundary pulse time is logged via `logBoundaryPulse()` (`src/main.cpp`): `boundary time=HH:MM:SS.cc`
- No per-tick logging (removed to eliminate accumulated drift from blocking Serial writes)

### Configuration storage

- `Preferences` library for persistent flash storage, namespace `"clock"`
- Stored values: `mqtt_host` (string), `mqtt_port` (uint16)
- WiFiManager captive portal for initial configuration; portal times out after 180 s


## Linting and testing commands

No linting, formatting, or testing infrastructure. This is typical for embedded Arduino projects.

**Build commands** (from `platformio.ini` and PlatformIO conventions):
- `pio run -e sleight` — build full firmware
- `pio run -e sleight -t upload` — upload to device via USB
- `pio run -e sleight-ota -t upload` — upload to device via OTA

**Monitoring**:
- Serial: `pio device monitor`
- UDP logs: `nc -kul 37243` (from `README.md`)


## Project structure hotspots

- `src/main.cpp` (~830 lines) — Full firmware: WiFi, NTP, MQTT, all tick modes, minute-boundary synchronization, calibration engine. Single source file; no headers.
- `platformio.ini` — Build configuration with two active environments (`sleight`, `sleight-ota`).
- `README.md` — Hardware wiring, modes, MQTT API, and configuration constants. Note: `gravity` mode is implemented but missing from the README mode table.
- `AGENTS.md` — Development constraints (especially the `displayed_time` explicit-set rule) and documentation maintenance rules.
- `partitions.csv` — Custom partition table enabling OTA (two equal app partitions, no SPIFFS).
- `misc/coding-team/` — Task spec documents for AI coding agents; not compiled.

**Key boundaries**:
- `src/` — all application code; single source file with no shared headers
- `.pio/` — build artifacts (gitignored)
- `misc/coding-team/` — task specs for AI coding agents; not compiled
- `.tickets/` — open and closed work items


## Do and don't patterns

### Do: Anchor all boundary detection to `getMsIntoMinute()`, not raw `millis()`

All minute-boundary detection uses `getMsIntoMinute()` (which reads `gettimeofday()`), both for `start_at_minute_pending` and for the pulse-59 boundary wait. This avoids drift from loop jitter and eliminates the need for a `millis()`-based `minute_start_ms` variable.
- Evidence: `src/main.cpp`

### Do: Use a fast-fail TCP probe before MQTT reconnect

MQTT reconnection uses a 100 ms TCP probe before calling `mqtt_client.connect()`, so the reconnect attempt fails fast when the broker is unreachable and never blocks the boundary pulse window.
- Evidence: `src/main.cpp` (`connectMqtt()` probe logic)

### Do: Apply timekeeping mode changes at revolution boundaries

Timekeeping mode changes are queued in `pending_mode` / `mode_change_pending` and applied in `onRevolutionComplete()`, so the clock never starts a new mode mid-revolution.
- Evidence: `src/main.cpp`

### Do: Re-sync to NTP after calibration

When calibration completes (sprint catches up, or WAIT path fires at the target minute), `start_at_minute_pending = true` so the clock waits for the next minute boundary before resuming timekeeping.
- Evidence: `src/main.cpp` (calibration branch in `loop()`)

### Don't: Explicitly set `displayed_time` except in `start`, `startNewMinute(true)`, or `calibrate`

Setting `displayed_time` at any other point breaks NTP synchronization. All other changes must happen via `pulseOnce()` incrementing it. This is an explicit constraint in `AGENTS.md`.
- Evidence: `AGENTS.md`, `src/main.cpp` — `start` command, `startNewMinute(true)`, `calibrate` command

### Don't: Block the main loop during active pulsing

All delays are calculated from gap constants or `tick_durations[]`, not arbitrary waits. MQTT operations are deferred. The only intentional blocking `delay()` calls are the pulse duration itself and the inter-pulse gap.
- Evidence: `src/main.cpp`

### Don't: Add per-tick serial/UDP logging

Per-tick logging was removed because the blocking Serial write accumulated enough latency to cause drift. Only boundary pulses and mode changes are logged.
- Evidence: commit `fe5c4a7` ("Remove per-tick logging to eliminate accumulated drift")


## Open questions

- **`gravity` mode is absent from `README.md`'s mode table.** It is fully implemented in source and documented in `ARCHITECTURE.md`, but the README table stops at `stumble`. The README should be updated when convenient.
