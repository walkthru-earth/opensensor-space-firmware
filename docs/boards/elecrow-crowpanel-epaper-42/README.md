# ELECROW CrowPanel ESP32-S3 4.2" E-paper HMI v1.2 with SEN63C

The [native ESP-IDF 6.1 application](../../../firmware/esp-idf-crowpanel-epaper/README.md)
logs a Sensirion SEN63C (PM1, PM2.5, PM4, PM10, temperature, humidity and CO2)
to Parquet on the microSD card every 10 seconds and draws the readings, 2 hour
sparklines and 24 hour charts on the 400 x 300 e-paper panel. **No hardware run
is recorded yet**, see the [bench record](bench-verified.md).

## Board

- ESP32-S3 with 8 MB flash and 8 MB Octal PSRAM (AP_3v3), UC8276C panel, as
  read on the owner unit. Confirm each unit with `pixi run flash-id` and
  `pixi run efuse` before the first flash.
- Console is UART0 at 115200 through the board USB bridge.
- The panel, microSD card, keys and LED pins are fixed by the board. They are
  listed once in `firmware/esp-idf-crowpanel-epaper/board_io.h` and
  `display/epd_uc8276c.h`.

## Wiring the SEN63C

The SEN63C uses a 6 pin 1.25 mm connector (cable side JST GHR-06V-S or ACES
51452-006H0H0-001), per the Sensirion SEN6x datasheet, table 17. Pins 1 and 6
are joined inside the sensor, and so are 2 and 5, so one of each is enough.

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
worst case of 3,072 bytes at this writing. CI fails the board leg when a task or
section grows past [tools/stack_budget.json](../../../tools/stack_budget.json).
