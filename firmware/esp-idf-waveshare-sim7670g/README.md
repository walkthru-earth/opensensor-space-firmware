# Waveshare V2 native ESP-IDF / PMS5003T logger

The current source is pure **ESP-IDF 6.1**, firmware identity
**`idf-waveshare-parquet-v2.26`**, with **62-column** schema
`waveshare-sim7670g-telemetry-v6` and dictionary `waveshare-sim7670g-telemetry-v8`.
The original 52 columns retain their order and types, with four nullable
location fields (`h3_cell_id`, `h3_resolution`, `location_source` and the WKB
Point `geometry`) and, since v2.7, six battery health columns appended (raw SOC,
CRATE, gauge STATUS flags, hibernation, `battery_health_state` and
`battery_minutes_to_empty`). The shared `power/` classifier reports
unknown, external power likely, ok, low, critical or settling. A critical
entry asks the storage worker to finalize the open file once per discharge,
with bounded retries while the card is busy. It never resets or sleeps. Thresholds are
uncalibrated and have no bench evidence yet. Since v2.6 the UTC instants are `event_time_utc_us`
and `clock_anchor_utc_us` with `TIMESTAMP(MICROS, UTC)`, and every leaf
carries Parquet `field_id` = its 1-based dictionary position (see
[mobile migration](../../docs/shared/mobile-migration.md)). The board has two native variants:
`logger/main.cpp` for the full application and `diagnostic/main.cpp` for the
retained UART/read-only TF fixture. `board_io.*` owns native UART, I²C, SDMMC
and indicator adapters. There are no Arduino APIs or Arduino component.
Shared radio code uses native NimBLE/Wi-Fi/netif/SNTP and managed mDNS
**1.15.1**. The RGB adapter uses led_strip **3.1.1**. These stable versions
were checked on **2026-11-09**. LZ4 is vendored **1.11.0**, and this board
has no M5Unified/M5GFX dependency. The optional shared location component uses
embedded-h3-location **v2.0.0** with official H3 **4.5.0**.

**Native hardware qualification is pending.** Arduino-era SD, sensor, battery
and pairing observations below stay tied to their recorded images. Host tests
and source migration do not establish native hardware behavior.

