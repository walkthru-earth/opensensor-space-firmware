# Waveshare ESP32-S3-ePaper-3.97 with SEN63C

The native ESP-IDF 6.1 application in
[firmware/esp-idf-waveshare-epaper-397](../../../firmware/esp-idf-waveshare-epaper-397/AGENTS.md)
logs a Sensirion SEN63C (PM1, PM2.5, PM4, PM10, temperature, humidity and CO2)
to Parquet on the microSD card every 10 seconds. The 800 x 480 SSD1677 panel
shows a temporary bring-up screen until the shared e-paper UI lands. **The
owner unit flashed, booted and logged SEN63C rows on 2026-11-20.** Nothing
else is qualified yet, see the [bench record](bench-verified.md).

## Board

- Waveshare ESP32-S3-ePaper-3.97 (SKU 33810), ESP32-S3-WROOM-1-N16R8. 16 MB
  flash and 8 MB Octal PSRAM, as read on the owner unit. Confirm each unit with
  `pixi run flash-id` and `pixi run efuse` before the first flash.
- Console is the ESP32-S3 USB-Serial/JTAG on the USB-C port. GPIO43 and GPIO44
  are UART0 by default, and here they carry the SEN63C bus instead.
- Flash layout is NVS 0x9000, otadata 0xe000, two 6 MiB OTA slots at 0x10000
  and 0x610000, the core dump at 0xc10000 and an unused data partition at
  0xc20000. The pins live once in
  `firmware/esp-idf-waveshare-epaper-397/board_io.h` and
  `display/epd_ssd1677.cpp`.

| Function | Pins |
| --- | --- |
| SSD1677 panel on SPI3 | SCK 11, MOSI 12, CS 10, DC 9, RST 46, BUSY 3 |
| Panel supply | TG28 ALDO3 at 3.3 V |
| Internal I2C, 100 kHz | SDA 41, SCL 42 |
| SEN63C I2C, 100 kHz | SDA 43, SCL 44 |
| microSD, SDMMC 4 bit | CLK 16, CMD 17, D0 15, D1 7, D2 8, D3 18 |

The internal bus holds the TG28 PMIC (0x34), the PCF85063 RTC (0x51), the
QMI8658A IMU (0x6B, SA0 tied to GND), the SHTC3 (0x70) and the ES8311 codec
(0x18). The boot scan of 2026-11-20 found exactly these five. The firmware
uses the PMIC, the RTC and the IMU accelerometer. The SHTC3 and the codec stay
in their reset state.

## Wiring the SEN63C

The SEN63C uses a 6 pin 1.25 mm connector, per the Sensirion SEN6x datasheet,
table 17. Pin 1 is on the left when you face the socket (datasheet figure 4).

| SEN63C pin | Signal | ePaper-3.97 |
| --- | --- | --- |
| 1 | VDD, 3.3 V | 3V3 |
| 2 | GND | GND |
| 3 | SDA | GPIO43 (TX on the header) |
| 4 | SCL | GPIO44 (RX on the header) |
| 5 | GND, joined to pin 2 | leave open or GND |
| 6 | VDD, joined to pin 1 | leave open or 3V3 |

- **Add 10 k pull-ups from SDA to 3V3 and from SCL to 3V3.** The datasheet
  requires external pull-ups and the ESP32-S3 internal ones are too weak.
- Supply is 3.15 V to 3.6 V. Never feed the sensor 5 V.
- The address is 0x6B, the same as the IMU, which is fine because the two sit
  on separate buses.

At boot the serial log prints `OS SEN6X bus ready=1 sda=43 scl=44 addr=0x6b`
and then `OS SEN6X present product=SEN63C` once the sensor answers.

## Orientation

The UI turns its drawing with the device. The board reads the QMI8658A
accelerometer about twice a second (gyro off, 3 Hz low power rate, +-2 g) and
`board::read_orientation()` returns the quarter turn 0 to 3, or nothing while
the device lies flat, the IMU fails or the value is older than 1.5 s. The
shared UI applies the hysteresis and the 2 s stability window. Without an IMU
the board still logs.

The schematic does not show how the IMU axes sit against the panel, and the
Waveshare demo never maps them to a rotation. So the axis table
`kImuToPanel` in `board_io.h` is a provisional guess, IMU x along panel x and
IMU y along panel y. The board logs the raw axes on every change and every
30 s.

```text
OS IMU ax=<x> ay=<y> az=<z> quarter=<0..3|null>
```

**Owner test, opened 2026-11-20, about 5 minutes.** With the board on USB and
`pixi run capture` running, hold or stand the device still for 10 s in each of
these poses and note the `OS IMU` line.

1. Landscape with the bring-up screen text reading upright.
2. Turned a quarter clockwise, so the old bottom edge is on the left.
3. Upside down.
4. Turned a quarter counterclockwise from upright.
5. Flat on the table, panel up.

Expected with a correct table is `quarter=0`, `1`, `2`, `3` and `null` in that
order. If the order differs, the raw values show which IMU axis points down in
each pose. Fix `kImuToPanel` and the static_asserts next to it, then record the
result in the bench record. Until then the UI orientation is unverified.

## First flash

The factory image had a 0x6000 byte NVS partition. The OpenSensor layout uses
0x5000 bytes at 0x9000, so the backup check in `tools/check_idf_target.py`
refuses that backup and `pixi run epaper397-flash` refuses to flash. The helper
is intentionally not changed.

For this board the owner allowed wiping the factory firmware (2026-11-20), and
on 2026-11-20 waived the backup for this round of flashes. A full 16,777,216
byte backup was still taken before the first write. Run the read-only checks,
then write the reviewed build directly with the same `flash_args` the helper
uses, without `erase-flash` and without touching the NVS region.

```sh
pixi run ports
pixi run chip --port <checked port>
pixi run flash-id --port <checked port>
pixi run efuse --port <checked port>
pixi run epaper397-build
pixi run python tools/check_idf_target.py \
  firmware/esp-idf-waveshare-epaper-397/build/logger \
  --board epaper397 --variant logger --verify-manifest
cd firmware/esp-idf-waveshare-epaper-397/build/logger
pixi run python -m esptool --chip esp32s3 --port <checked port> --baud 115200 \
  --before default-reset --after hard-reset write-flash @flash_args
```

Never write eFuses or format the card. Once the board runs OpenSensor, take a
full backup (`pixi run backup --board epaper397 --port <checked port>`) and
use `pixi run epaper397-flash <checked port> <backup>`. That backup holds the
OpenSensor partition table, so the NVS check passes.

## Stack and heap budgets

`pixi run stack-budget --image epaper397/logger --path` prints each task's
deepest static call path. The bring-up display task `os-epd` has a 6 KiB
stack. CI fails the board leg when a task or section grows past
[tools/stack_budget.json](../../../tools/stack_budget.json).
