# Sleight of hand

An ESP32-C3 firmware that drives a sweeping quartz clock movement's Lavet
motor with configurable sweep patterns. Inspired by the Vetinari clock from
Terry Pratchett's Discworld, where the clock ticks at irregular intervals but
still keeps perfect time.

The clock connects to WiFi, syncs to NTP for accurate timekeeping, and accepts
MQTT commands to switch between sweep modes at runtime. All timing is anchored
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

The firmware drives the Lavet motor with alternating polarity 30 ms pulses,
producing 960 micro-steps per revolution of the second hand. Both pins are set
low between pulses.

For movements that need more current than the ESP32 can source directly, use a
small H-bridge (e.g. DRV8833) between the GPIO pins and the coil.


## Building

Requires [PlatformIO](https://platformio.org/).

```sh
# Full firmware (WiFi, NTP, MQTT, sweep modes)
pio run -e vetinari
pio run -e vetinari -t upload

# Simple test firmware (continuous sweep, no WiFi)
pio run -e simple
pio run -e simple -t upload

# Pulse counting firmware (for measuring pulses per revolution)
pio run -e counting
pio run -e counting -t upload
```

The simple environment is useful for verifying that the motor steps correctly
before adding network complexity. The counting environment lets you measure the
exact number of pulses per revolution for your movement.


## First boot

1. Flash the `vetinari` environment.
2. The ESP32 creates a WiFi access point called **SleightOfHand**.
3. Connect to it and configure your WiFi credentials, MQTT broker host, and
   MQTT broker port through the captive portal.
4. The clock syncs to NTP and rapidly advances the second hand to the current
   second.
5. Normal sweeping begins.

WiFi credentials and MQTT settings are saved to flash and persist across
reboots. The captive portal only appears when no saved credentials are found
(or the saved network is unavailable for 3 minutes).


## Sweep modes

All modes produce exactly 960 pulses (one full revolution) per minute, anchored
to NTP minute boundaries, so the clock keeps accurate time regardless of which
mode is active.

| Mode | Description |
|---|---|
| `steady` | 960 pulses at 62 ms cycle (30 ms pulse + 32 ms pause). Smooth continuous sweep completing in ~59.5 s, with a brief idle before the next minute. |
| `rush_wait` | 960 pulses at 58 ms cycle (30 ms pulse + 28 ms pause). Completes the revolution in ~55.7 s, then idles for ~4.3 s until the next minute boundary. |

The default mode is `rush_wait`.


## MQTT

The clock subscribes to `clock/mode/set` and publishes the current mode to
`clock/mode/state` (retained).

### Changing modes

```sh
mosquitto_pub -h <broker> -t clock/mode/set -m "rush_wait"
mosquitto_pub -h <broker> -t clock/mode/set -m "steady"
```

Mode changes take effect when the current revolution completes (after 960 pulses).

### Control commands

| Command | Description |
|---|---|
| `stop` | Halts the sweep immediately. Use to manually position the hand. |
| `stop_at_top` | Finishes the current revolution (960 pulses), then stops with the hand at 12 o'clock. Safe to power off after this. Mutually exclusive with `start_at_minute`. |
| `start` | Starts sweeping immediately from pulse 0, anchoring the minute to now. |
| `start_at_minute` | Waits for the next NTP minute boundary, then starts from pulse 0. Position the hand at 12, send this command, and the clock begins exactly on the minute. Mutually exclusive with `stop_at_top`. |

```sh
# Stop the clock to position the hand
mosquitto_pub -h <broker> -t clock/mode/set -m "stop"

# Stop the clock at 12 o'clock (safe to power off)
mosquitto_pub -h <broker> -t clock/mode/set -m "stop_at_top"

# Start at the next minute boundary
mosquitto_pub -h <broker> -t clock/mode/set -m "start_at_minute"
```

MQTT reconnection attempts only happen during the idle gap between revolutions,
so a slow or unreachable broker never stalls the sweep.


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
| `PULSE_MS` | 30 | Coil pulse duration in ms |
| `PULSES_PER_REVOLUTION` | 960 | Micro-steps per full revolution of the second hand |
| `STEADY_CYCLE_MS` | 62 | Total cycle time (pulse + pause) for steady mode |
| `RUSH_CYCLE_MS` | 58 | Total cycle time (pulse + pause) for rush_wait mode |
| `CATCHUP_CYCLE_MS` | 32 | Cycle time for rapid catch-up pulses on boot |