Native build/host gates passed on **2026-10-09**; see the
[shared validation table](../../docs/shared/development.md#native-migration-validation-2026-10-09).

## Current build and flash workflow

```sh
pixi install
pixi run idf-setup
pixi run waveshare-build              # logger
pixi run waveshare-build diagnostic   # UART/read-only TF fixture
pixi run waveshare-contract-test --sanitize
```

The shared SDK pin is in `../../tools/idf-dependencies.lock`. Variant manifests
[`components/os_waveshare_logger/idf_component.yml`](components/os_waveshare_logger/idf_component.yml)
and [`components/os_waveshare_diagnostic/idf_component.yml`](components/os_waveshare_diagnostic/idf_component.yml)
with their CMake files select the required native components. Defaults select
**16 MB QIO flash**, **8 MB OPI PSRAM** for the logger, **no PSRAM** for the
diagnostic, and the **UART0 console at 115200 through CH343**. USB CDC is not
the console. Existing custom partition offsets are preserved. Build wrappers
check vendor/dictionary hashes and generated target configuration; ephemeral
outputs are in `build/logger/` or `build/diagnostic/`. Retain reviewable
binaries, ELF files, hashes and captures in ignored `artifacts/`. ELF source
paths are relative to the repo root, so run `addr2line`, `gdb` and the core
dump tools from there.

Before any write, complete the [exact-board identification and full-backup sequence](../../AGENTS.md#before-any-physical-flash-write)
for **V2.0**, including a matching **16,777,216-byte** full backup. Flash the
verified selected variant without an implicit rebuild or NVS erase:

```sh
pixi run waveshare-flash <checked-port> backup/waveshare-sim7670g-v2-flash-<timestamp>.bin
pixi run waveshare-flash <checked-port> backup/waveshare-sim7670g-v2-flash-<timestamp>.bin diagnostic
```

Keep the `parquet` namespace with its `thing` key and the unread old `station`
key, and keep every key and value type. Since v2.10 (#93)
settings live in the `oscfg` namespace and nothing is copied from the old one.
It starts empty after that reflash, so the pairing mode and PIN, the display
flag, Wi-Fi on, the SSID and password, the `lan.on` switch (back to its default
of on), the LAN sync token, the sensor identity (`sensor.vendor`,
`sensor.model`, `sensor.serial`, `sensor.batch_candidate`), the location and
the NTP settings must be provisioned again, as on a new board. Until the sensor
identity is set again, Waveshare files carry no sensor identity and
`pm25_batch_candidate_status` stays 0, disabled. Bonds survive the reflash and
the phone may cache the old GATT table, so forget the board in the phone's
Bluetooth settings and send `ble.clear_bonds=1` before pairing again. The old
settings and crash log namespaces stay in flash with the old Wi-Fi password and
LAN token, and only a full flash erase removes them. Do not erase Thing
identity or settings. Never write eFuses or format TF.

## Native board ownership and qualification

The V2 adapter owns PMS5003T UART1 at **9600 8N1, RX GPIO1 / TX GPIO2**,
one-bit SDMMC **CLK5 / CMD4 / D0=6** mounted at `/sd`, MAX17048 I²C
**SDA15 / SCL16**, and RGB **GPIO38**. It keeps the explicit
`kBatteryInstalled` setting and read-failure/null behavior. Charger state has
no verified ESP32 signal. Modem data and camera remain outside this logger.
From v2.17 the logger reads GNSS from the SIM7670G on `UART_NUM_2`, ESP TX
GPIO18 and RX GPIO17 at 115200, after latching the modem enable GPIO21 high.
A moving Thing labels each row with the fresh GNSS cell (`location_source`
2) or nulls, a fixed Thing keeps its configured cell. See
[hardware](../../docs/boards/waveshare-esp32-s3-sim7670g/hardware.md) and
[moving Things](../../docs/shared/time-and-location.md#moving-things-and-gnss).
OSLogger owns the 10-second monotonic sampling and sole archive worker;
there is no external RTC anchor. Raw PM/temperature/RH and optional sensor
identity/batch candidate semantics retain the existing dictionary contract.

Both variants reserve a heap-owned **4 KiB internal DMA transfer buffer** before
mounting TF and lend it to ESP-IDF for **eight-sector chunks**. The single card
owner retains it for the mount; failed mounts release it. This prevents
per-transfer DMA allocation when FatFs buffers live in PSRAM. The logger's
shared connectivity prefers PSRAM for Wi-Fi/lwIP packets and uses **eight
static RX and eight static TX DMA buffers**, **32 software RX buffers** and
**16 cached TX buffers**. The diagnostic has no radios or PSRAM. These changes
use the pinned SDK's [SD buffer API](https://docs.espressif.com/projects/esp-idf/en/v6.1/esp32s3/api-reference/storage/sdmmc.html#unaligned-buffer-performance);
native Waveshare throughput/coexistence remains unqualified.

Location starts unset. Owner provisioning supplies a cell and optional
declared ISO country code. The resolution is fixed at **8** (#139), and only the
resolution 8 parent of the cell is persisted. Its centre supplies the
`geometry` WKB Point, alongside `h3_cell_id`, `h3_resolution` (always 8) and
`location_source`. Each queued sample owns its location
snapshot, and cell or country changes split RAM batches and
files, preserving older file metadata. See
[time and location policy](../../docs/shared/time-and-location.md).

Wi-Fi time synchronization prefers router DHCP option 42 and configured local
servers, with public fallback enabled by default. Authenticated host time still
works offline. This board has no external RTC: every boot starts with null UTC
until a real host or network anchor is accepted.

Qualify console startup, physical OPI PSRAM, TF read/write, sensor warm/stale
nulls, gauge readings, indicator behavior, secure pairing, Wi-Fi provisioning,
mDNS/LAN transfer and immutable USB/BLE/LAN file readback on the native image.
Record date, image hashes, readback and limits in the [board bench record](../../docs/boards/waveshare-esp32-s3-sim7670g/bench-verified.md).
Only one host process owns the console at a time. Physical owner/provisioning
helpers retain the existing protocol and keep credentials out of artifacts:

```sh
pixi run parquet-device command --port <checked-port> 'parquet status'
pixi run parquet-device sync-time --port <checked-port>
pixi run owner-pin --port <checked-port>
```

## Historical material below

Following Arduino APIs, build commands, footprint numbers and hardware results
describe earlier images. The old build commands are retained to interpret those
records and are not the current workflow; use the native tasks above.

## Historical Arduino implementation and bench record

**Historical status: logger; full-image boot, SD Parquet write/serial readback, unsynced/host-UTC behavior and BLE advertising/discovery verified 2026-09-28. Encrypted BLE pairing/file transfer and Wi-Fi/LAN operation remain unverified.** Framework: Arduino-ESP32 **3.3.11**, Arduino CLI **1.5.1**, NimBLE-Arduino **2.5.1**, and LZ4 **1.10.0**, pinned in the historical trial lock retained in Git history. The trial consumes OSCommon, OSRuntime, OSConnectivity and OSLogger from `firmware/common`.

The separate trial is justified by a concrete board change: Waveshare V2 uses GPIO1/2 for PMS5003T and one-bit SDMMC storage, while CoreS3 uses M5Unified, GPIO18/17 for PMSA003 and SPI SD. PMS5003T also replaces two frame words with temperature/RH. The trial owns its build settings, partitions, measurement dictionary and board adapter; it does not include another trial.

## Historical Arduino build and flash

```sh
pixi install
pixi run waveshare-setup
pixi run waveshare-build               # logger, 16 MB Quad flash / OPI PSRAM
pixi run waveshare-diagnostic-build    # retained UART/read-only TF diagnostic
pixi run waveshare-contract-test --sanitize
```

The default logger image is `build/logger/logger.ino.bin`. The diagnostic image is `build/diagnostic/diagnostic.ino.bin` and keeps PSRAM disabled. Logger builds check the dictionary SHA-256 and selected flash/PSRAM SDK. The logger requires initialized physical **8 MiB OPI PSRAM** before starting. On 2026-09-28 the separate probe initialized 8 MiB and passed four patterns over a 512 KiB allocation; the full logger subsequently booted and used its PSRAM workspace. These runs do not establish whole-memory endurance. The first full logger build used 1,258,779 bytes program storage and 84,412 bytes static RAM; build size is tied to that image, not a permanent limit.

Before the first write, follow the [identification and full-backup sequence](../../AGENTS.md#before-any-physical-flash-write) on the checked port, confirm V2.0/16 MB flash and recoverable fuses, and retain the verified 16,777,216-byte backup. Flash commands require the named Waveshare backup and verify that the selected build target matches:

```sh
pixi run waveshare-flash <checked-port> backup/waveshare-sim7670g-v2-flash-<timestamp>.bin
pixi run waveshare-flash <checked-port> backup/waveshare-sim7670g-v2-flash-<timestamp>.bin diagnostic
```

[Recovery and pin ownership](../../docs/boards/waveshare-esp32-s3-sim7670g/hardware.md) are board-specific. A flash write changes ESP32 firmware; it does not format the TF card or write eFuses. Keep captures, fetched files, binaries and build reports under this trial's ignored `artifacts/`, outside `build/`.

## Logger and measurement contract

The historical `logger/logger.ino` owned UART1 at 9600 8N1, GPIO1 RX / GPIO2 TX, explicitly selects `Pms5003t`, and mounted TF at `/sd` with `SD_MMC.setPins(5, 4, 6)` and one-bit mode without formatting. The shared engine owns the ten-second monotonic deadline, one filesystem worker, bounded queues, row groups, file rotation, archive commands and local radio transports. The RGB indicator implementation reports startup/card/sensor state; its visual behavior has not been checked and it does not certify a finalized or copied file.

The [52-field dictionary](logger/telemetry_fields.inc) is **`waveshare-sim7670g-telemetry-v2`**, with firmware identity **`arduino-waveshare-parquet-v2.1`** and a compiled SHA-256. It includes CF=1/atmospheric PM1/PM2.5/PM10, four particle-count bins (>0.3/>0.5/>1/>2.5 µm), PMS5003T ambient temperature and RH, sensor/parser status, runtime/storage counters and clock provenance. There are no >5/>10 µm counts or CoreS3 IMU/display/touch/RTC fields. Missing, warming (<30 s), stale (>5 s), sensor-error and model-mismatch snapshots retain null raw measurements. Receipt/completion timestamps describe acquisition timing, not instrument phenomenon time or calibration.

The owner can save the actual Plantower/PMS5003T sticker through authenticated CONFIG. The physical UART parser remains PMS5003T regardless of that setting. Version 2 stores the 9–17 sticker digits per row when the provisioned label has the exact `PMS5003T-` + valid date + 1–9 unit-digit shape. The separate `pm25_batch_candidate_ug_m3` is **off by default** and uses `0.003964 * particles_gt03_per_01l` only for opted-in stickers from batch `PMS5003T-20260408` with a valid PMS snapshot. Candidate status is 0 disabled, 1 identity mismatch, 2 snapshot unavailable, or 3 candidate calculated. This AirGradient candidate has not been validated against a Cairo reference. Raw atmospheric/CF=1/count fields and LIVE `pm25` remain unchanged, and file `calibration_id` remains `unknown`. The serial and batch rules live in [`logger/pms5003t_identity.h`](logger/pms5003t_identity.h) and reach the board-neutral runtime through `config::set_sensor_policy`. A per-file serial is intentionally omitted because the owner can replace the sensor during an open file; the per-row serial code records the active setting. Host validation and a build pass, but no v2 board flash/readback has occurred.

The owner subsequently installed an 18650 and verified battery-only boot. The logger now explicitly configures battery presence with `kBatteryInstalled=true`, and reads MAX17048 VCELL/SOC over I²C on SDA15/SCL16 for `battery_mv` and whole `battery_percent`. The percentage is truncated and capped at 100; raw estimates through 120% are accepted as a project heuristic after this unit returned 107.8% on USB. These existing row fields also flow into live BLE/LAN JSON as `bat` and `pct`; no protocol or Android battery-card change is needed for those values. A failed or implausible read gives `gauge_status=1`. VCELL and SOC are checked on their own, so a good VCELL with a bad SOC still writes `battery_mv` and drives the battery health state while `battery_percent` stays null. Status 2 needs both words. Set `kBatteryInstalled=false` for an image used without a battery (status 0/null), because a gauge response does not prove presence. Charging and USB input status remain unknown: the V2 schematic does not show a verified ESP32 charger-status signal, so live `chg`/`vbus` are omitted. Camera, cellular modem and GNSS are not initialized, and no serial number, location, calibration or UTC is invented. A three-row USB-attached finalized-file readback verified the corrected gauge path. The connected Android app displayed 100% / 4,277 mV with board USB attached and 100% / 4,067 mV after the board was unplugged, in Detailed view.

Unknown UTC uses the `unsynced` Hive tree. An explicit host `SET_TIME`/`parquet time` supplies an anchor for this boot; there is no external RTC restore. Captured rows keep their original clock epoch. Unsynced and host-synchronized serial readbacks have been checked; UTC accuracy has not been measured. See [storage and sync](../../docs/boards/waveshare-esp32-s3-sim7670g/storage.md) for rotation, partial files, readback and remaining measurements.

## Headless pairing and local provisioning

The logger starts BLE without a display. On first use, shared settings generate a per-device random **fixed** pairing PIN; existing NVS settings remain authoritative. Retrieve it only through the physical checked UART:

```sh
pixi run owner-pin --port <checked-port>
```

The PIN bypasses the debug ring and has no BLE/LAN retrieval opcode. Do not retain its output in captures or artifacts. The normal phone CONFIG path configures Wi-Fi; Wi-Fi is off until enabled. To copy Wi-Fi SSID/PSK and Wi-Fi/LAN enable flags from another owner-controlled OSLogger console:

```sh
pixi run provision-usb --source-port <source-checked-port> --port <waveshare-checked-port>
```

This explicit command writes destination settings. Credentials stay in host memory and are not printed or saved; pairing PINs, LAN tokens and Thing identities remain per device. BLE and token-authenticated LAN support the existing protocol; file sync needs no internet or SIM. Only one host process may own each serial port.

## Verification scope

**Last verified on real hardware: 2026-09-30.** The earlier 2026-09-28 run covered identification/full backup, diagnostic boot, TF mount with reported 31,457,280,000-byte capacity, PMS5003T reception, the OPI PSRAM probe, full logger boot and serial Parquet readback. One unsynced file held **16 rows × 49 columns**, **12,060 bytes**, CRC-32 **`102bc7ff`**, with matching PyArrow/DuckDB values and nulls. Its first two snapshots had warm-up nulls, later PMS readings were valid, temperature was **24.8 °C** and RH **41.9–42.2%**. Gauge status was zero, all battery and UTC/anchor values were null, adjacent sampling intervals were within **1 ms** of ten seconds, and no errors/drops were observed in that bounded run. A subsequent host-UTC change/readback was also checked. The 2026-09-30 USB-attached battery run had three valid gauge rows at 4,275 mV and capped 100% with no drops/errors. Firmware images, file hashes and methods belong in the [bench record](../../docs/boards/waveshare-esp32-s3-sim7670g/bench-verified.md).

BLE advertising and macOS discovery of **`AQ-7500`** were verified. The first encrypted-pairing attempt timed out; the shared fixed-PIN callback fix was deployed and emitted its pairing marker. The companion app later recorded a foreground pairing repair and live sensor values on an earlier image, but live battery delivery and BLE file transfer after this image remain unverified. Wi-Fi was not configured, so LAN provisioning/discovery/file transfer remain unmeasured. No RGB visual verification, radio-load endurance or power-cut test is claimed.

Sanitized synthetic contract tests exercise the actual shared parser/writer, warm/stale/error/model gates, signed temperature/RH, absent/read-error/valid gauge gates and register scaling, clock epochs, dictionary digest and both PyArrow/DuckDB readers for uncompressed/LZ4 files. The bounded physical battery readback separately verified stored gauge values, and the phone displayed live battery values on USB and battery-only power. These checks do not establish percentage accuracy, power-cut durability, timing under radio load, file transfer or environmental accuracy. The Android companion code transfers/indexes files without a CoreS3 schema/count restriction and already has ambient/RH history metrics; its live model lacks `rh` and labels `t` as board temperature, so full live environmental display needs a small companion update.

**Next hardware gates:** owner-completed first secure pairing and encrypted BLE file transfer, explicit Wi-Fi/LAN provisioning/file transfer, multi-group/rotation and reset-partial preservation, and ten-second timing/error/drop checks under radio transfers. Longer endurance and power-cut testing require separate evidence. CoreS3's schema and bench results remain distinct.

The final deployed logger includes the bounded startup archive scan, fixed-PIN pairing-state callback and complete-JSON MTU guard. A fresh 16-row reset file passed both readers with zero drop/error/missed counters; an earlier finalized file remained byte-identical after normal firmware resets. The host clock was explicitly supplied after that reset test, and the device was left recording. Image identities and limits are in the bench record; secure first pairing and Wi-Fi/LAN testing remain pending owner action.
