# Waveshare ESP32-S3-ePaper-3.97 with SEN63C

The native ESP-IDF 6.1 application in
[firmware/esp-idf-waveshare-epaper-397](../../../firmware/esp-idf-waveshare-epaper-397/AGENTS.md)
logs a Sensirion SEN63C (PM1, PM2.5, PM4, PM10, temperature, humidity and CO2)
to Parquet on the microSD card every 10 seconds. The 800 x 480 SSD1677 panel
runs the shared e-paper UI (`firmware/common/epaper_ui`), the same pages as
the CrowPanel at the 800 x 480 layout, and turns the picture with the device
when Rotation is Auto. **The owner unit flashed, booted and logged SEN63C rows
on 2026-11-20.** The shared UI on this board is built and host tested, not yet
seen on the glass. Nothing else is qualified yet, see the
[bench record](bench-verified.md).

## Board

- Waveshare ESP32-S3-ePaper-3.97 (SKU 33810), ESP32-S3-WROOM-1-N16R8. 16 MB
  flash and 8 MB Octal PSRAM, as read on the owner unit. Confirm each unit with
  `pixi run flash-id` and `pixi run efuse` before the first flash.
- Console is the ESP32-S3 USB-Serial/JTAG on the USB-C port. GPIO43 and GPIO44
  are UART0 by default, and here they carry the SEN63C bus instead.
- Flash layout is NVS 0x9000, otadata 0xe000, two 6 MiB OTA slots at 0x10000
  and 0x610000, the core dump at 0xc10000 and a data partition at
  0xc20000 (0x3e0000 bytes, 3.875 MiB, label `spiffs`) that holds the internal
  flash archive while no card is usable, see
  [decision record 0004](../../shared/decisions/0004-flash-fallback-archive.md).
  The pins live once in
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

## Keys and the settings popup

The board has four GPIO keys, all active low with the internal pull-ups. The
GPIOs come from the Waveshare ESP-IDF demo,
[waveshareteam/ESP32-S3-ePaper-3.97](https://github.com/waveshareteam/ESP32-S3-ePaper-3.97)
at commit `1ce1c42421c4690aa3a7eef7354599ce00163f59`, file
`ESP-IDF/08_ESP32-S3_e-Paper-3.97/components/button_bsp/button_bsp.c`. The
PWR key sits on the TG28 PMIC, not on a GPIO, and the firmware does not read
it.

| Demo name | GPIO | UI input | Does |
| --- | --- | --- | --- |
| Button_Up | 4 | Up | Previous page, or the row above in the popup |
| Button_Down | 6 | Down | Next page, or the row below in the popup |
| Button_Function | 5 | Press | Full refresh on a page, change or edit a row in the popup |
| Boot | 0 | Menu | Opens and closes the settings popup |

- There is no Exit key and the shared UI needs none. Menu closes the popup
  and, during an Altitude or Temp offset edit, drops the edit and closes the
  popup. The pages wrap, so Up or Down always reach the dashboard.
- GPIO0 is a strapping pin. It is read at reset only, so using it as an input
  afterwards is safe. Holding it while the board resets enters the ROM
  download mode, as on every ESP32-S3.
- The popup rows, their values and the sensor settings behave as on the
  CrowPanel, see
  [its keys section](../elecrow-crowpanel-epaper-42/README.md#keys-and-the-settings-popup).
  Rotation here offers Auto as well, and Auto is the default. The Language
  row after it switches every page between English and Egyptian Arabic in
  the Cairo font, with a full refresh. Arabic reads right to left, numbers,
  units and sensor names stay Latin.
- Settings are saved to the NVS blob `ospanel/ui`, the same format as the
  CrowPanel, 5 seconds after the last change. The `oscfg` and `parquet`
  namespaces are never touched and nothing is erased.
- The phone keys (`panel.*` and `sen6x.*` on `GET_CONFIG` page 7) are not
  declared on this board yet. Declaring them adds `config_keys` to
  `logger/board.json`, which moves the template desc digest and needs the
  pinned URI commits, so it is left for its own change. The knob is the only
  writer for now.

Each settings load and save logs
`OS PANEL settings operation=<load|save> error=<n> ... rotation=<0|90|180|270|auto>`.

## Orientation

The board reads the QMI8658A accelerometer about twice a second (gyro off,
3 Hz low power rate, +-2 g) on the board task and caches the raw counts. The
UI task never touches the I2C bus. While Rotation is Auto, each pass of the
UI loop (at most 200 ms apart, never during a refresh) hands the cached read,
up to 1.5 s old, to the shared `ui::OrientationTracker`. The tracker turns
the picture once a new pose holds for 2 s, ignores a device lying flat
(less than half of gravity in the panel plane) and keeps 15 degrees of
hysteresis past each 45 degree boundary. A turn redraws with a full refresh.
A fixed 0, 90, 180 or 270 setting leaves the tracker alone. Without an IMU
the board still logs, and Auto keeps the last picture.

At boot under Auto the first frame is drawn at 0 and turns about 2 s later if
the device stands another way. Switching to Auto starts from the picture on
the glass. Every change of the shown rotation logs one line.

```text
OS PANEL rotation=<0|90|180|270> source=<auto|setting>
```

The schematic does not show how the IMU axes sit against the panel, and the
Waveshare demo never maps them to a rotation. So the axis table
`kImuToPanel` in `board_io.h` is a provisional guess, IMU x along panel x and
IMU y along panel y. The display profile reads the same table, and
`pixi run epaper-ui-test` checks that the tracker and the board's
`quarter_turn()` agree on every pose. The board logs the raw axes on every
change and every 30 s.

```text
OS IMU ax=<x> ay=<y> az=<z> quarter=<0..3|null>
```

**Owner test, opened 2026-11-20, about 10 minutes.** Set Rotation to Auto
(Boot opens the popup, Down to the Rotation row, Press until it reads Auto,
Boot closes it). With the board on USB and `pixi run capture` running, hold
or stand the device still for 10 s in each of these poses and note the
`OS IMU` and `OS PANEL rotation` lines and the picture.

1. Landscape, held the way a fixed Rotation of 0 reads upright (check it
   with Rotation 0 first if unsure).
2. Turned a quarter clockwise, so the old bottom edge is on the left.
3. Upside down.
4. Turned a quarter counterclockwise from upright.
5. Flat on the table, panel up.

Expected with a correct table is `quarter=0`, `1`, `2`, `3` and `null` in that
order, `OS PANEL rotation=0`, `270`, `180`, `90` for the first four with the
picture upright each time, and no new rotation line in pose 5. If the order
differs, the raw values show which IMU axis points down in each pose. Fix
`kImuToPanel` and the static_asserts next to it, run `pixi run
epaper-ui-test`, then record the result in the bench record. Until then the
UI orientation is unverified.

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
deepest static call path. The UI task `os-ui` has an 8 KiB stack, the same
as on the CrowPanel, and measures 3,408 bytes static worst case. Its logical
frame, the panel frame, the frame on the glass (48,000 bytes each) and the
chart history live in PSRAM. CI fails the board leg when a task or section grows past
[tools/stack_budget.json](../../../tools/stack_budget.json).
