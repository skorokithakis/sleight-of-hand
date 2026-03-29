---
id: soh-zbhj
status: open
deps: []
links: []
created: 2026-03-29T23:30:34Z
type: task
priority: 2
assignee: Stavros Korokithakis
---
# Time-aware calibration

Add an internal displayed_time state (seconds in 12-hour cycle, 0-43199) that tracks what the clock face currently shows. Extend the calibrate command to accept H:MM:SS format, which sets displayed_time and converges the clock to NTP time. Remove sprint/crawl modes and stop_at_top, which are superseded by time-aware calibration.

## displayed_time (replaces pulse_index)

A uint32_t tracking the clock's displayed time as seconds past 12:00:00 (range 0-43199). Every pulseOnce() call increments it by 1 (mod 43200). The seconds component (displayed_time % 60) replaces pulse_index everywhere. No separate pulse_index state.

Initialization:
- On boot, set from NTP at the first minute boundary (same moment start_at_minute_pending fires).
- On calibrate H:MM:SS, set from the parsed time.
- On calibrate <position>, derive from position + current NTP hour/minute.

## calibrate H:MM:SS command

Parse H:MM:SS (1:33:19 format). Set displayed_time. Compute d = forward clockwise distance in seconds from displayed_time to NTP time (mod 43200).

If d <= 10800 (3 hours): SPRINT
- Enter calibration sprint mode. Each pulse advances displayed_time.
- Sprint across revolution boundaries (multi-revolution) until displayed_time passes NTP time.
- Once displayed_time > ntp_time, finish the current revolution to p59.
- Stop at p59, wait for minute boundary, resume normal timekeeping.

If d > 10800: WAIT
- Sprint from current position to p59 (partial revolution).
- Stop at p59 with start_at_minute_pending, but store a target timestamp for the specific NTP minute boundary to wait for (not just the next one).
- Wait until NTP reaches that target, then fire the boundary pulse and resume.

## Existing calibrate <position> behavior

Unchanged mechanically. Now also sets displayed_time = (ntp_hour % 12) * 3600 + ntp_minute * 60 + position.

## Removals

- pulse_index: replaced by displayed_time % 60 (derived, not stored).
- TickMode::sprint and TickMode::crawl: removed. The only fast pulsing is the internal calibration sprint.
- sprint <ms> and crawl <ms> MQTT commands: removed.
- positioning_tick_ms: removed. Calibration sprint uses CALIBRATE_SPRINT_MS directly.
- stop_at_top command and stop_at_top_pending flag: removed.
- isTimekeeping() / isPositioning() distinction: removed (all user-facing modes are timekeeping).
- The positioning branch in the main loop: replaced by calibration sprint logic.

## Command interaction during calibration

- Mode changes (vetinari, steady, etc.) during calibration: queue via mode_change_pending, applied when calibration completes and the minute boundary fires. Do NOT interrupt the calibration.
- calibrate (any form): cancels current calibration, starts fresh.
- start, stop: cancel calibration (explicit override-everything commands).

## Constraints

- The p59 invariant is preserved: all paths end at p59 waiting for a minute boundary.
- Sprint speed: CALIBRATE_SPRINT_MS (200ms).
- The 3-hour sprint threshold is a hardcoded constant.

## Non-goals

- No mid-minute resume logic.
- No backward pulsing.
- No changes to existing timekeeping modes tick distribution logic.

## Acceptance Criteria

calibrate H:MM:SS when clock is behind (d <= 3h): clock sprints multi-revolution to catch up, lands at p59, syncs at minute boundary. calibrate H:MM:SS when clock is ahead (d > 3h): clock sprints to p59, waits for NTP to catch up to the correct minute boundary, then syncs. calibrate <position> (bare number) still works as before but now sets displayed_time. displayed_time replaces pulse_index everywhere. sprint/crawl/stop_at_top commands removed. Mode changes during calibration queue and apply after calibration completes.

