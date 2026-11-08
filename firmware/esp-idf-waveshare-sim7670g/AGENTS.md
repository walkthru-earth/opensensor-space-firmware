# Waveshare V2 native ESP-IDF adapter

One active firmware variant at a time: `logger/` is the full application;
`diagnostic/` is the retained UART/TF bring-up fixture. Both use this trial's
pinned ESP-IDF toolchain, component manifests and partition files. Never borrow a sibling trial's settings.

- Use `pixi run idf-setup`, `pixi run waveshare-build [logger|diagnostic]`
  and the backup-gated `waveshare-flash` task. No Arduino APIs or components.
  See the newest dated USB Parquet readback in the board bench record, at
  this writing native v2.15 on 2026-10-26. v2.17 and v2.18 have no dated readback yet,
  and its GNSS path has only the empty no-fix reply on the bench
  (board `hardware.md`). Earlier Arduino image results remain historical and
  require native readback to extend.
- Exact board: ESP32-S3-SIM7670G-4G **V2.0**, ESP32-S3R8 rev0.2, 16 MB Quad
  flash and 8 MB Octal PSRAM. OPI was qualified by an allocated-region memory
  test. Preserve UART0 console through CH343; USB CDC is not the console.
- The board adapter owns UART1 PMS5003T RX1/TX2, 9600 8N1; four particle bins
  plus temperature/RH. GPIO2 is valid on V2, conflicting with the V1 gauge.
- Mount TF with SDMMC CLK5/CMD4/D0=6 in one-bit mode without formatting. After
  startup, OSLogger's single worker owns every filesystem operation.
- OSLogger, OSConnectivity, OSRuntime and OSCommon own shared application
  services. Board code supplies only descriptors, sensor collection, card
  capacity hooks and board GPIO setup. No second radio initializer.
- Sensor collection is the module table in `logger/sensor_modules.h` (#131),
  driven by `logger/board.json` segments and the board-local
  `logger/modules/waveshare/module.json`, and the checked-in `telemetry_*.inc`
  must equal their expansion. `shipped.lock` is append only. Never hand edit a
  generated `*_module.h`, and run `plugin-contract-test` after any manifest or
  field change. `tools/waveshare_collect_reference.h` is the pre-module
  collect, and `waveshare-contract-test` keeps both byte identical row for row.
- Reserve the heap-owned 4 KiB internal DMA SD transfer buffer before radios
  and retain it while mounted. Shared connectivity prefers PSRAM for packet/TCP
  memory and bounds static Wi-Fi DMA buffers; the diagnostic excludes radios
  and PSRAM. Source/host validation does not qualify this board's native IO.
- GPIO38 RGB is a board-local startup/card/sensor indicator. Camera, modem
  data and charger control are disabled. The board adapter alone owns MAX17048
  I²C on SDA15/SCL16; never initialize that controller from another driver.
  Never change USB routing or DIP switches for the logger.
- Modem GNSS is on since v2.17 (owner, 2026-10-27), the example moving Thing.
  `logger/main.cpp` only fills `opensensor::gnss::ReaderConfig` (UART2, ESP
  TX GPIO18 to modem RXD, ESP RX GPIO17 from modem TXD, 115200, SIMCom
  dialect) and the GPIO21 modem power callback, then passes
  `opensensor::gnss::latest` as `Hooks::gnss_fix`. GPIO21 is wired OR with the
  4G DIP switch, so it is latched high and never driven low. Leave GPIO45 (DTR,
  a strapping pin) and GPIO40 (RI) unconfigured. Pins come from the V2.0
  schematic, see `docs/boards/waveshare-esp32-s3-sim7670g/hardware.md`. Never
  send modem NVM, band, network, SIM, CFUN or reset commands, and never log
  the IMEI. Parser, freshness and the source rule live in `firmware/common/gnss`.
- The owner subsequently installed an 18650 and verified battery-only boot.
  `kBatteryInstalled` is the explicit board configuration; a MAX17048 response
  still cannot prove battery presence. A failed register nulls only its own
  columns. A good VCELL with a bad SOC still writes `battery_mv` and drives
  `battery_health_state`, which is always written and reads 0 when unknown.
  Charger state has no verified ESP32 signal. No external RTC: UTC remains null until a real host or network anchor,
  and a reboot starts a new unanchored clock epoch.
- Current firmware is `idf-waveshare-parquet-v2.18`, schema v6/dictionary v8
  with 62 fields. Fields 57 to 62 hold MAX17048 diagnostics and the shared
  `power/` battery health state. Battery thresholds live in
  `telemetry_contract.h` (`kBatteryThresholds`) and are uncalibrated until a
  dated discharge run. VCELL senses the VBAT rail, so external power is only
  inferred. The gauge ALRT pin is tied to ground, so never rely on alerts. Preserve the original 52-field order. Field ids are the
  1-based `telemetry_fields.inc` position, so only append fields. Each `FIELD`
  row ends with its LIVE key ("" stays off LIVE) and its LIVE float decimals, so
  the dictionary digest covers both and any change needs a matching mobile app release. Shared H3 helpers
  publish only a coarsened cell and its centre point, the configured cell for a
  fixed Thing and a fresh GNSS cell for a moving one. Raw GNSS coordinates are
  never logged or stored. Country is optional and owner-declared.
- Fixed pairing PIN is random per device. Retrieve it only through the
  explicit physical-serial owner command; never put PIN/PSK/token values in
  logs, artifacts or docs. Preserve network-side encryption/authentication.
- Contract changes require `waveshare-contract-test --sanitize`; writer and
  runtime changes also require shared gates, both board builds and real
  readback proportional to the change. Retain images/logs outside `build/`
  in ignored `artifacts/`, and record hashes and limitations in board docs.
