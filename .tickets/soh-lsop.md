---
id: soh-lsop
status: closed
deps: [soh-qa33]
links: []
created: 2026-03-30T15:06:46Z
type: task
priority: 2
assignee: Stavros Korokithakis
---
# Calibration engine and command overhaul

Remove old positioning infrastructure and replace with time-aware calibration. REMOVALS: TickMode::sprint, TickMode::crawl, sprint/crawl MQTT commands, stop_at_top command and stop_at_top_pending flag, positioning_tick_ms, is_calibrate_sprint, isTimekeeping()/isPositioning() helpers, the positioning branch in the main loop. Everything is timekeeping now. ADD calibration state: is_calibrating bool, and a target NTP minute timestamp for the WAIT path. ADD calibration sprint logic in main loop: pulse at CALIBRATE_SPRINT_MS (200ms), compare displayed_time against live NTP time (seconds in 12h cycle), when displayed_time passes ntp_time finish the current revolution to p59, set start_at_minute_pending, done. ADD WAIT path: when gap > 10800 (3h), sprint to p59, store the specific target NTP minute boundary to wait for, set start_at_minute_pending but only fire when NTP reaches that target (not just the next minute). ADD calibrate H:MM:SS parsing: accept flexible hour (1 or 2 digits), mandatory 2-digit MM and SS. Set displayed_time from parsed time, compute forward distance to NTP mod 43200, choose SPRINT or WAIT path. UPDATE calibrate <position>: set displayed_time = (ntp_hour % 12) * 3600 + ntp_minute * 60 + position, then run same SPRINT/WAIT convergence. COMMAND INTERACTION: mode changes during calibration queue via pending_mode, applied when calibration completes at minute boundary. calibrate (any form) cancels current calibration and starts fresh. start/stop cancel calibration. Non-goals: no backward pulsing, no mid-minute resume, no changes to tick distribution logic.

## Acceptance Criteria

calibrate H:MM:SS with d<=3h sprints multi-revolution to NTP, lands at p59, syncs at boundary. calibrate H:MM:SS with d>3h sprints to p59, waits for correct NTP minute, syncs. calibrate <position> works as before but triggers convergence. sprint/crawl/stop_at_top commands removed. Mode changes during calibration queue. Firmware compiles.

