# Waveshare ESP32-S3-ePaper-3.97 bench record

**Scope, 2026-11-20.** The native ESP-IDF 6.1 logger image for the Waveshare
ESP32-S3-ePaper-3.97 (SKU 33810) with a Sensirion SEN63C. Each entry names
the image it ran and its limits. Nothing moves into the board README as a
claim without a dated entry here.

## Image identity, 2026-11-20

- Firmware `idf-epaper397-parquet-v0.1`, schema and dictionary
  `waveshare-epaper-397-telemetry-v1`, 39 columns.
- dict digest
  `a19c450b6fc5bc4bb76e9b52e03aa75bee05cd3c1505d961771279366c820439`, desc
  digest `5ef08e04a98cc572a350914b10ae554e85a2823ac923e8c0037b7d376e93f3ac`.
- `pixi run epaper397-build` and `epaper397-contract-test --sanitize` pass.
  The app image sits in a 6 MiB (0x600000) OTA slot with 73% free.

## Identification and backup, 2026-11-20

- Owner unit on the ESP32-S3 USB-Serial/JTAG, `/dev/cu.usbmodem112301`.
- `chip` read ESP32-S3 (QFN56) rev v0.2 with embedded PSRAM 8 MB (AP_3v3).
  `flash-id` read manufacturer 0x46 device 0x4018, 16 MB. `efuse` read
  only, secure boot off and USB JTAG enabled. No eFuse was written.
- Full 16,777,216 byte backup
  `backup/waveshare-epaper-397-flash-20261120T131039Z.bin`, SHA-256
  `281c4ab0a8e954428f7d76a6b34bcc249497d748ec42514eed45b789850befc4`,
  matching the 16 MB that `flash-id` reported.

## First flash and boot, 2026-11-20

- The factory partition table had a 0x6000 byte NVS, so
  `pixi run epaper397-flash` refused the backup ("backup NVS region
  differs"). The owner had allowed wiping the factory firmware, and later the
  same day waived the backup for this round. The helper was not changed.
- After `check_idf_target.py --verify-manifest` passed, the image was written
  with `esptool write-flash @flash_args` from `build/logger` at 115200 baud.
  It wrote the bootloader, partition table, otadata and app at 0x0, 0x8000,
  0xe000 and 0x10000, with no `erase-flash`, and every region was hash
  verified.
- Image `App version: 704270f-dirty`, ELF SHA-256 starting `f6688ecad`, app
  1,690,816 bytes. The dirty tree held the bring-up screen cleanup committed
  next as `cc94f13`.
- The 75 s boot capture (`artifacts/boot-2026-11-20.log`, ignored) shows
  - `OS MEMORY flash_bytes=16777216 psram_physical_bytes=8388608 psram_ok=1`
  - `OS PMIC chip=tg28 found=1 chip_id=0x4a`, `OS POWER vbus=1 battery=0`
  - `OS I2C scan bus=internal count=5 addresses=0x18,0x34,0x51,0x6b,0x70`
  - `OS DISPLAY ready=1 panel=ssd1677 width=800 height=480`, then
    `OS DISPLAY refresh=full ok=1 ms=2531` about 70 s after boot
  - `OS I2C scan bus=sensor count=1 addresses=0x6b` and `OS SEN6X present
    product=SEN63C version=5.2 match=1`, then `OS SEN6X started`
  - `OS TF mounted=1 size_bytes=31457280000 bus=sdmmc width=4
    format_attempted=false`
  - `PARQUET STAC sidecar=true`, `PARQUET BEGIN
    schema=waveshare-epaper-397-telemetry-v1 columns=39`
  - `OS READY logging=1 mounted=1 display=1 sensor=1`
  - rows 0 to 6 at 10 s with `dropped=0`, no panic and no reset.
- `PARQUET RTC state=unusable-or-absent`. The PCF85063 answers but was never
  set, so UTC stays null.

Open findings from this boot.

- One `PARQUET ERROR operation=worker-stall seconds=5 state=running` while the
  storage worker mounted the 32 GB card, before the first row. Logging went
  on and no row dropped. Not investigated.
- The first SEN63C status read after the start showed `co2_error=1`
  (0x00001000), cleared, and the next reads were 0. The CrowPanel saw the same
  after a reset inside the CO2 conditioning window.
- The SEN63C ran on the ESP32-S3 internal pull-ups only, which is not a
  qualified setup. Add the 10 k pull-ups.

## IMU orientation hook, 2026-11-20

- Commit `b76abc6` adds the QMI8658A accelerometer (0x6B) and
  `board::read_orientation()`. Built only, image SHA-256
  `85e61438ae432006fd1cbe20c1ec8534c679b7cff6b97e1410245ce68436dd47`, app
  1,692,896 bytes. The board was no longer on USB when this image was ready,
  so it is not flashed yet and no `OS IMU` line has been captured.
- The IMU to panel axis map is provisional. The owner test in the board
  README confirms or fixes it.

Not shown yet. The panel content and its orientation on the glass, partial
refresh, the IMU lines on device, battery operation, the RTC set and kept
across a power cut, SEN63C values against a reference, BLE and LAN sync, OTA,
endurance and power-cut durability.
