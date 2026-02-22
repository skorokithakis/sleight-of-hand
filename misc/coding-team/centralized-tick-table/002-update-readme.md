# Update README.md

## Context

The README is heavily outdated. It describes the old 960-pulse sweeping model,
30 ms pulses, cycle-based constants, and burst-based vetinari. The code now uses
60 pulses per revolution, 31 ms pulses, gap-based timing, and a centralized
59-element tick duration table (from task 001).

## Objective

Rewrite the README to accurately describe how the clock works after the
centralized tick table refactor.

## Scope

Only `README.md`. Do not touch `ARCHITECTURE.md`.

### What to fix

- **Hardware section**: pulse duration is 31 ms, not 30 ms. Remove the "960
  micro-steps" language. The motor advances one second mark per pulse (60 pulses
  per revolution).
- **Sweep modes section**: rename to "Tick modes" or similar. Rewrite the table
  to reflect the current modes and their actual timing:
  - `steady`: 59 ticks at 1000 ms each (real clock), ~1 s idle at minute boundary
  - `rush_wait`: 59 ticks at 932 ms each, ~5 s idle at minute boundary
  - `vetinari`: 59 ticks with shuffled irregular durations (534-2001 ms), small
    idle at minute boundary. Reshuffled every minute.
  - `sprint`: continuous ticking at 300 ms per tick for positioning. Not NTP-anchored.
  - `crawl`: continuous ticking at 2000 ms per tick for positioning. Not NTP-anchored.
- **Timing model**: add a brief explanation of the centralized tick table model:
  each timekeeping mode defines a 59-element array of tick durations. At t00 the
  NTP-anchored tick fires (p59->p00), then the array governs the remaining 59
  ticks. The hand waits at p59 for the next t00.
- **MQTT section**: replace "960 pulses" with "60 ticks" / "one revolution".
  Replace "revolution" language in stop_at_top with "the hand reaches 12 o'clock".
- **Configuration table**: update constants to match the actual code after the
  refactor (remove old cycle constants, reflect `TICK_COUNT`, `PULSE_MS = 31`,
  `PULSES_PER_REVOLUTION = 60`). Only list constants that a user might
  realistically want to change.
- **Building section**: remove the `counting` environment (it doesn't exist).
- Remove the paragraph about the counting environment being useful for measuring
  pulses per revolution.

### Style

- Keep the same overall structure and tone.
- Be concise. Don't over-explain internals; the README is for users, not
  contributors.
