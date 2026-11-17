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

Not shown yet. What the panel draws, the keys and the settings popup, SEN63C
readings, a finalized Parquet file read back from the card, BLE and LAN sync,
endurance and power-cut durability.
