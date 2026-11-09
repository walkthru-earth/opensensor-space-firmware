# opensensor-space-firmware

OpenSensor firmware is native ESP-IDF firmware for ESP32-S3 Things. A Thing
samples its sensors every 10 seconds, writes immutable Parquet files straight to
its microSD card and syncs them to a phone or laptop over BLE or the local
network, with no internet needed. The firmware is board and sensor neutral.
Each board app plugs its sensors in through logger hooks and a field
dictionary, and reuses the shared components in
[`firmware/common/`](firmware/common/README.md). Air quality is sensor set 1,
the first set of measurements the boards ship with.

## Status

This table is the single source of truth for board versions. Other docs link
here instead of repeating them.

| Board app | Firmware | Schema and dictionary | Columns | Sensor set 1 | Hardware evidence |
| --- | --- | --- | --- | --- | --- |
| [M5Stack CoreS3](firmware/esp-idf-cores3/README.md) | v6.32 | `cores3-telemetry-v6`, `cores3-telemetry-v7` | 81 | Optional M134 (PMSA003), onboard IMU, light, RTC and power | Short native USB and SD readback of v6.21 on 2026-10-26. [Bench record](docs/boards/m5stack-cores3/bench-verified.md#main-689bb61-usb-reflash-thing-id-and-first-geo-footer-2026-10-26) |
| [Waveshare ESP32-S3-SIM7670G-4G V2](firmware/esp-idf-waveshare-sim7670g/README.md) | v2.25 | `waveshare-sim7670g-telemetry-v6`, `waveshare-sim7670g-telemetry-v8` | 62 | PMS5003T with temperature and humidity, MAX17048 battery gauge | Short native USB and SD readback of v2.15 on 2026-10-26. [Bench record](docs/boards/waveshare-esp32-s3-sim7670g/bench-verified.md#main-689bb61-usb-reflash-thing-id-and-first-geo-footer-2026-10-26) |

Phone transfers, endurance and power-cut durability remain unqualified on both
boards. The firmware proves on-device Parquet is feasible, not that it is
production durable. See [security hardening](docs/shared/security-hardening.md)
for the open limits.

## Quick start

```sh
pixi install
pixi run idf-setup
pixi run cores3-build
pixi run waveshare-build              # logger
pixi run waveshare-build diagnostic   # UART and read-only TF fixture
pixi run gate                         # host gate, the same one CI runs
```

Before any physical flash write, follow the
[flash checklist in AGENTS.md](AGENTS.md#before-any-physical-flash-write) on the
exact board and checked serial port. The
[development guide](docs/shared/development.md) covers the toolchain, the flash
tasks and every gate.

## Where to read next

- [AGENTS.md](AGENTS.md) is the entry point for people and coding agents.
- [Documentation router](docs/README.md) explains Things, board apps and sensor
  sets, and routes to every shared contract and board reference.
- [Board porting](docs/shared/board-porting.md) shows how a new board plugs in
  its sensors.
- [Telemetry pipeline](docs/shared/telemetry-pipeline.md) covers sampling,
  nulls, Parquet, rotation, Hive paths and offline capacity.
- [Sync protocol](docs/shared/ble-sync-protocol.md) is the BLE and LAN wire
  contract.
- [Time and location](docs/shared/time-and-location.md) covers router and local
  NTP, RTC anchors and private H3 location.
- [Product notes](product/) hold proposals such as the mobile app and board
  options. They are not implemented features.

## License and attribution

Our work is licensed under [CC BY 4.0](LICENSE), like other Walkthru.Earth
repositories. Third-party code keeps its own license and notices. See
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) and ship it with any firmware
binary you distribute.

**If you use any of this work, you must credit us visibly**, for example in
your README, documentation, about screen or paper. A buried source comment does
not count. State what you changed. Attribution does not imply we endorse your
project. Use at least the credit line below.

> Based on [walkthru-earth/opensensor-space-firmware](https://github.com/walkthru-earth/opensensor-space-firmware)
> by Walkthru.Earth, licensed under [CC BY 4.0](https://creativecommons.org/licenses/by/4.0/).

To cite it in a paper, use [CITATION.cff](CITATION.cff). GitHub shows it as
APA and BibTeX under "Cite this repository".
