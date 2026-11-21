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
- `board_io` reserves the 4 KiB internal DMA SD transfer buffer before radios
  and keeps it for the whole boot, also after a failed mount (#168). `OS TF`
  reports `dma_buffer=0|1`. A failed remount logs `reason=spi_bus_unavailable`
  or `reason=dma_buffer_unavailable` once each, by the stage that failed.
- The UI task (`os-ui`, core 1) alone owns SPI2 and the panel (`display/`).
  Panel pins are in `opensensor::epd::Pins`, GPIO7 panel power stays high.
  It reads a copy of the latest row and the pairing passkey through the UI
  feed in `logger/main.cpp`, never the logger or the card. The 2 h and 24 h
  chart history lives in RAM. At boot the board task seeds it once from this
  Thing's own finalized Parquet (`logger/history_restore.*`), after the card
  mounts and before `oslogger::begin`, so no storage worker and no UI task
  run yet. It is read only on the card and NVS, bounded to 24 h of rows and
  about 8 s, and logs one `OS HISTORY` line. A failure only leaves the charts
  empty, it never stops logging. Limits are in the board README.
- Pages, charts, fonts, the settings popup and the refresh policy are the
  shared `firmware/common/epaper_ui` component, see its AGENTS.md. This board
  keeps only its driver (`display/epd_uc8276c.*`) and its
  `display/display_profile.h`, which must stay equal to the host 400 x 300
  profile so the golden frames are the frames it sends. A UI change goes in
  the shared component and reaches every e-paper board. The UI draws in
  English or Egyptian Arabic from `firmware/common/text`, so this board
  lists `../common/text` in `EXTRA_COMPONENT_DIRS`.
- The settings popup is pure UI state. The UI task
  saves it as a versioned blob (version 3, with the language) in NVS
  namespace `ospanel` key `ui` and hands
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
  `epaper-ui-test`. Writer and runtime changes also require the shared
  gates, all three board builds and real readback proportional to the change.
