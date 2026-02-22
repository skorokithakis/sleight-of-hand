# Clear blocking state when activating positioning modes

## Context

Sprint and crawl are positioning modes (not timekeeping). They activate via the
`!isTimekeeping(requested)` branch in `onMqttMessage()` (lines 293-299 of `src/main.cpp`).

Bug: that branch sets `current_mode` but does not clear `stopped` or
`start_at_minute_pending`. If the clock is in the waiting-for-minute state
(boot, after `stop_at_top`, after returning from a positioning mode), the loop
blocks at line 468 and sprint/crawl never starts.

## Objective

Sprint and crawl must begin pulsing immediately regardless of current state.

## Scope

`src/main.cpp`, lines 293-299 only. Add `stopped = false;` and
`start_at_minute_pending = false;` inside the `!isTimekeeping(requested)` block,
before the log message.

Also clear `stop_at_top_pending` — if the user asked to stop at top but then
switches to sprint, the stop-at-top intent is superseded.

## Non-goals

- Do not reset `pulse_index`.
- Do not change any other code paths.
