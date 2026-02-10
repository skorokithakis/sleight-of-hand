# Sleight of hand

An ESP32-driven analog wall clock. Take a cheap quartz clock, remove the
original timing IC, and wire the coil to two GPIO pins through a resistor. The
ESP32 then drives the coil with alternating-polarity pulses to step the second
hand.

## Hardware

- ESP32 dev board
- Cheap quartz clock with the timing IC removed
- 100Ω resistor in series with the coil

```
GPIO 25 --[100Ω]--> Coil pin A
GPIO 26 --[100Ω]--> Coil pin B
```

## Building

```
pio run
```

## Flashing

```
pio run -t upload
```
