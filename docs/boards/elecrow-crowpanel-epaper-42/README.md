# ELECROW CrowPanel ESP32-S3 4.2" E-paper HMI v1.2 with SEN63C

The [native ESP-IDF 6.1 application](../../../firmware/esp-idf-crowpanel-epaper/README.md)
logs a Sensirion SEN63C (PM1, PM2.5, PM4, PM10, temperature, humidity and CO2)
to Parquet on the microSD card every 10 seconds and draws the readings, 2 hour
sparklines and 24 hour charts on the 400 x 300 e-paper panel. **The owner unit
flashed and booted on 2026-11-17 with no sensor wired.** Nothing else is
qualified yet, see the [bench record](bench-verified.md).

## Board

- ESP32-S3 with 8 MB flash and 8 MB Octal PSRAM (AP_3v3), UC8276C panel, as
  read on the owner unit. Confirm each unit with `pixi run flash-id` and
  `pixi run efuse` before the first flash.
- Console is UART0 at 115200 through the board USB bridge.
- The panel, microSD card, keys and LED pins are fixed by the board. They are
  listed once in `firmware/esp-idf-crowpanel-epaper/board_io.h` and
  `display/epd_uc8276c.h`.

## Internal flash archive

With 8 MB flash the `spiffs` data partition sits at 0x7a0000 and is only
0x60000 bytes (384 KiB). It is a FAT volume mounted at `/flash`. With no usable
card the logger writes Parquet there and copies each file to the card once one
is Ready, see [decision record 0004](../../shared/decisions/0004-flash-fallback-archive.md). After the 128 KiB reserve and the FAT and wear levelling
overhead it holds about 3 to 4 windows of 15 minutes, about an hour. That is far
less than the 16 MB boards. `firmware/esp-idf-crowpanel-epaper/sdkconfig.defaults`
enables it. Nothing is proven on the bench yet.

## Wiring the SEN63C

The SEN63C uses a 6 pin 1.25 mm connector (cable side JST GHR-06V-S or ACES
51452-006H0H0-001), per the Sensirion SEN6x datasheet, table 17. Pins 1 and 6
are joined inside the sensor, and so are 2 and 5, so one of each is enough.
Pin 1 is on the left when you face the socket (datasheet figure 4). Counting
from the other end lands power and ground on the right pins but swaps SDA and
SCL, and the board then logs `OS SEN6X absent`.

| SEN63C pin | Signal | CrowPanel |
| --- | --- | --- |
| 1 | VDD, 3.3 V | 3V3 |
| 2 | GND | GND |
| 3 | SDA | GPIO8 |
| 4 | SCL | GPIO9 |
| 5 | GND, joined to pin 2 | leave open or GND |
| 6 | VDD, joined to pin 1 | leave open or 3V3 |

- **Add 10 k pull-ups from SDA to 3V3 and from SCL to 3V3.** The datasheet
  requires external pull-ups and the ESP32-S3 internal ones are too weak.
- Supply is 3.15 V to 3.6 V. Never feed the sensor 5 V. It draws about 75 to
  100 mA while measuring, with 2 ms peaks up to 200 mA.
- Keep the cable under 10 cm where possible. Sensirion allows up to 50 cm of
  AWG26 or thicker, with care against crosstalk.
- The bus runs at 100 kHz, the SEN63C maximum. Its address is 0x6B.
- Find GPIO8, GPIO9, 3V3 and GND on the CrowPanel expansion header by the
  silkscreen on your unit before connecting anything.

At boot the serial log prints `OS SEN6X bus ready=1 sda=8 scl=9 addr=0x6b ...`
and then `OS SEN6X present ...` once the sensor answers. `OS SEN6X absent ...`
means the board did not find it, usually a missing pull-up or swapped SDA and
SCL. PM values appear after about 30 s of warm up and CO2 after about 24 s.

## Keys and the settings popup

- Turn the knob to change page. Down goes to the next page, Up to the
  previous one. One turn Up from the dashboard is the status page.
- MENU opens the settings popup over any page. Turn the knob to pick a row,
  press it to change the value, and MENU or EXIT closes the popup.
- On Altitude and Temp offset a press starts an edit. Turn to adjust, press
  to keep the value, EXIT to cancel.
- Settings are saved to NVS namespace `ospanel` 5 seconds after the last
  change. The `oscfg` and `parquet` namespaces are never touched.
