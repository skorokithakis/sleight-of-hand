# Documentation: p00/t00 terminology and cleanup

## Context

The project uses a timing model where "position" (where the hand is) and "time"
(what the real clock says) are distinct concepts. We use the notation:

- **p00–p59**: hand position (p00 = 12 o'clock, p01 = 1-second mark, etc.)
- **t00**: real-world time at the top of the minute (second :00)

The minute cycle is: at t00, the NTP-anchored tick fires (p59 → p00). Then
`tick_durations[0]` through `tick_durations[58]` govern ticks p00 → p01 through
p58 → p59. The hand waits at p59 for the next t00.

## Objective

1. Add a "Timing model" section to `README.md` that defines p00/t00 terminology
   and explains the minute cycle using it.
2. Remove the stale comment `// 500ms total tick` on `SPRINT_GAP_MS` in
   `src/main.cpp` (line 16). The actual total is 300 ms; just remove the
   comment entirely rather than fixing it, since the math is obvious.
3. Update `TODO.md`: mark the first two items as done, and mark the third
   ("verify that we keep internal time") as done with a note that
   `minute_start_ms` is advanced by `+= 60000` each minute independently of
   NTP, so the clock keeps running when the internet is down.

## Scope

### `README.md`

Rework the "Tick modes" section intro to use p00/t00 terminology. The current
text says "At the NTP minute boundary the 60th tick fires (advancing the hand to
12 o'clock), then the array governs the next 59 ticks. The hand waits at the
59th position for the next minute boundary before firing the final tick."

Replace with something that uses the p00/t00 notation explicitly, e.g.:

> The clock distinguishes between hand **position** (p00–p59, where p00 is
> 12 o'clock) and real **time** (t00 = top of the minute). Each minute cycle:
>
> 1. At t00, the NTP-anchored tick fires: p59 → p00.
> 2. `tick_durations[0]` through `tick_durations[58]` govern the next 59 ticks:
>    p00 → p01, p01 → p02, ..., p58 → p59.
> 3. The hand waits at p59 for the next t00.

Keep it concise. This replaces the existing intro paragraph, not the mode table.

### `src/main.cpp`

Remove the comment on `SPRINT_GAP_MS` (line 16). Change:
```
constexpr uint32_t SPRINT_GAP_MS = 269;      // 500ms total tick
```
to:
```
constexpr uint32_t SPRINT_GAP_MS = 269;
```

### `TODO.md`

Update to reflect current state. All three items are done:
- "Consolidate all documentation to p00/t00 terminology" — done by this task.
- "Consolidate all modes to use the same machinery" — done by task 001.
- "Verify that we keep internal time" — already correct: `minute_start_ms` is
  advanced by `+= 60000` independently of NTP.

## Non-goals

- Do not touch `ARCHITECTURE.md`. It's a repo-scout artifact and will be
  regenerated.
- Do not change any code behavior.
