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

One active build target defined in `platformio.ini`:
- `vetinari`: Full firmware with WiFi, NTP, MQTT, and all tick modes.

### Pulse model

The firmware drives a Lavet motor with alternating-polarity 31 ms pulses, one per second mark, for 60 pulses per full revolution of the second hand.

- `PULSES_PER_REVOLUTION` = 60 (`src/main.cpp` line 12)
- `PULSE_MS` = 31 ms (`src/main.cpp` line 14)
- `TICK_COUNT` = 59 — the number of ticks governed by the `tick_durations` table per minute (`src/main.cpp` line 19)
- Default tick durations for positioning modes (`src/main.cpp` lines 16–17):
  - `SPRINT_DEFAULT_MS` = 300 ms total tick (used when no parameter is given)
  - `CRAWL_DEFAULT_MS` = 2000 ms total tick (used when no parameter is given)
  - `CALIBRATE_SPRINT_MS` = 200 ms total tick (fixed speed used during `calibrate` sprints; not user-configurable)
- `positioning_tick_ms` (`src/main.cpp` line 79): runtime variable holding the active tick duration for the current positioning mode; set on every sprint/crawl activation

Timekeeping modes use `tick_durations[]` (total wall-clock duration per tick, including the pulse). The gap after the pulse is `tick_durations[pulse_index] - PULSE_MS`.

### Modes

| Mode | Type | NTP-anchored | Activates |
|---|---|---|---|
| `steady` | Timekeeping | Yes | At next revolution boundary |
| `rush_wait` | Timekeeping | Yes | At next revolution boundary |
| `vetinari` | Timekeeping | Yes | At next revolution boundary |
| `hesitate` | Timekeeping | Yes | At next revolution boundary |
| `stumble` | Timekeeping | Yes | At next revolution boundary |
| `sprint` | Positioning | No | Immediately |
| `crawl` | Positioning | No | Immediately |

`isTimekeeping()` (`src/main.cpp` line 171) returns true for steady/rush_wait/vetinari/hesitate/stumble and false for sprint/crawl.

Default mode on boot: `vetinari`.

### Tick duration table

`tick_durations[TICK_COUNT]` is a 59-element array of `uint16_t` total wall-clock durations (ms). Filled by `fillTickDurations()` at the start of each minute:

- `steady`: all 59 entries = 1000 ms
- `rush_wait`: all 59 entries = 932 ms (~55 s total, ~5 s idle before minute boundary)
- `vetinari`: Fisher-Yates shuffle of `VETINARI_TEMPLATE` (534–2001 ms, sorted ascending in the template)
- `hesitate`: 58 entries of 980 ms and 1 entry of 2000 ms, Fisher-Yates shuffled each minute
- `stumble`: 58 entries of 1010 ms and 1 entry of 420 ms, Fisher-Yates shuffled each minute
- Positioning modes: table is not used

### Vetinari mode

59 pulses with shuffled irregular durations, plus a 60th pulse fired exactly at the NTP minute boundary.

- Template: 59 sorted `uint16_t` total-duration values (534–2001 ms) in `VETINARI_TEMPLATE` (`src/main.cpp` lines 25–32)
- Shuffled each minute via Fisher-Yates into `tick_durations[]` (`src/main.cpp` lines 211–217)
- `getGapMs()` does not exist; the gap is computed inline as `tick_durations[pulse_index] - PULSE_MS`
- Index 59 (the 60th pulse) never reads `tick_durations` — it waits for the NTP boundary instead

### Hesitate and stumble modes

Both modes follow the same structure as vetinari — 59 programmatically generated entries shuffled each minute — but use a simpler two-value tick table rather than a hand-crafted template.

**Hesitate**: 58 entries of 980 ms and 1 entry of 2000 ms. The single long tick lands at a random position each minute, causing the hand to pause noticeably for ~2 seconds before continuing.

**Stumble**: 58 entries of 1010 ms and 1 entry of 420 ms. The single short tick lands at a random position each minute, causing the hand to jump forward quickly as if stumbling.

Both tables sum to the same ~58 s total as the other timekeeping modes, leaving the remainder as idle time before the NTP-anchored 60th pulse.

### Timing and minute synchronization

- `minute_start_ms` holds the `millis()` timestamp of the current minute boundary (`src/main.cpp` line 100)
- All timekeeping modes produce exactly 60 pulses per minute, anchored to NTP
- **Pulse 59 (the last pulse)** is special: the loop spins until `millis() - minute_start_ms >= 60000`, then fires the pulse and calls `onRevolutionComplete()` (`src/main.cpp` lines 590–597)
- After `onRevolutionComplete()`, `minute_start_ms += 60000` (not re-read from NTP) to avoid drift from NTP query latency
- `startNewMinute()` resets `pulse_index = 0` and refills `tick_durations` (`src/main.cpp` lines 459–462)
- `getMsIntoMinute()` reads `gettimeofday()` and returns `tm_sec * 1000 + tv_usec / 1000` (`src/main.cpp` lines 242–248)
- On boot, the firmware waits for `getMsIntoMinute() < 1000` (i.e. the first second of a new minute) before starting (`src/main.cpp` lines 530–573)
- `start_at_minute_pending` flag drives this wait; it is set on boot and whenever switching from a positioning mode back to a timekeeping mode

