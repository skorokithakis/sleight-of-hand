---
id: soh-wuso
status: closed
deps: []
links: []
created: 2026-03-28T00:45:00Z
type: task
priority: 2
assignee: Stavros Korokithakis
---
# Add non-blocking WiFi reconnection in loop()

WiFi drops are never recovered because WiFiManager only runs in setup(). Add a millis-based check in loop() that detects WiFi.status() != WL_CONNECTED and calls WiFi.disconnect() followed by WiFi.begin() to trigger a reconnect. Rate-limit to once every 30 seconds. Place the check after the p59 boundary-pulse block but before or near the MQTT reconnect block (since MQTT reconnect is pointless without WiFi anyway). Use a simple static or global unsigned long for the last-attempt timestamp. Non-goals: no captive portal re-launch, no backoff, no new state reporting.

## Acceptance Criteria

When WiFi drops, the ESP32 recovers automatically without power cycle. The reconnect call does not block loop(). Timekeeping is unaffected.

