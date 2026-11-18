# CrowPanel 4.2" e-paper native ESP-IDF adapter

One firmware variant, `logger/`, the full application with the e-paper UI.
It uses this trial's pinned ESP-IDF toolchain, sdkconfig and partition file.
Never borrow a sibling trial's settings.

- Use `pixi run idf-setup`, `pixi run crowpanel-build` and the backup-gated
  `crowpanel-flash` task. No Arduino APIs or components. The owner unit has
  flashed and booted (2026-11-17), see the board bench record before claiming
  any other behaviour.
- Exact board: ELECROW CrowPanel ESP32-S3 4.2" E-paper HMI **v1.2**, UC8276C
  panel, 400 x 300, 8 MB flash and 8 MB Octal PSRAM. Console is UART0 at
  115200 through the board USB bridge. Two 3.75 MiB (0x3C0000) OTA slots, the
  core dump and an unused 384 KiB data partition fill the 8 MiB. The image
  must stay under half a slot, see `tools/ota_slot_check.py`.
- The board adapter (`board_io.*`) owns I2C_NUM_0 for the SEN63C on GPIO8 SDA
  and GPIO9 SCL at 100 kHz, the SDSPI card on SPI3 (MOSI40, MISO13, CLK39,
  CS10, GPIO42 card power), the red LED on GPIO41 and the five keys (rotary
  Up6, Down4, Press5, side Exit1 and Menu2, active low). The SEN63C needs
  external 10 k pull-ups to 3V3 on SDA and SCL.
- The UI task (`os-ui`, core 1) alone owns SPI2 and the panel (`display/`).
  Panel pins are in `opensensor::epd::Pins`, GPIO7 panel power stays high.
  It reads a copy of the latest row and the pairing passkey through the UI
  feed in `logger/main.cpp`, never the logger or the card. The 2 h and 24 h
  chart history lives in RAM and starts empty after each reset.
- The settings popup (`display/settings.*`) is pure UI state. The UI task
  saves it as a versioned blob in NVS namespace `ospanel` key `ui` and hands
  altitude, CO2 self calibration, temperature offset and fan cleaning to the
  board task through `sensor_requests` under the feed mutex. Only the board
  task talks to the sensor. The phone keys of page 7 (`panel.*`, `sen6x.*`)
  come through `config::BoardKeys`. The storage worker parses them with
  `ui::set_phone_key` and leaves them in `phone_pending`, and the UI task
  takes them on its next pass. `shared_panel` is what page 7 reads. Keep the
  key list in `board.json` and `display/settings.*` together,
  `crowpanel-contract-test` checks it. The Wi-Fi row is read-only, because
  `config::apply_lines` is safe only from the control worker.
- Sensor collection is the module table in `logger/sensor_modules.h` from
  the shared `sen63c` manifest. The screen and the stored row come from the
  same `reading_of` result. The checked-in `telemetry_*.inc` must equal the
  manifest expansion. `shipped.lock` is append only.
- Field ids are the 1-based `telemetry_fields.inc` position, append only.
  Every `FIELD` row carries its LIVE key and decimals, so the dict digest
  covers both and any change needs a matching app release and a
  `docs/shared/mobile-migration.md` entry.
- No external RTC. UTC stays null until a host or network anchor, and a reboot
  starts a new clock epoch. Location starts unset, H3 res 8 only.
- Open decisions, both owner calls. `SensorPolicy` is unset, so the board
  refuses sensor identity provisioning. `self_test_field` is
  `pm25_mass_ug_m3`.
- The BLE passkey shows on the panel while a phone pairs. Never put it, the
  device PIN, a Wi-Fi PSK or the LAN token in logs, artifacts or docs.
- Contract changes require `crowpanel-contract-test --sanitize` and
  `crowpanel-ui-test`. Writer and runtime changes also require the shared
  gates, all three board builds and real readback proportional to the change.
