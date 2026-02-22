# Task: Refactor pulse timing model

## Context

The clock currently uses 960 pulses per revolution with 16-pulse bursts per second. We're switching to 60 pulses per revolution (one per second mark) with a fixed 31ms pulse duration and mode-specific gap durations.

## Objective

Change the pulse timing model so all modes use 31ms pulse + variable gap, and remove sweeping mode code entirely.

## Changes required

### Constants

- `PULSES_PER_REVOLUTION` = 60
- `PULSE_MS` = 31 (no longer conditional)
- Remove `TICKING_MOVEMENT` flag and all `#if`/`#else` branches for it
- Remove `STEADY_CYCLE_MS`, `RUSH_CYCLE_MS`, `SPRINT_CYCLE_MS`, `CRAWL_CYCLE_MS`

### New gap constants

```cpp
constexpr uint32_t STEADY_GAP_MS = 969;      // 1s total tick
constexpr uint32_t RUSH_WAIT_GAP_MS = 901;   // ~932ms total, 59 pulses in 55s
constexpr uint32_t SPRINT_GAP_MS = 469;      // 500ms total tick
constexpr uint32_t CRAWL_GAP_MS = 1969;      // 2s total tick
```

### Vetinari template

Replace the current 60-entry template (32-117ms values) with this 59-entry sorted template (gaps in ms):

```cpp
constexpr uint8_t VETINARI_PULSES = 59;
constexpr uint16_t VETINARI_GAPS[VETINARI_PULSES] = {
     503,  519,  521,  530,  534,  543,  543,  588,  610,  618,
     654,  655,  656,  662,  663,  666,  669,  711,  712,  713,
     766,  773,  785,  797,  832,  835,  843,  843,  852,  875,
     889,  926,  950,  953, 1030, 1046, 1065, 1077, 1098, 1159,
    1161, 1173, 1180, 1196, 1221, 1237, 1279, 1350, 1350, 1356,
    1379, 1393, 1457, 1598, 1614, 1653, 1698, 1742, 1970,
};
```

Update `shuffleVetinariCycles()` to work with 59 `uint16_t` entries instead of 60 `uint8_t` entries.

### getCycleMs() -> getGapMs()

Rename and simplify to return gap duration:

```cpp
static uint32_t getGapMs() {
  switch (current_mode) {
    case TickMode::steady:
      return STEADY_GAP_MS;
    case TickMode::rush_wait:
      return RUSH_WAIT_GAP_MS;
    case TickMode::vetinari:
      return vetinari_gaps[pulse_index];  // pulse_index 0-58
    case TickMode::sprint:
      return SPRINT_GAP_MS;
    case TickMode::crawl:
      return CRAWL_GAP_MS;
  }
  return STEADY_GAP_MS;
}
```

### Main loop logic

Replace the current ticking/sweeping branches with unified logic:

For **timekeeping modes** (steady, rush_wait, vetinari):
- If `pulse_index < 59`: fire pulse, delay by `getGapMs()`
- If `pulse_index == 59`: wait for NTP minute boundary, then fire pulse 60 and call `onRevolutionComplete()`

For **positioning modes** (sprint, crawl):
- Fire pulse, delay by `getGapMs()`, increment, wrap at 60

### startNewMinute()

Update to reset `pulse_index = 0` and shuffle vetinari if needed. The shuffle now operates on 59 entries.

### onRevolutionComplete()

Should trigger when `pulse_index` reaches 60.

## Non-goals

- Don't change WiFi/MQTT/NTP connection handling
- Don't change MQTT command handling (stop, start, start_at_minute, stop_at_top, mode changes)

## Caveats

- `pulse_index` must only be reset by the `start` MQTT command (per AGENTS.md), except at minute boundaries via `startNewMinute()`
- Vetinari uses `pulse_index` 0-58 to index into the gap array; pulse 59 (index 59) waits for NTP
