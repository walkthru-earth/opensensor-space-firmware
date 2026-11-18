# CrowPanel 4.2" e-paper bench record

**Scope, 2026-11-17.** The native ESP-IDF 6.1 logger image for the ELECROW
CrowPanel ESP32-S3 4.2" E-paper HMI v1.2 (UC8276C panel) with a Sensirion
SEN63C builds and passes its host contract gates. The first entry below is
host only, the second is the first flash and boot. Board identification, the first flash, boot, display, SEN63C
readings, SD logging and BLE/LAN sync each need their own dated entry with the
image identity and its limits before any claim moves into the board README.

## Image identity, 2026-11-17

- Firmware `idf-crowpanel-parquet-v0.1`, dictionary
  `crowpanel-epaper-telemetry-v1`, dict digest
  `e7279c4edbe87071e4a51889124a43d7ffcd1977c79c8575887ae35bdee10903`.
- Host build only. The app image is 1,702,288 bytes in a 3 MiB (0x300000) OTA
  slot, 1,443,440 bytes (46%) free.

No board was connected to a serial port, flashed or captured for this entry.

## Flash and boot, 2026-11-17

- Owner unit on a CH340 USB bridge (VID:PID 1A86:7523). ESP32-S3 rev v0.2,
  flash manufacturer 0x20 device 0x4017, 8 MB, PSRAM 8 MB AP_3v3.
- Full 8,388,608 byte backup before the write, SHA-256
  `e5bcd82e15808d8d96c0764bef9be043be2739408391db2c928e0856940b3991`.
- Image `opensensor_crowpanel.bin` built from commit `78fc1fe`, SHA-256
  `0437f7134613564fe45367ae7b489b2e0a7c61aed465ea4289eb1640adedb6ab`, written
  with `pixi run crowpanel-flash` and hash verified. The first attempt lost
  the serial link at 6% of the app write and was rerun, the second wrote and
  verified all regions.
- The 35 second boot capture shows `OS BOARD`, `OS MEMORY flash_bytes=8388608
  psram_ok=1`, `OS PANEL settings operation=load error=0` with defaults,
  `OS DISPLAY ready=1`, `OS TF mounted=1`, `PARQUET STORAGE state=ready`,
  `OS READY logging=1` and rows 0 to 2 at 10 s with no drops. No panic or
  reset.
- No SEN63C was wired. The log shows `OS SEN6X absent error=write-nack`, as
  expected, so rows hold null sensor values.

## Reflash with 3.75 MiB app slots, 2026-11-17

- Same owner unit and port, checked again with `ports`, `chip`, `flash-id`
  and `efuse` (MAC unchanged, 8 MB flash, PSRAM 8 MB AP_3v3).
- Full 8,388,608 byte backup before the write, SHA-256
  `99bd40af0a2200cb712c0249af449ecc77897ff9bcc4fabcfbacd38d2b8f1a1d`. The
  first backup attempt stopped at 72% with a short serial read and wrote no
  file, the second completed and verified.
- Image `opensensor_crowpanel.bin` built from commit `a25aeca`, 1,712,224
  bytes, SHA-256
  `a626960ea7cc3d750a95f7639cc0b017b4ba13bba8dded46e3402f66dd323a01`, written
  with `pixi run crowpanel-flash` in one pass, all four regions hash verified.
  This write moved the board to the new partition table, app1 at 0x3d0000,
  coredump at 0x790000 and the unused data partition at 0x7a0000, with NVS
  still at 0x9000.
- The 60 second boot capture shows `App version: a25aeca`, the coredump
  partition found at 0x790000, `OS PANEL settings operation=load error=0`,
  `OS DISPLAY ready=1`, `OS TF mounted=1`, `PARQUET STORAGE state=ready`,
  `PARQUET SCAN files=2 retained_partials=0 quarantine_files=0`, `OS READY
  logging=1` and rows 0 to 4 at 10 s with no drops. No panic or reset.
- Still no SEN63C wired, `OS SEN6X absent error=write-nack` as expected.

## Theme setting image, 2026-11-17

- Same unit and port, `ports`, `chip`, `flash-id` and `efuse` checked again.
  Full 8,388,608 byte backup before the write, SHA-256
  `c100b7cf678b6a614d231782d2d14459584087c9d2f7674b37b055ddefcf2a96`.
- Image built from commit `bd75c21`, SHA-256
  `4e00de0d045da915323fdfcc060ef149a932ec8f435340f5daf5279cb6e36c42`, written
  with `pixi run crowpanel-flash`, all four regions hash verified.
- The 40 second boot capture shows `App version: bd75c21`, `OS PANEL settings
  operation=load error=0 ... theme=light`, `OS DISPLAY ready=1`, `OS READY
  logging=1` and rows 0 to 2 at 10 s with no drops. No panic or reset.
- How the Dark theme looks on the glass and any ghosting are not checked yet.

## First SEN63C wiring, 2026-11-17

- Same unit, port and image (`App version: bd75c21`), no flash write. The
  SEN63C was wired with four jumpers on pins 1 to 4 (3V3, GND, IO8 SDA, IO9
  SCL), with no external pull-ups, so the bus ran on the ESP32-S3 internal
  pull-ups only. That is outside the I2C rise time limits and is not a
  qualified setup.
- The first try counted the sensor socket from pin 6, which swaps SDA and SCL
  and leaves power and ground right, because pins 1 and 6 are both VDD and
  pins 2 and 5 are both GND. The board logged `OS SEN6X absent
  error=write-nack` and nothing was damaged. Pin 1 is on the left facing the
  socket, Sensirion SEN6x datasheet figure 4.
- After the swap the 90 second capture shows `OS SEN6X present
  product=SEN63C serial=1554990D1F2585A5 version=5.3 match=1`, `OS SEN6X
  started`, rows 0 to 7 at 10 s with `dropped=0` and no I2C or CRC error.
- The first status read after the start showed `co2_error=1` (0x00001000),
  cleared by the board, and every later read was 0. The capture reset the
  board a few seconds after the owner powered it, so the sensor was most
  likely restarted inside the 24 s CO2 conditioning window of the previous
  boot's start (datasheet 4.8.1). The driver cannot see a start made before a
  reset. Not reproduced on a cold power up yet.
- The owner saw readings on the panel with the Dark theme on. The values were
  not recorded and are not checked against a reference.

## Chart history restore, 2026-11-18

- Same unit and port, SEN63C still on internal pull-ups. Full 8,388,608 byte
  backups before each flash write.
- Image `215b445` logged `OS HISTORY restore=none reason=order-unknown
  ms=89` on a card holding 32 files of this Thing. Some earlier boots took
  UTC from the bonded phone before their first file, so they wrote only
  dated folders and never entered the NVS boot ring, and nothing proved
  their order against bench boots without UTC.
- Image `945203b` adds the card usage fallback. The 45 second capture
  `artifacts/history-restore-945203b-20261118.log` shows `OS HISTORY restored
  boots=10 files=35 rows=2840 span_s=28300 ms=2239 skipped=0 cut=none
  ring_boots=9 dated_boots=1 placed=10 fallback=3`, then `OS READY
  logging=1`, with no watchdog line. The board decoded 35 finalized files
  from the card on device.
- Not shown yet. The chart on the glass after the restore and a real power
  cut test, both owner checks.

Not shown yet. The keys and the settings popup on the glass, SEN63C values
against a reference, the bus with the 10 k pull-ups, BLE and LAN sync,
endurance and power-cut durability.
