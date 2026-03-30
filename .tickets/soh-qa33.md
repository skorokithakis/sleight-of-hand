---
id: soh-qa33
status: closed
deps: []
links: []
created: 2026-03-30T15:06:33Z
type: task
priority: 2
assignee: Stavros Korokithakis
---
# Replace pulse_index with displayed_time

Mechanical refactor: introduce displayed_time (uint32_t, 0-43199) tracking seconds past 12:00:00 in a 12-hour cycle. Increment mod 43200 in every pulseOnce() call. Replace all reads of pulse_index with (displayed_time % 60). Remove pulse_index entirely. Set displayed_time from NTP in startNewMinute() on first minute boundary. Set displayed_time in existing calibrate <position> to (ntp_hour % 12) * 3600 + ntp_minute * 60 + position. On boot, displayed_time starts at 0 (wrong but fine, corrected at first boundary). NO BEHAVIOR CHANGE — everything works exactly as before, just with the new state variable. Non-goals: do not touch TickMode, do not add H:MM:SS parsing, do not change calibration logic beyond setting displayed_time.

## Acceptance Criteria

pulse_index no longer exists. displayed_time is incremented mod 43200 on every pulse. All former pulse_index reads use (displayed_time % 60). Firmware compiles and behaves identically to before.

