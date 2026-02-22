# Centralized tick duration table

## Context

Timekeeping modes currently each have their own gap constants or arrays, and the
main loop has mode-specific branching via `getGapMs()`. We want every timekeeping
mode to express itself as a 59-element array of tick durations, and the main loop
to be mode-agnostic.

The timing model is:

| Event | Position | Timing |
|---|---|---|
| t00 arrives, fire tick | p59 -> p00 | NTP-anchored |
| durations[0], fire tick | p00 -> p01 | Mode-defined |
| durations[1], fire tick | p01 -> p02 | Mode-defined |
| ... | ... | ... |
| durations[58], fire tick | p58 -> p59 | Mode-defined |
| Wait for t00 | stay at p59 | NTP-anchored idle |

Each duration value is the **total wall-clock time** from one tick to the next
(not just the gap). The implementation subtracts `PULSE_MS` to get the delay.
For a real clock, every entry is 1000.

## Objective

Replace the per-mode gap constants and `getGapMs()` with a single
`uint16_t tick_durations[59]` array that is filled at the start of each minute
by a mode-dispatch function.

## Scope

All changes are in `src/main.cpp`.

### Add

- `constexpr uint8_t TICK_COUNT = 59;`
- `uint16_t tick_durations[TICK_COUNT];` (replaces `vetinari_gaps[]`)
- A `fillTickDurations()` function that switches on `current_mode` and fills the
  array:
  - `steady`: fill all 59 with 1000
  - `rush_wait`: fill all 59 with 932
  - `vetinari`: copy template, Fisher-Yates shuffle (same logic as today)
- Update `startNewMinute()` to call `fillTickDurations()` instead of the
  vetinari-specific shuffle.

### Change

- The vetinari template values must become **total durations** (current values
  are gaps only). Add `PULSE_MS` (31) to each value in `VETINARI_TEMPLATE`.
- Rename `VETINARI_TEMPLATE` to something mode-scoped if you like, but it's fine
  to keep the name since vetinari is the only mode with a template.
- The timekeeping branch of `loop()`: instead of calling `getGapMs()`, read
  `tick_durations[pulse_index]` and subtract `PULSE_MS` for the delay. The
  structure stays the same: pulses 0-58 use the array, pulse 59 waits for the
  NTP boundary.

### Remove

- `STEADY_GAP_MS`, `RUSH_WAIT_GAP_MS` constants
- `VETINARI_PULSES` constant (replaced by `TICK_COUNT`)
- `vetinari_gaps[]` array (replaced by `tick_durations[]`)
- `shuffleVetinariCycles()` (inlined into the vetinari branch of `fillTickDurations()`)
- `getGapMs()` function

### Do not change

- Positioning modes (sprint/crawl): they keep their own gap constants and their
  branch in `loop()` is unchanged.
- MQTT command handling, mode switching logic, NTP sync model.
- The `pulse_index` reset rules (only in `start` command and `startNewMinute()`).

## Constraints

- The sum of all 59 duration values must be < 60000 for any mode. This is the
  mode author's responsibility and doesn't need runtime enforcement.
- `SPRINT_GAP_MS` and `CRAWL_GAP_MS` stay as-is (positioning modes don't use
  the table).

## Acceptance criteria

- `getGapMs()` no longer exists.
- Adding a new timekeeping mode means adding a case to `fillTickDurations()`.
- Existing behavior is preserved for all modes.
