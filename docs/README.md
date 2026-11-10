# Documentation router

The device protocol, telemetry format, and offline-first data flow are shared across board trials. Wiring, power, peripheral ownership, build settings, and measured performance belong to the individual board. Source-checked claims and real-device results remain separate.

## Project and sensor sets

This is general OpenSensor firmware for any measurement. Air quality is the
first sensor set, not the whole product. Four terms describe how it fits
together.

- A **Thing** is one deployed device with a persistent UUID. Its files land
  under `output/thing=<UUID>/` and it can move between places.
- A **board app** is one self-contained native ESP-IDF application under
  `firmware/esp-idf-<board>/`. It owns pins, power, peripherals, storage
  mounting and acquisition, and it records its own bench evidence.
- A **sensor set** is the group of measurements a board app collects. The board
  declares it in its field dictionary, `telemetry_fields.inc`, which fixes the
  Parquet columns, units and LIVE keys, and fills each row in `Hooks::collect`.
  Air quality is sensor set 1, with a Plantower particle sensor on both
  current boards. On CoreS3 it is the optional M134 add-on, on Waveshare V2 a
  wired PMS5003T.
- The **shared components** in [`firmware/common`](../firmware/common/README.md)
  are board and sensor neutral. They sample, write Parquet, keep settings and
  sync over BLE and LAN for any sensor set.

To add a board or a new sensor set, start with
[board porting](shared/board-porting.md). Module and board manifests gate each board dictionary, see
[Sensor plugins](shared/plugins/README.md). Current board versions live in the
[root README status table](../README.md#status).

## Shared contracts

| Topic | Reference |
| --- | --- |
| Host tooling and shared modules | [Development workflow](shared/development.md), [shared component map](../firmware/common/README.md) |
| New ESP32-S3 board hooks, shared logger worker, board measurement/RTC hooks and immutable archives | [Board porting](shared/board-porting.md), [OSLogger adapter contract](../firmware/common/logger/README.md) |
| Sampling, nulls, Parquet, Hive partitions, and offline archives | [Telemetry pipeline](shared/telemetry-pipeline.md) |
| Plugin contract (Pilot), versioned module and board manifests gate the board dictionary | [Sensor plugins](shared/plugins/README.md) |
| Dictionary, observation semantics, and proposed Iceberg workflow | [Table and observation model](shared/table-and-observation-model.md) |
| Small file sizes, encodings, compaction and range reads | [Small Parquet files](shared/parquet-small-files.md) |
| Decision records, who writes STAC for a standalone Thing and the small file roadmap | [Decisions](shared/decisions/README.md) |
| STAC metadata contract research, firmware audit and review. The File Item sidecar is built (#142), the Thing Collection is not (#145) | [STAC contract research](shared/stac-contract/README.md) |
| Proto 3 BLE/LAN frames, disconnect evidence, pairing and UART owner provisioning | [Sync protocol](shared/ble-sync-protocol.md) |
| LAN transfer scheduling, socket policy and qualification | [LAN performance](shared/lan-performance.md) |
| Phone visible changes per firmware release and what the app must do | [Mobile migration notes](shared/mobile-migration.md) |
| Published board templates, pinned dictionary and template URIs | [Board templates](shared/templates/README.md) |
| Router/local NTP, configurable H3 privacy and mobile deployment provisioning | [Time and location](shared/time-and-location.md) |
| Phone background sync triggers | [Background sync triggers](shared/background-sync-triggers.md) |
| Security and production-hardening boundary | [Security hardening](shared/security-hardening.md) |
| Mobile app workflows, architecture, board options and other proposals (not implemented) | [Product notes](../product/) |

## Boards

| Board | Status | Reference |
| --- | --- | --- |
| M5Stack CoreS3 with optional M134/PMSA003 | Native OSLogger adapter with a short native USB/SD/sensor/time readback. Proto 3 phone transfers and endurance remain pending. Historical Arduino radio evidence stays image-specific | [CoreS3 board docs](boards/m5stack-cores3/README.md), [trial](../firmware/esp-idf-cores3/README.md) |
| Waveshare ESP32-S3-SIM7670G-4G V2 with PMS5003T | Native ESP-IDF logger and retained diagnostic with a short native USB/SD readback. Phone transfers and endurance remain pending. Historical Arduino evidence stays in its bench record | [Waveshare board docs](boards/waveshare-esp32-s3-sim7670g/README.md), [trial](../firmware/esp-idf-waveshare-sim7670g/README.md) |

The eleven shared ESP-IDF components are OSCommon (encoding and the Parquet writer), OSSensors (sensor drivers),
OSRuntime (settings/logs), OSConnectivity (local transports), OSLogger (sampling/archive worker),
OSLocation (official H3 geometry), `stac` (STAC File Item builder), `gnss` (GNSS fix type, receiver dialects
and row source rule), `gnss_reader` (UART reader task), `power` (battery health classifier) and `upload`
(off by default source.coop uploader). A shared
source or host gate is not a hardware endurance result.

For any physical write, follow the board-specific safety sequence in [AGENTS.md](../AGENTS.md). Keep captures and firmware binaries in the relevant trial's ignored `artifacts/`, and record real-board measurements only in that board's bench record.
