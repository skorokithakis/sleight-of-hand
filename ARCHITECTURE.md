# Repository scout report

## Detected stack

- **Languages**: C++ (Arduino framework)
  - Evidence: `src/main.cpp`, `src/simple.cpp`
- **Platform**: ESP32-C3 microcontroller (espressif32)
  - Evidence: `platformio.ini` lines 2–3, board `esp32-c3-devkitm-1`
- **Framework**: Arduino
  - Evidence: `platformio.ini` line 4, `#include <Arduino.h>` in all source files
- **Build system**: PlatformIO
  - Evidence: `platformio.ini`, `.pio/` directory
- **Libraries**:
  - WiFiManager (https://github.com/tzapu/WiFiManager.git) — WiFi configuration via captive portal
  - PubSubClient (v2.8) — MQTT client
  - Arduino core libraries: WiFi, Preferences, WiFiUdp, time.h
  - Evidence: `platformio.ini` lines 9–10, `src/main.cpp` includes


## Conventions

### Build environments

Three separate build targets defined in `platformio.ini`:
- `vetinari`: Full firmware with WiFi, NTP, MQTT, and sweep modes (excludes `simple.cpp` and `counting.cpp`)
- `simple`: Test firmware with continuous sweep, no network features (excludes `main.cpp` and `counting.cpp`)
- `counting`: Pulse measurement firmware for calibrating movements (excluded from `build_src_filter` in both other envs; its own env is not currently defined in `platformio.ini` — see open questions)

### Pulse model (post-refactor)

The firmware uses **60 pulses per revolution** (one per second mark), each 31 ms long, followed by a mode-specific gap. There is no `TICKING_MOVEMENT` compile-time flag and no sweeping mode. All modes use the same `pulseOnce()` + `delay(getGapMs())` pattern.

- `PULSES_PER_REVOLUTION` = 60 (`src/main.cpp` line 12)
- `PULSE_MS` = 31 ms (`src/main.cpp` line 14)
- Gap constants (`src/main.cpp` lines 17–20):
  - `STEADY_GAP_MS` = 969 ms → 1 s total tick
  - `RUSH_WAIT_GAP_MS` = 901 ms → ~932 ms total, 59 pulses in ~55 s
  - `SPRINT_GAP_MS` = 269 ms → 300 ms total tick
  - `CRAWL_GAP_MS` = 1969 ms → 2 s total tick

### Modes

| Mode | Type | NTP-anchored | Activates |
|---|---|---|---|
| `steady` | Timekeeping | Yes | At next revolution boundary |
| `rush_wait` | Timekeeping | Yes | At next revolution boundary |
| `vetinari` | Timekeeping | Yes | At next revolution boundary |
| `sprint` | Positioning | No | Immediately |
| `crawl` | Positioning | No | Immediately |

`isTimekeeping()` (`src/main.cpp` line 173) returns true for steady/rush_wait/vetinari and false for sprint/crawl.

### Vetinari mode

59 pulses with shuffled irregular gaps, plus a 60th pulse fired exactly at the NTP minute boundary.

- Template: 59 sorted `uint16_t` gap values (503–1970 ms) in `VETINARI_TEMPLATE` (`src/main.cpp` lines 27–34)
- Shuffled each minute via Fisher-Yates into `vetinari_gaps[]` (`src/main.cpp` lines 39–47)
- `getGapMs()` indexes `vetinari_gaps[pulse_index]` for indices 0–58; index 59 never calls `getGapMs()` — it waits for the NTP boundary instead

### Timing and minute synchronization

- `minute_start_ms` holds the `millis()` timestamp of the current minute boundary (`src/main.cpp` line 102)
- All timekeeping modes produce exactly 60 pulses per minute, anchored to NTP
- **Pulse 59 (the last pulse)** is special: the loop spins until `millis() - minute_start_ms >= 60000`, then fires the pulse and calls `onRevolutionComplete()` (`src/main.cpp` lines 499–508)
- After `onRevolutionComplete()`, `minute_start_ms += 60000` (not re-read from NTP) to avoid drift from NTP query latency
- `startNewMinute()` resets `pulse_index = 0` and reshuffles vetinari gaps (`src/main.cpp` lines 377–382)
- `getMsIntoMinute()` reads `gettimeofday()` and returns `tm_sec * 1000 + tv_usec / 1000` (`src/main.cpp` lines 234–240)
- On boot, the firmware waits for `getMsIntoMinute() < 1000` (i.e. the first second of a new minute) before starting (`src/main.cpp` lines 468–484)
- `start_at_minute_pending` flag drives this wait; it is set on boot and whenever switching from a positioning mode back to a timekeeping mode

### Sprint and crawl (positioning modes)

- Activate immediately when commanded, bypassing the revolution-boundary queue
- Run continuously without NTP sync: `pulseOnce()` + `delay(getGapMs())`, wrapping `pulse_index` at `PULSES_PER_REVOLUTION` (`src/main.cpp` lines 511–520)
- When switching back to a timekeeping mode, `onRevolutionComplete()` sets `stopped = true` and `start_at_minute_pending = true`, so the clock waits for the next NTP minute boundary before resuming (`src/main.cpp` lines 368–372)

### `pulse_index` invariant

`pulse_index` must **only** be reset by:
1. The `start` MQTT command (`src/main.cpp` line 271)
2. `startNewMinute()` at each minute boundary (`src/main.cpp` line 378)

It must never be reset elsewhere. This is a hard constraint from `AGENTS.md`.

### MQTT idle window

MQTT (re)connection is only attempted when `stopped` is true or `pulse_index == 59` (the idle wait at the minute boundary), so a slow broker never stalls pulse timing (`src/main.cpp` lines 460–463).

### Error handling

- No exception handling (embedded C++ without exceptions)
- Defensive checks for MQTT connection state before publishing
- NTP sync has a 10 s timeout; the clock proceeds even if sync fails

### Logging

- Dual output: Serial (115200 baud) and UDP broadcast (port 37243)
- UDP logging only when WiFi is connected
- Format: `(millis - IP): message`
- Helpers: `logMessage()` and `logMessagef()` (`src/main.cpp` lines 106–129)

### Configuration storage

- `Preferences` library for persistent flash storage, namespace `"clock"`
- Stored values: `mqtt_host` (string), `mqtt_port` (uint16)
- WiFiManager captive portal for initial configuration; portal times out after 180 s


## Linting and testing commands

No linting, formatting, or testing infrastructure. This is typical for embedded Arduino projects.

**Build commands** (from `platformio.ini` and PlatformIO conventions):
- `pio run -e vetinari` — build full firmware
- `pio run -e vetinari -t upload` — upload to device
- `pio run -e simple` — build simple test firmware
- `pio device monitor` — open serial monitor (115200 baud)

**Monitoring**:
- Serial: `pio device monitor`
- UDP logs: `nc -kul 37243` (from `README.md`)


## Project structure hotspots

- `src/main.cpp` (522 lines) — Full firmware: WiFi, NTP, MQTT, all sweep modes, minute-boundary synchronization. The only file that matters for production.
- `src/simple.cpp` (53 lines) — Minimal test firmware for verifying motor operation without network complexity.
- `platformio.ini` — Build configuration with three environments.
- `README.md` — Comprehensive documentation of hardware, modes, MQTT API, and configuration constants.
- `AGENTS.md` — Development constraints (especially the `pulse_index` reset rule) and documentation maintenance rules.
- `misc/coding-team/ticking-mode-refactor/001-refactor-pulse-timing.md` — Spec document for the refactor that replaced the 960-pulse/16-burst model with the current 60-pulse/gap model. Already applied to `src/main.cpp`.

**Key boundaries**:
- `src/` — all application code; no shared headers between the two source files
- `.pio/` — build artifacts (gitignored)
- `misc/coding-team/` — task specs for AI coding agents; not compiled


## Do and don't patterns

### Do: Anchor all timekeeping to `minute_start_ms`, not raw `millis()`

Pulse scheduling for timekeeping modes is relative to `minute_start_ms`, which is set once at the minute boundary and incremented by exactly 60000 ms each revolution. This prevents drift from loop jitter or NTP query latency.
- Evidence: `src/main.cpp` lines 102, 479, 501–502

### Do: Defer blocking operations to the idle window

MQTT reconnection only happens when `stopped || pulse_index == 59`. The blocking `connect()` call cannot stall pulse timing.
- Evidence: `src/main.cpp` lines 460–463

### Do: Apply mode changes at revolution boundaries

Timekeeping mode changes are queued in `pending_mode` / `mode_change_pending` and applied in `onRevolutionComplete()`, so the clock never starts a new mode mid-revolution.
- Evidence: `src/main.cpp` lines 80–81, 351–373

### Do: Activate positioning modes immediately

Sprint and crawl bypass the revolution-boundary queue because they don't need NTP sync.
- Evidence: `src/main.cpp` lines 293–299

### Do: Re-sync to NTP after leaving a positioning mode

When switching from sprint/crawl back to a timekeeping mode, `onRevolutionComplete()` sets `start_at_minute_pending = true` so the clock waits for the next minute boundary.
- Evidence: `src/main.cpp` lines 368–372

### Don't: Reset `pulse_index` except in `start` or `startNewMinute()`

Resetting `pulse_index` at any other point breaks NTP synchronization. This is an explicit constraint in `AGENTS.md`.
- Evidence: `AGENTS.md` line 6, `src/main.cpp` lines 271, 378

### Don't: Read NTP time to advance `minute_start_ms`

`minute_start_ms` is advanced by `+= 60000`, not by re-reading `gettimeofday()`. Re-reading would introduce jitter from the NTP query itself.
- Evidence: `src/main.cpp` line 502

### Don't: Block the main loop during active pulsing

All delays are calculated from gap constants, not arbitrary waits. MQTT operations are deferred. The only intentional blocking `delay()` calls are the pulse duration itself and the inter-pulse gap.
- Evidence: `src/main.cpp` lines 495–497, 514–516


## Open questions

1. **`counting` environment missing from `platformio.ini`**: `README.md` documents `pio run -e counting` and `src/counting.cpp` is referenced in `build_src_filter`, but no `[env:counting]` section exists in `platformio.ini`. Either the environment was removed or it was never added. Clarify before attempting to build the counting firmware.

2. **`SPRINT_GAP_MS` discrepancy**: The task spec (`misc/coding-team/ticking-mode-refactor/001-refactor-pulse-timing.md` line 26) specifies `SPRINT_GAP_MS = 469`, but `src/main.cpp` line 19 has `SPRINT_GAP_MS = 269`. The README describes sprint as "500ms total tick" (31 ms pulse + 469 ms gap = 500 ms). The current value of 269 gives a 300 ms total cycle. Clarify which is correct.
