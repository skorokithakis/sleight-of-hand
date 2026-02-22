# Track last timekeeping mode for start_at_minute

## Context

`start_at_minute` starts the clock at the next NTP minute boundary in whatever
`current_mode` happens to be. If the user was in sprint/crawl (positioning
modes), the clock starts back up in that positioning mode, which is wrong —
positioning modes don't do NTP-synced timekeeping.

## Objective

When the minute-boundary startup fires, if `current_mode` is not a timekeeping
mode, fall back to the last timekeeping mode that was active (default: vetinari).

## Scope

`src/main.cpp` only.

1. Add a global `TickMode last_timekeeping_mode = TickMode::vetinari;` next to
   the other mode variables (near line 79-81).

2. Update `last_timekeeping_mode` whenever a timekeeping mode becomes the active
   `current_mode`. There are three assignment sites to guard:
   - Line ~308: timekeeping mode set while stopped (`else if (stopped)` branch).
   - Line ~366: deferred mode applied in `onRevolutionComplete()`.
   - Line ~477: pending mode applied at minute-boundary startup.
   In each case, after `current_mode = ...`, add:
   `if (isTimekeeping(current_mode)) last_timekeeping_mode = current_mode;`

3. In the minute-boundary startup block (the `if (start_at_minute_pending)`
   block in `loop()`), after any pending mode change is applied but before
   `startNewMinute()`, add: if `current_mode` is not timekeeping, set
   `current_mode = last_timekeeping_mode` and publish.

## Non-goals

- Do not change sprint/crawl activation.
- Do not change `stop_at_top` logic.
- Do not reset `pulse_index`.
- Do not persist `last_timekeeping_mode` to flash.
