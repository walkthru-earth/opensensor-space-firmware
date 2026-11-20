# Waveshare ESP32-S3-ePaper-3.97 native ESP-IDF adapter

One firmware variant, `logger/`, the full application. It uses this trial's
pinned ESP-IDF toolchain, sdkconfig and partition file. Never borrow a sibling
trial's settings or pins.

- Use `pixi run idf-setup`, `pixi run epaper397-build` and the backup-gated
  `epaper397-flash` task. No Arduino APIs or components. The owner unit
  flashed, booted and logged SEN63C rows on 2026-11-20, see the bench record
  in `docs/boards/waveshare-esp32-s3-epaper-397/` before claiming anything
  else. Its first flash went through esptool directly, the README says why.
- The exact board is the Waveshare ESP32-S3-ePaper-3.97 (SKU 33810), SSD1677 panel,
  800 x 480, 16 MB flash and 8 MB Octal PSRAM. Console is USB-Serial/JTAG,
  because GPIO43 and GPIO44 carry the SEN63C. Two 6 MiB OTA slots, the core
  dump and an unused data partition fill the 16 MiB, NVS stays at 0x9000.
- The board adapter (`board_io.*`) owns I2C_NUM_1 for the SEN63C on GPIO43
  SDA and GPIO44 SCL at 100 kHz, I2C_NUM_0 for the internal bus on GPIO41 SDA
  and GPIO42 SCL (TG28 PMIC 0x34, PCF85063 RTC 0x51, QMI8658A IMU 0x6B), and
  the 4-bit SDMMC card (CLK16, CMD17, D0 15, D1 7, D2 8, D3 18). The SHTC3
  (0x70) and the ES8311 codec (0x18) stay untouched. The SEN63C needs
  external 10 k pull-ups to 3V3.
- The display task (`os-epd`, core 1) alone owns SPI3 and the panel
  (`display/epd_ssd1677.*`). It switches the panel supply on TG28 ALDO3
  through `board::panel_power`. `display/bringup_screen.*` is a temporary
  screen until the shared e-paper UI lands. Do not grow it into a UI, and do
  not touch the CrowPanel display code or create a common UI component here.
- The QMI8658A runs the accelerometer only, 3 Hz low power, gyro off. The
  board task polls it about every 500 ms and `board::read_orientation()`
  returns the quarter turn for any task. The UI owns hysteresis. The axis
  table `kImuToPanel` is provisional until the owner test in the board README
  passes. The board must keep logging when the IMU does not answer.
- The PCF85063 is the RTC hook. UTC stays null until it holds a set time or a
  host or network anchor arrives. Location starts unset, H3 res 8 only.
- Sensor collection is the module table in `logger/sensor_modules.h` from
  the shared `sen63c` manifest. The checked-in `telemetry_*.inc` must equal
  the manifest expansion. `shipped.lock` is append only. Field ids are the
  1-based `telemetry_fields.inc` position, append only. Every `FIELD` row
  carries its LIVE key and decimals, so any change moves the dict digest and
  needs a `docs/shared/mobile-migration.md` entry.
- Never put the device PIN, a Wi-Fi PSK or the LAN token in logs, artifacts
  or docs.
- Contract changes require `epaper397-contract-test --sanitize` and
  `stac-item-test`. Writer and runtime changes also require the shared gates,
  every board build and real readback proportional to the change.
