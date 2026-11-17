# CrowPanel 4.2" e-paper bench record

**Scope, 2026-11-17.** The native ESP-IDF 6.1 logger image for the ELECROW
CrowPanel ESP32-S3 4.2" E-paper HMI v1.2 (UC8276C panel) with a Sensirion
SEN63C builds and passes its host contract gates. No hardware run is claimed
here yet. Board identification, the first flash, boot, display, SEN63C
readings, SD logging and BLE/LAN sync each need their own dated entry with the
image identity and its limits before any claim moves into the board README.

## Image identity, 2026-11-17

- Firmware `idf-crowpanel-parquet-v0.1`, dictionary
  `crowpanel-epaper-telemetry-v1`, dict digest
  `e7279c4edbe87071e4a51889124a43d7ffcd1977c79c8575887ae35bdee10903`.
- Host build only. The app image is 1,702,544 bytes in a 3 MiB (0x300000) OTA
  slot, 1,443,184 bytes (46%) free.

No board was connected to a serial port, flashed or captured for this entry.
