# Sleight of hand

An ESP32-C3 firmware that drives a sweeping quartz clock movement's Lavet
motor with configurable tick patterns. Inspired by the Vetinari clock from
Terry Pratchett's Discworld, where the clock ticks at irregular intervals but
still keeps perfect time.

The clock connects to WiFi, syncs to NTP for accurate timekeeping, and accepts
MQTT commands to switch between tick modes at runtime. All timing is anchored
to NTP minute boundaries, so the clock stays accurate regardless of mode.


## Hardware

- ESP32-C3 (Super Mini or similar)
- Sweeping quartz clock movement with the original driver board removed
- Coil driven directly from GPIO 5 and GPIO 6 with a series resistor (820 ohm
  on one lead for a 600 ohm coil at 3.3V)

```
GPIO 5 --[820R]--> Coil lead A
GPIO 6 ------------> Coil lead B
```

The firmware drives the Lavet motor with alternating polarity 31 ms pulses,
one per second mark, for 60 pulses per full revolution of the second hand.
Both pins are set low between pulses.

For movements that need more current than the ESP32 can source directly, use a
small H-bridge (e.g. DRV8833) between the GPIO pins and the coil.


## Building and flashing

Requires [PlatformIO](https://platformio.org/).

```sh
# Build
pio run -e sleight
```

### Flashing over USB

```sh
pio run -e sleight -t upload
```

The first flash after changing the partition table must be done over USB.

### Flashing over WiFi (OTA)

Once the firmware is running and connected to WiFi, subsequent flashes can be
done over the air:

```sh
pio run -e sleight-ota -t upload
```

This uses mDNS to find the device at `sleight-of-hand.local`. You can also
flash by IP address directly:

```sh
pio run -e sleight -t upload --upload-port <ip>
```


## First boot

1. Flash the `sleight` environment.
2. The ESP32 creates a WiFi access point called **SleightOfHand**.
3. Connect to it and configure your WiFi credentials, MQTT broker host, and
   MQTT broker port through the captive portal.
4. The clock syncs to NTP and waits for the next minute boundary.
5. Ticking begins exactly on the minute.

WiFi credentials and MQTT settings are saved to flash and persist across
reboots. The captive portal only appears when no saved credentials are found
(or the saved network is unavailable for 3 minutes).


## Tick modes

The clock distinguishes between hand **position** (p00–p59, where p00 is
12 o'clock) and real **time** (t00 = top of the minute). Each minute cycle:

1. At t00, the NTP-anchored tick fires: p59 → p00.
2. `tick_durations[0]` through `tick_durations[58]` govern the next 59 ticks:
   p00 → p01, p01 → p02, ..., p58 → p59.
3. The hand waits at p59 for the next t00.

Each timekeeping mode defines the 59-element `tick_durations` array.

| Mode | Description |
|---|---|
| `steady` | 59 ticks at 1000 ms each. The hand advances once per second, with ~1 s idle at the minute boundary. |
| `rush_wait` | 59 ticks at 932 ms each. Completes in ~55 s, then idles ~5 s until the next minute boundary. |
| `vetinari` | 59 ticks with shuffled irregular durations (534–2001 ms). The hand visibly speeds up and slows down, but completes the minute on time. Reshuffled every minute. |
| `hesitate` | 58 ticks at 980 ms, 1 tick at 2000 ms, shuffled each minute. The hand pauses for ~2 seconds at a random position each minute, creating a noticeable hesitation somewhere in the revolution. |
| `stumble` | 58 ticks at 1010 ms, 1 tick at 420 ms, shuffled each minute. The hand skips forward quickly at a random position each minute, as if stumbling. |
| `gravity` | 59 ticks computed from pendulum physics. Fast near 6 o'clock (~500 ms), slow near 12 o'clock (~3150 ms). Not shuffled — the speed difference is tied to the dial position. |

On every boot and at every top-of-hour minute boundary, the clock picks a random mode. Manual MQTT mode changes still work; the next hour boundary overrides them.


## MQTT

The clock subscribes to `clock/mode/set` and publishes the current mode to
`clock/mode/state` (retained).

### Changing modes

```sh
mosquitto_pub -h <broker> -t clock/mode/set -m "steady"
mosquitto_pub -h <broker> -t clock/mode/set -m "rush_wait"
mosquitto_pub -h <broker> -t clock/mode/set -m "vetinari"
mosquitto_pub -h <broker> -t clock/mode/set -m "hesitate"
mosquitto_pub -h <broker> -t clock/mode/set -m "stumble"
mosquitto_pub -h <broker> -t clock/mode/set -m "gravity"

# rush_wait accepts an optional tick duration in milliseconds (minimum 200 ms).
# Without a parameter the default (700 ms) is used.
mosquitto_pub -h <broker> -t clock/mode/set -m "rush_wait 500"
```

Mode changes take effect when the current revolution completes (after 60
ticks).

### Control commands

| Command | Description |
|---|---|
| `stop` | Halts ticking immediately. Cancels any in-progress calibration. |
| `start` | Starts ticking immediately from tick 0. Cancels any in-progress calibration. |
| `start_at_minute` | Waits for the next NTP minute boundary, then starts from tick 0. Position the hand at 12, send this command, and the clock begins exactly on the minute. Cancels any in-progress calibration. |
| `calibrate <position>` | Tell the clock the second hand is at position (0–59). The clock sprints to p59 and re-syncs to NTP at the next minute boundary. Uses the current NTP hour and minute to determine the full displayed time. |
| `calibrate H:MM:SS` | Tell the clock the full time it's displaying (in UTC). The clock converges to NTP time: if ≤3 hours behind it sprints forward, if >3 hours behind it sprints to p59 and waits for NTP to catch up. |

```sh
# Stop the clock to manually position the hand
mosquitto_pub -h <broker> -t clock/mode/set -m "stop"

# Start at the next minute boundary (hand must be at 12 o'clock)
mosquitto_pub -h <broker> -t clock/mode/set -m "start_at_minute"

# Tell the clock the second hand is at the 45-second mark
mosquitto_pub -h <broker> -t clock/mode/set -m "calibrate 45"

# Tell the clock it's showing 3:22:15 UTC
mosquitto_pub -h <broker> -t clock/mode/set -m "calibrate 3:22:15"
```

MQTT reconnection attempts only happen during the idle gap at the minute
boundary, so a slow or unreachable broker never stalls ticking.


## UDP logging

All log messages are broadcast via UDP on port 37243, in addition to serial
output. Listen with:

```sh
nc -kul 37243
```


## Configuration

Constants at the top of `src/main.cpp`:

| Constant | Default | Description |
|---|---|---|
| `PIN_COIL_A` | 5 | GPIO pin for coil lead A |
| `PIN_COIL_B` | 6 | GPIO pin for coil lead B |
| `PULSE_MS` | 31 | Coil pulse duration in ms |
| `PULSES_PER_REVOLUTION` | 60 | Ticks per full revolution of the second hand |
| `TICK_COUNT` | 59 | Number of ticks governed by the tick duration table per minute |
| `CALIBRATE_SPRINT_MS` | 200 | Tick duration during calibration sprints |
| `UTC_OFFSET_SECONDS` | 0 | Timezone offset from UTC. All times (including `calibrate H:MM:SS`) use this. |