### Sprint and crawl (positioning modes)

- Activate immediately when commanded, bypassing the revolution-boundary queue
- Both modes accept an optional tick-duration parameter in milliseconds (e.g. `sprint 150`, `crawl 500`). The value is clamped to a minimum of 50 ms. Without a parameter, `SPRINT_DEFAULT_MS` (300) or `CRAWL_DEFAULT_MS` (2000) is used.
- Run continuously without NTP sync: `pulseOnce()` + `delay(positioning_tick_ms - PULSE_MS)`, wrapping `pulse_index` at `PULSES_PER_REVOLUTION` (`src/main.cpp` lines 599–609)
- When switching back to a timekeeping mode, `onRevolutionComplete()` sets `stopped = true` and `start_at_minute_pending = true`, so the clock waits for the next NTP minute boundary before resuming (`src/main.cpp` lines 448–454)

### `pulse_index` invariant

`pulse_index` must **only** be reset by:
1. The `start` MQTT command (`src/main.cpp` line 280)
2. `startNewMinute()` at each minute boundary (`src/main.cpp` line 460)
3. The positioning-mode wrap in `loop()` (`src/main.cpp` line 607) — this is the only other reset, and it is intentional for sprint/crawl which run without NTP sync
4. The `calibrate <position>` MQTT command — sets `pulse_index` to the user-supplied position so the sprint loop counts the remaining pulses correctly and wraps at exactly p00. This is intentional: the user is asserting the physical hand position.

It must never be reset elsewhere. This is a hard constraint from `AGENTS.md`.

### MQTT command handling

All commands arrive on topic `clock/mode/set` via `onMqttMessage()` (`src/main.cpp` lines 260–399).

Control commands (handled first, before mode parsing):

| Command | Effect |
|---|---|
| `stop` | Sets `stopped = true`, clears `start_at_minute_pending` |
| `start` | Sets `stopped = false`, resets `pulse_index = 0`, sets `minute_start_ms = millis()` |
| `start_at_minute` | Sets `start_at_minute_pending = true`, clears `stop_at_top_pending` |
| `stop_at_top` | Sets `stop_at_top_pending = true`, clears `start_at_minute_pending` |
| `calibrate <position>` | Sets `pulse_index` to the given position (0–59), then sprints to p00 and queues a return to `last_timekeeping_mode`. Position 0 skips sprint and waits for the minute boundary directly. Position ≥ 60 is rejected. |

Mode commands (parsed by `stringToMode()` for bare names, or by prefix matching for parameterized forms):

- Positioning modes (`sprint`, `crawl`): applied immediately, all blocking state cleared; `positioning_tick_ms` is set to the default (`SPRINT_DEFAULT_MS` or `CRAWL_DEFAULT_MS`)
- Positioning modes with duration (`sprint <ms>`, `crawl <ms>`): same as above, but `positioning_tick_ms` is set to the given value, clamped to a minimum of 100 ms
- Timekeeping modes when `stopped`: applied immediately, `start_at_minute_pending = true`
- Timekeeping modes when running: queued in `pending_mode` / `mode_change_pending`, applied at next revolution boundary via `onRevolutionComplete()`

Current mode is published retained to `clock/mode/state` after every change.

### MQTT idle window

MQTT (re)connection is only attempted when `stopped` is true or `pulse_index == 59` (the idle wait at the minute boundary), so a slow broker never stalls pulse timing (`src/main.cpp` lines 540–543).

### GPIO drive strength

- Both coil pins (GPIO 5 and 6) are set to `GPIO_DRIVE_CAP_0` (5 mA) — the minimum, because the 820 Ω series resistor limits current to ~4 mA at 3.3 V anyway.
- Evidence: `src/main.cpp` lines 477–478

### Error handling

- No exception handling (embedded C++ without exceptions)
- Defensive checks for MQTT connection state before publishing
- NTP sync has a 10 s timeout; the clock proceeds even if sync fails

### Logging

- Dual output: Serial (115200 baud) and UDP broadcast (port 37243)
- UDP logging only when WiFi is connected
- Format: `(millis - IP): message`
- Helpers: `logMessage()` and `logMessagef()` (`src/main.cpp` lines 104–127)
- Per-tick status line logged after every pulse: `tick <pulse_index> t=<duration_ms> time=HH:MM:SS.cc` (pulse 59 logs `t=0` since it has no `tick_durations` entry — it waits for the NTP boundary instead)

### Configuration storage

- `Preferences` library for persistent flash storage, namespace `"clock"`
- Stored values: `mqtt_host` (string), `mqtt_port` (uint16)
- WiFiManager captive portal for initial configuration; portal times out after 180 s