- The phone app changes the same settings with the `panel.*` and `sen6x.*`
  keys on `GET_CONFIG` page 7, see
  [board keys](../../shared/ble-sync-protocol.md#board-keys-page-7). A phone
  change shows on the screen and is saved like a knob change. It does not
  wake a sleeping screen. The phone sets Altitude to any metre, the knob
  then steps from that value.

| Row | Values | Effect |
| --- | --- | --- |
| Screen refresh | 10 s, 30 s, 1 min, 5 min | How often new readings redraw the screen. Keys redraw at once. Logging stays at 10 s. |
| Start on | Dashboard, Last page | The page shown after a reset. |
| Theme | Light, Dark | Light is black on white. Dark is white on black for every page, the popup and the pairing passkey. The sleeping screen stays light. A change does a full refresh. |
| Rotation | 0°, 90°, 180°, 270° | Turns the picture clockwise. 90° and 270° use the portrait layout. The keys keep their meaning. A change does a full refresh. The board has no motion sensor, so there is no Auto. |
| Language | English, عربي | Every page, the popup and the pairing screen in English or Egyptian Arabic. Arabic reads right to left and mirrors the popup, the status rows and the tiles. Numbers, units, sensor and protocol names stay in Latin script. A change does a full refresh. Set from the knob only, the phone has no key for it. |
| Wi-Fi | status only | On, Off or Not set up. Wi-Fi is set up and switched from the phone app. |
| Fan cleaning | Run now | Stops the measurement, runs the fan at full speed for 10 s, then starts again. Rows logged meanwhile hold null sensor values. |
| CO2 calibration | On, Off | SEN63C automatic self calibration. Keep it on for a Thing in the field. |
| Altitude | 0 to 3000 m, 50 m steps | Pressure compensation for CO2. |
| Temp offset | -10.0 to +5.0 C, 0.1 C steps | Sent to the sensor, see below. |
| Sleep after | Off, 10 min, 30 min, 1 h | Puts the panel to sleep with no readings on it. Any key wakes it, and so does a phone that pairs. |

Changing Altitude or CO2 calibration restarts the measurement, because the
SEN63C takes both in idle mode only (datasheet 4.8.35 and 4.8.39), so PM and
CO2 warm up again. A change made within 24 s of a start waits for that CO2
conditioning window to end before it is sent (4.8.1). The temperature
offset applies at once (4.8.14). The
sensor applies it itself, so it changes the stored `ambient_temperature_c`
column, not just the screen. Every change logs
`OS SEN6X config when=... altitude_m=... co2_asc=... temp_offset_dc=...`.
The sensor forgets all three at a reset, so the board writes them before
every start.

## Chart history after a reset

The 2 hour and 24 hour charts live in RAM. At boot, after the card mounts
and before logging starts, the board reads this Thing's own finalized
Parquet files back and seeds the charts with up to 24 hours of earlier rows,
newest boot first. It reads the unsynced folders of the boots in the NVS boot
ring and the two newest dated day folders. It only reads the card and NVS,
it writes, renames and deletes nothing, and it stops after about 8 seconds
with what it has. It logs one line, either `OS HISTORY restored boots=N
files=N rows=N span_s=N ms=N skipped=N cut=none|budget|rows|order|deadline
ring_boots=N dated_boots=N placed=N fallback=N` or `OS HISTORY restore=none
reason=no-thing|memory|path|no-boots|order-unknown|deadline|no-rows ms=N
ring_boots=N dated_boots=N placed=N fallback=N`. `ring_boots` counts the
boots of the NVS boot ring, `dated_boots` the boots seen only in the dated
folders, `placed` the boots the order could place (a ring boot with no
finalized file is left out) and `fallback` the placements the used bytes
decided. A failure only leaves the charts empty, logging starts the same way.

Limits.

- Rows still in RAM at a power cut are lost, and an unfinished `.partial`
  file is skipped, so the chart before a reset ends at the last finalized
  file, up to one file window early (15 minutes by default).
- Each boot is a separate segment. A restart mark sits between two boots
  and between the last one and this boot, and it never claims a length,
  because the time the board was off is unknown.
- The board has no RTC. Rows are placed by each boot's own monotonic time,
  never by UTC, so a restored row is never placed at an exact clock time.
- Boot order is proven by the NVS boot ring and the UTC in dated folder and
  file names. A boot that got UTC from a phone before its first file never
  enters the ring, so a ring boot that never had UTC and a dated only boot
  can have no proven order. Only there the card used bytes decide. Every row
  stores `sd_used_bytes`, and the firmware never deletes archive files, so
  card usage only grows from boot to boot. The board reads the footer bounds
  of each boot's oldest and newest file, and a boot whose largest value is
  at or below another boot's smallest is the older one. Overlapping or
  missing ranges stay unknown, and a used bytes answer never overrides a
  proven order. Deleting files from a computer between boots breaks this
  assumption, and the worst result is a misordered chart, the files stay as
  they are. These footers are read only when the proofs alone stop early,
  for at most about 4 of the 8 seconds.
- When a boot still cannot be placed, the restore stops at that boot and
  older boots stay out (`cut=order`, or `reason=order-unknown` when not even
  the newest boot can be placed). A boot whose ring entry could not be saved
  (a `ringless=` marker) and dated files older than the two newest day
  folders are not read.
- At most 160 files per boot and 64 boots are considered. Files of another
  shape, a damaged footer or a page the bounded decoder refuses count as
  `skipped` and add nothing.

## First flash

The image the owner unit carries before OpenSensor has a 0x6000 byte NVS
partition. The OpenSensor
layout uses 0x5000 bytes at 0x9000, like the other boards. The backup check in
`tools/check_idf_target.py` refuses to flash over a backup whose NVS region
differs, so `pixi run crowpanel-flash` refuses a backup of that image. The helper
is intentionally not changed.

For this CrowPanel the owner waived the backup and allowed a full erase on the
first flash only (2026-11-17). The previous image and its NVS are lost. Run the
read-only checks first, then erase and write the reviewed build directly.

```sh
pixi run ports
pixi run chip --port <checked port>
pixi run flash-id --port <checked port>
pixi run efuse --port <checked port>
pixi run crowpanel-build
pixi run python -m esptool --chip esp32s3 --port <checked port> erase-flash
cd firmware/esp-idf-crowpanel-epaper/build/logger
pixi run python -m esptool --chip esp32s3 --port <checked port> --baud 115200 \
  --before default-reset --after hard-reset write-flash @flash_args
```

Never write eFuses or format the card. From the second flash on, take a full
8,388,608 byte backup (`pixi run backup --board crowpanel --port <checked
port>`) and use `pixi run crowpanel-flash <checked port> <backup>`. That backup
holds the OpenSensor partition table, so the NVS check passes.

## Stack and heap budgets

`pixi run stack-budget --image crowpanel/logger --path` prints each task's
deepest static call path. The UI task `os-ui` has an 8 KiB stack, with a static
worst case of 3,280 bytes at this writing. CI fails the board leg when a task or
section grows past [tools/stack_budget.json](../../../tools/stack_budget.json).
