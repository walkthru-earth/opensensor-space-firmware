# GNSS for moving Things

Board-neutral GNSS support in two ESP-IDF components.

| Component | Sources | Requires | Who uses it |
| --- | --- | --- | --- |
| `gnss` | `src/os_gnss.*` | `common runtime location` | The logger, every board that builds it, and the host tests |
| `gnss_reader` | `../gnss_reader/src/os_gnss_reader.*` | `gnss`, plus `esp_driver_uart esp_timer` privately | Only a board with a receiver |

## Data flow

1. A `Dialect` parses one receiver line into a `Reading` (raw degrees, stack
   only). `SimcomDialect` speaks `AT+CGNSSPWR` and `AT+CGNSSINFO` per the
   [SIM767XX AT manual](https://files.waveshare.com/wiki/SIM7670G-LTE-Cat-1-GNSS-HAT/SIM767XX_Series_AT_Command_Manual_V1.01.pdf).
2. `to_fix()` turns it at once into an H3 resolution 8 cell with
   `from_degrees` and drops the degrees. No finer cell is computed (#139). A
   `Fix` holds the cell, its resolution, the fix monotonic time, the fix mode
   and the satellites if known.
3. The reader task stores the newest `Fix` and `latest()` copies it out.
4. The logger calls `Hooks::gnss_fix` once per row and applies
   `select_location()`. The storage worker also calls it for GET_CONFIG
   page 1 `location.gnss_cell` through `readback_cell()`, so the hook must be
   safe from any task, never from an ISR. `latest()` is.

| `thing.mobility` | Fresh fix (at most 15 s old) | Row location |
| --- | --- | --- |
| `fixed` | any | Configured cell, `location_source` 1 |
| `moving` | yes | GNSS cell at resolution 8, `location_source` 2 |
| `moving` | no | Four nulls |

A reply without a fix clears the stored fix at once. A silent receiver keeps
it until it is no longer fresh. Polls are about 12 s apart, so one silent poll
already ages the fix past 15 s and rows about 15 to 22 s after the last fix
are null. Anything that does not parse counts as no fix.

## SIMCom parser

The no-fix reply seen on SIM7670G firmware V1.9.05 is `+CGNSSINFO: ,,,,,,,,`
and the manual shows 18 empty fields. Both mean no fix. A fixed line is
matched by its latitude, N or S, longitude, E or W run, so a firmware that
adds or drops leading fields still parses. The SIM767XX manual V1.01
contradicts itself. Its Defined Values give decimal degrees (`dd.ddddd`,
`ddd.ddddd`) and its example line, copied from SIM7600, gives degrees and
minutes (`ddmm.mmmmmm`, `dddmm.mmmmmm`). The parser takes both and tells them
apart by the integer digit count. Latitude with 1 or 2 digits and longitude
with 1 to 3 are decimal degrees. Latitude with exactly 4 and longitude with
exactly 5 are degrees and minutes, minutes below 60. Any other count is
malformed, so an unexpected shape never becomes a wrong cell. The fix mode
must be 2 or 3 when the line carries one. Null island, latitude over 90 and
longitude over 180 are rejected. Both fixed forms are unverified on V1.9.05
until a dated bench record shows one.

## Reader task

`start(ReaderConfig)` installs the UART driver and creates the `os-gnss` task
with the given stack, priority and core. The task calls the board power
callback once, polls the power query until the engine reports on (it never
waits for a READY URC), sends the power-on command when it is off, then sends
the fix query every `poll_period_ms`. After three silent polls it asks for
power again. Line buffers are fixed arrays and every read has a timeout. It
prints `OS GNSS state=<unknown|no-reply|off|on|fix|no-fix> uart=<n> tx=<n>
rx=<n>` on each change, never a position.

## Adding a board

See [board porting](../../../docs/shared/board-porting.md#adding-gnss).

## Verification

`pixi run gnss-test` builds `tools/test_gnss.cpp` against the real H3 sources
and runs the parser, freshness, source rule and no-split cases, then checks
that both components stay board-neutral.