## Linting and testing commands

No linting, formatting, or testing infrastructure. This is typical for embedded Arduino projects.

**Build commands** (from `platformio.ini` and PlatformIO conventions):
- `pio run -e vetinari` — build full firmware
- `pio run -e vetinari -t upload` — upload to device

**Monitoring**:
- Serial: `pio device monitor`
- UDP logs: `nc -kul 37243` (from `README.md`)


## Project structure hotspots

- `src/main.cpp` (615 lines) — Full firmware: WiFi, NTP, MQTT, all tick modes, minute-boundary synchronization.
- `platformio.ini` — Build configuration with one active environment (`vetinari`).
- `README.md` — Comprehensive documentation of hardware, modes, MQTT API, and configuration constants.
- `AGENTS.md` — Development constraints (especially the `pulse_index` reset rule) and documentation maintenance rules.
- `misc/coding-team/` — Task spec documents for AI coding agents; not compiled. Six completed task series:
  - `ticking-mode-refactor/` — Replaced 960-pulse/16-burst model with 60-pulse/gap model
  - `centralized-tick-table/` — Introduced `tick_durations[]` table and p00/t00 terminology
  - `sprint-crawl-immediate-start/` — Made positioning modes activate immediately and added `last_timekeeping_mode` fallback
  - `sprint-crawl-parameter/` — Added optional tick-duration parameter to sprint/crawl commands
  - `calibrate-command/` — Added `calibrate <position>` command for hand re-synchronization
  - `pin-change-status-print-cleanup/` — Changed coil pins from 4/5 to 5/6, added per-tick status logging, removed `simple.cpp` and its build environment

**Key boundaries**:
- `src/` — all application code; single source file with no shared headers
- `.pio/` — build artifacts (gitignored)
- `misc/coding-team/` — task specs for AI coding agents; not compiled


## Do and don't patterns

### Do: Anchor all timekeeping to `minute_start_ms`, not raw `millis()`

Pulse scheduling for timekeeping modes is relative to `minute_start_ms`, which is set once at the minute boundary and incremented by exactly 60000 ms each revolution. This prevents drift from loop jitter or NTP query latency.
- Evidence: `src/main.cpp` lines 101, 569, 591

### Do: Defer blocking operations to the idle window

MQTT reconnection only happens when `stopped || pulse_index == 59`. The blocking `connect()` call cannot stall pulse timing.
- Evidence: `src/main.cpp` lines 540–543

### Do: Apply timekeeping mode changes at revolution boundaries

Timekeeping mode changes are queued in `pending_mode` / `mode_change_pending` and applied in `onRevolutionComplete()`, so the clock never starts a new mode mid-revolution.
- Evidence: `src/main.cpp` lines 70–72, 432–456

### Do: Activate positioning modes immediately

Positioning modes (`sprint`, `crawl`) bypass the revolution-boundary queue because they don't need NTP sync. All pending blocking state (`start_at_minute_pending`, `stop_at_top_pending`) is cleared.
- Evidence: `src/main.cpp` lines 364–379

### Do: Re-sync to NTP after leaving a positioning mode

When switching from sprint/crawl back to a timekeeping mode, `onRevolutionComplete()` sets `start_at_minute_pending = true` so the clock waits for the next minute boundary.
- Evidence: `src/main.cpp` lines 448–454

### Do: Fall back to `last_timekeeping_mode` at minute boundary if in a positioning mode

If `start_at_minute_pending` fires while `current_mode` is sprint or crawl, the clock falls back to `last_timekeeping_mode` rather than running an unsynchronized positioning mode.
- Evidence: `src/main.cpp` lines 558–565

### Don't: Reset `pulse_index` except in `start`, `startNewMinute()`, the positioning-mode wrap, or `calibrate`

Resetting `pulse_index` at any other point breaks NTP synchronization. This is an explicit constraint in `AGENTS.md`.
- Evidence: `AGENTS.md` line 6, `src/main.cpp` lines 280, 460, 612, 323

### Don't: Read NTP time to advance `minute_start_ms`

`minute_start_ms` is advanced by `+= 60000`, not by re-reading `gettimeofday()`. Re-reading would introduce jitter from the NTP query itself.
- Evidence: `src/main.cpp` line 557

### Don't: Block the main loop during active pulsing

All delays are calculated from gap constants or `tick_durations[]`, not arbitrary waits. MQTT operations are deferred. The only intentional blocking `delay()` calls are the pulse duration itself and the inter-pulse gap.
- Evidence: `src/main.cpp` lines 585–586, 603–604


## Open questions

1. **README pin numbers are stale**: `README.md` still documents `PIN_COIL_A = 4` and `PIN_COIL_B = 5`, but the source was changed to 5 and 6 by the `pin-change-status-print-cleanup` task. The README hardware wiring diagram needs updating to reflect GPIO 5 and 6.
