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
- Coil driven directly from GPIO 4 and GPIO 5 with a series resistor (820 ohm
  on one lead for a 600 ohm coil at 3.3V)

```
GPIO 4 --[820R]--> Coil lead A
GPIO 5 ------------> Coil lead B
```

The firmware drives the Lavet motor with alternating polarity 31 ms pulses,
one per second mark, for 60 pulses per full revolution of the second hand.
Both pins are set low between pulses.

For movements that need more current than the ESP32 can source directly, use a
small H-bridge (e.g. DRV8833) between the GPIO pins and the coil.


## Building

Requires [PlatformIO](https://platformio.org/).

```sh
# Full firmware (WiFi, NTP, MQTT, tick modes)
pio run -e vetinari
pio run -e vetinari -t upload

# Simple test firmware (continuous ticking, no WiFi)
pio run -e simple
pio run -e simple -t upload
```

The simple environment is useful for verifying that the motor steps correctly
before adding network complexity.


## First boot

1. Flash the `vetinari` environment.
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
| `sprint` | Continuous ticking at a configurable duration (default 300 ms per tick). For quickly advancing the hand to a target position. Activates immediately; not NTP-anchored. |
| `crawl` | Continuous ticking at a configurable duration (default 2000 ms per tick). For precisely positioning the hand at 12 o'clock. Activates immediately; not NTP-anchored. |

Sprint and crawl are positioning modes, not timekeeping modes. When switching
from either back to a timed mode, the clock waits for the next NTP minute
boundary to re-sync.

The default mode is `vetinari`.


## MQTT

The clock subscribes to `clock/mode/set` and publishes the current mode to
`clock/mode/state` (retained).

### Changing modes

```sh
mosquitto_pub -h <broker> -t clock/mode/set -m "rush_wait"
mosquitto_pub -h <broker> -t clock/mode/set -m "steady"
mosquitto_pub -h <broker> -t clock/mode/set -m "vetinari"
mosquitto_pub -h <broker> -t clock/mode/set -m "sprint"
mosquitto_pub -h <broker> -t clock/mode/set -m "crawl"

# Sprint and crawl accept an optional tick duration in milliseconds (minimum 50 ms).
# Without a parameter the defaults (300 ms and 2000 ms) are used.
mosquitto_pub -h <broker> -t clock/mode/set -m "sprint 150"
mosquitto_pub -h <broker> -t clock/mode/set -m "crawl 500"
```

Mode changes take effect when the current revolution completes (after 60
ticks), except sprint and crawl which activate immediately.

### Control commands

| Command | Description |
|---|---|
| `stop` | Halts ticking immediately. Use to manually position the hand. |
| `stop_at_top` | Finishes the current minute's ticks, then stops with the hand at 12 o'clock. Safe to power off after this. Mutually exclusive with `start_at_minute`. |
| `start` | Starts ticking immediately from tick 0, anchoring the minute to now. |
| `start_at_minute` | Waits for the next NTP minute boundary, then starts from tick 0. Position the hand at 12, send this command, and the clock begins exactly on the minute. Mutually exclusive with `stop_at_top`. |

```sh
# Stop the clock to position the hand
mosquitto_pub -h <broker> -t clock/mode/set -m "stop"

# Stop the clock at 12 o'clock (safe to power off)
mosquitto_pub -h <broker> -t clock/mode/set -m "stop_at_top"

# Start at the next minute boundary
mosquitto_pub -h <broker> -t clock/mode/set -m "start_at_minute"
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
| `PIN_COIL_A` | 4 | GPIO pin for coil lead A |
| `PIN_COIL_B` | 5 | GPIO pin for coil lead B |
| `PULSE_MS` | 31 | Coil pulse duration in ms |
| `PULSES_PER_REVOLUTION` | 60 | Ticks per full revolution of the second hand |
| `TICK_COUNT` | 59 | Number of ticks governed by the tick duration table per minute |
| `SPRINT_DEFAULT_MS` | 300 | Default total tick duration in sprint mode when no parameter is given |
| `CRAWL_DEFAULT_MS` | 2000 | Default total tick duration in crawl mode when no parameter is given |
