# CoreS3 native ESP-IDF logger

The current source is a pure **ESP-IDF 6.1** application, firmware identity
**`idf-cores3-parquet-v6.27`**, with an **81-column** CoreS3 schema
`cores3-telemetry-v6` and dictionary `cores3-telemetry-v7`. The original 77
columns retain their order and types. Four nullable location fields follow,
`h3_cell_id`, `h3_resolution`, `location_source` and the WKB Point `geometry`.
Since v6.12 the two UTC instants are `event_time_utc_us` and
`clock_anchor_utc_us`, INT64 microseconds annotated `TIMESTAMP(MICROS, UTC)`,
and every leaf carries Parquet `field_id` = its 1-based dictionary position
(see [mobile migration](../../docs/shared/mobile-migration.md)). Each `FIELD`
row ends with its LIVE key and its LIVE float decimals, so the dictionary
SHA-256 `3b4d8500…` covers the LIVE key map and precision. `bringup/main.cpp` owns board setup, sensors, display and native
UART; `bringup/telemetry_logger.cpp` supplies the OSLogger adapter. M5Unified
**0.2.26** and M5GFX **0.2.33** are native IDF components; there are no Arduino
APIs or Arduino component in the application. Location uses the optional shared
component with official H3 **4.5.0**. Shared connectivity uses native
NimBLE/Wi-Fi/netif/SNTP and managed mDNS **1.14.0**. These stable release pins
were checked on **2026-10-16**; LZ4 remains vendored **1.10.0**. The
[development guide](../../docs/shared/development.md#host-and-toolchain) records
the latest-release pin policy and qualification scope.

**The newest dated native readback is the 2026-10-26 reflash of main 689bb61**,
with the Thing id, null location columns and the `geo` footer key. See the
[dated image and limits](../../docs/boards/m5stack-cores3/bench-verified.md#main-689bb61-usb-reflash-thing-id-and-first-geo-footer-2026-10-26).
The first native v6.9 run on 2026-10-09 is in the
[same record](../../docs/boards/m5stack-cores3/bench-verified.md#board-1-first-native-esp-idf-flash-and-readback).
Phone transfers, sustained display/card arbitration and power-cut durability
remain unqualified. Earlier measurements below retain their Arduino identities.

Native build/host gates passed on **2026-10-09**; see the
[shared validation table](../../docs/shared/development.md#native-migration-validation-2026-10-09).

## Current build and flash workflow

```sh
pixi install
pixi run idf-setup
pixi run cores3-build
pixi run telemetry-contract-test --sanitize
```

The shared SDK source/tools pin is in `../../tools/idf-dependencies.lock`;
managed board dependencies are in
[`components/os_cores3/idf_component.yml`](components/os_cores3/idf_component.yml). Board defaults
select **16 MB QIO flash**, **8 MB Quad PSRAM**, the **USB Serial/JTAG console**
and the existing custom partition offsets. Build wrappers verify the dictionary,
vendor hashes and generated target configuration. Ephemeral output is in
`build/bringup/`; retain image/ELF copies, hashes and captures in ignored
`artifacts/` before a physical run.

Before any write, complete the [exact-board identification and full-backup sequence](../../AGENTS.md#before-any-physical-flash-write).
Confirm the matching full backup is **16,777,216 bytes**, then use:

```sh
pixi run cores3-flash <checked-port> backup/m5stack-cores3-flash-<timestamp>.bin
```

This uses pinned Pixi esptool and the validated `flash_args` to flash the
verified build without an implicit rebuild or NVS erase. Keep the `parquet`
namespace with its `thing` key and the unread old `station` key, and keep every
key, value type and partition offset. Since v6.16
(#93) settings live in the `oscfg` namespace and nothing is copied from the old
one. It starts empty after that reflash, so the pairing mode and PIN, the
display flag, Wi-Fi on, the SSID and password, the `lan.on` switch (back to its
default of on), the LAN sync token, the location and the NTP settings must be
provisioned again, as on a new board. CoreS3 installs no sensor policy, so it
stores no sensor identity to lose. Bonds survive the reflash and the phone may
cache the old GATT table, so forget the board in the phone's Bluetooth settings
and send `ble.clear_bonds=1` before pairing again. The old settings and crash
log namespaces stay in flash with the old Wi-Fi password and LAN token, and
only a full flash erase removes them. Do not erase Thing identity,
credentials or owner data to make pairing pass. Native USB full-flash readback
uses esptool's default baud. 921600 corrupted an earlier measured read. Never
write eFuses or format SD.

## Native board ownership and qualification

CoreS3 M5Unified/M5GFX own the display and existing SPI2 bus. The native SDSPI
adapter attaches the SD device to that bus instead of creating another SPI
controller owner; application locking and completed display DMA retain the
shared-bus ordering. The board owns power sequencing, PMSA003 UART GPIO18/17,
IMU/light/touch acquisition and BM8563 RTC callbacks. OSLogger owns the
10-second monotonic deadline and the sole archive worker after startup.
Missing measurements and unknown UTC remain null; finalized Parquet remains
immutable and interrupted `.partial` files remain retained.

The SD adapter reserves one **4 KiB internal DMA buffer before radios start**
and lends it to ESP-IDF through `sdmmc_host_t::dma_aligned_buffer`. It stays
allocated for the mount, avoiding per-transfer DMA allocations when FatFs
buffers live in PSRAM. Eight-sector chunks retain bounded multi-block reads
and writes. Failed mounts release the reservation. This follows the pinned
[ESP-IDF 6.1 SD transfer guidance](https://docs.espressif.com/projects/esp-idf/en/v6.1/esp32s3/api-reference/storage/sdmmc.html#unaligned-buffer-performance);
the automatic allocation flag applies to non-SPI initialization, so this
SDSPI adapter supplies its own buffer.

Shared connectivity now prefers PSRAM for Wi-Fi/lwIP packet storage, with
**eight static RX and eight static TX DMA buffers**, **32 software RX buffers**
and **16 cached TX buffers**. Hardware buffers reserve approximately **25.6 KiB**;
software packets and TCP allocations use PSRAM first, with internal fallback.
The TCP send/receive limits remain **5,760 bytes per socket**, for three LAN
clients, plus SDK metadata. Startup and finalized-file logs report DMA free,
largest-block and minimum-free values. These bounds follow the SDK's
[PSRAM Wi-Fi guidance](https://docs.espressif.com/projects/esp-idf/en/v6.1/esp32s3/api-guides/wifi-driver/wifi-performance-and-power-save.html#using-psram);
actual peak memory, radio coexistence and transfer rates require board evidence.

Location starts unset. Owner provisioning supplies a cell and optional
declared ISO country code. The resolution is fixed at **8** (#139), and only the
resolution 8 parent of the cell is persisted. Its centre supplies the
`geometry` WKB Point, alongside `h3_cell_id`, `h3_resolution` (always 8) and
`location_source`. Each queued sample owns its location
snapshot, and location/country changes finalize the old file before writing
new-location rows. Existing files retain their original schema and metadata.
See [time and location policy](../../docs/shared/time-and-location.md).

Wi-Fi time synchronization prefers router DHCP option 42 and configured local
servers, with public fallback enabled by default. CoreS3 can still restore its
offline RTC anchor or accept authenticated host time; no successful anchor means
null UTC. Hardware qualification must cover router/local/public transitions and
location changes as well as RTC behavior.

Qualify USB startup with no connected reader, display/card arbitration, optional
sensor and RTC behavior, secure BLE pairing/long reads, mDNS/LAN transfers and
byte-identical finalized-file readback on the native image. Record date, image
hash, readback and limits in the [board bench record](../../docs/boards/m5stack-cores3/bench-verified.md).
The existing host readback commands and phone wire protocol remain applicable:

```sh
pixi run parquet-device command --port <checked-port> 'parquet status'
pixi run parquet-device sync-time --port <checked-port>
pixi run parquet-device command --port <checked-port> 'parquet list'
```

## Historical material below

All following Arduino build commands, APIs, dependency tables, footprints and
hardware observations describe earlier images. Their old build commands are
retained for interpreting evidence and are not the current workflow. Use the
native tasks above for the current source.

## Historical Arduino implementation and bench record

**Historical status: Real-sensor Parquet SD logging, full 60/90-row uncompressed files, UTC Hive checks and identical-row LZ4 compression comparisons were verified on hardware on 2026-09-08. Bluetooth LE file sync (`arduino-cores3-parquet-v4`) was flashed and verified from a host BLE client on 2026-09-16; protocol v2 with Wi-Fi/LAN sync (`-v5`) on 2026-09-17; file schema v3 with multi-row-group files, statistics and TIMESTAMP annotations (`-v6`, then the `-v6.1` lifetime fix) flashed and read back the same day; the BM8563 RTC hand-off (`-v6.2`) and the protocol 2.1 advertising payload that wakes the phone (`-v6.3`) both on 2026-09-18.**

## Concurrent LAN collection (2026-10-08)

`arduino-cores3-parquet-v6.6` supports three authenticated Wi-Fi clients plus
one BLE connection, with independent archive handles and no connection takeover.
Each phone still pairs over BLE once to obtain LAN access. This image was flashed
and its hash verified on the backed-up CoreS3. Two host collectors downloaded
identical verified files alongside a third existing connection; the logger
reported zero drops/errors. See the dated [bench record](../../docs/boards/m5stack-cores3/bench-verified.md#board-1-concurrent-lan-collectors-firmware-v66)
for image identity, measured rate and phone/Waveshare qualification limits.

## Earlier bring-up and phone integration (through 2026-09-18)

### Flashed 2026-09-18 11:02Z: `arduino-cores3-parquet-v6.3`, advertising payload (protocol 2.1)

The device now tells a phone that is *not* connected that something changed: ten bytes of service data under the service UUID in the ADV PDU (`ver`, flags `no_utc`/`new_files`/`sd`/`lan`/`fail`/`clk_restored`, `fin` u32, `boot16`), refreshed in place with one HCI command when a field changes; the local name and the 128-bit UUID list moved to the scan response because the ADV is now exactly 31 bytes. `new_files` is set at `PARQUET READY` and cleared after a `LIST` on any link. Contract: [advertising payload](../../docs/shared/ble-sync-protocol.md#advertising-payload-v21); reasoning and the Android side: [background-sync-triggers](../../docs/shared/background-sync-triggers.md). Host gates: `arduino-build` (1,417,339 program bytes / 86,868 static RAM), `fmt-check`, `lint`, contract and `ble_sync` tests; `pixi run ble-sync scan` decodes the payload. **Flashed and bench-tested the same afternoon** (image SHA-256 `74d4a172…`, retained at `artifacts/firmware/cores3-parquet-v6.3-74d4a172.bin`): `BLE ADV flags=0x24 fin=0` at boot, the phone still found and synced the device with the UUID list in the scan response, and — with the app process killed — a `flush` on the board woke the phone through its controller's offloaded filter and the new file was on the phone 28 s later (`BLE ADV flags=0x2c fin=1` after the LIST). Logs in `artifacts/adv-v6.3/`. Record: [bench](../../docs/boards/m5stack-cores3/bench-verified.md#board-1-advertising-payload-wakes-the-phone-firmware-v63-protocol-21). Companion-device presence and the Wi-Fi-arrival run are still unmeasured (the association dialog needs the owner's tap).

`arduino-cores3-parquet-v5` — protocol **v2** ([contract](../../docs/shared/ble-sync-protocol.md)): device configuration, pairing modes, Wi-Fi provisioning, LAN sync server. Was on the board until 12:41 on 2026-09-17 (superseded by `-v6` below, which keeps all of it), binary SHA-256 `6144ea1d…`, retained at `artifacts/firmware/cores3-parquet-v5-6144ea1d.bin` (previous: `d5f1db40…`, same directory). 1,408,487 program bytes / 78,628 static RAM. The board is provisioned to the owner's home Wi-Fi (credentials live only in the board's NVS). `pixi run fmt-check`, `pixi run lint` and `pixi run python tools/test_ble_sync.py` pass. Bench detail: [bench record](../../docs/boards/m5stack-cores3/bench-verified.md#board-1-protocol-v2-configuration-wi-fi-lan-sync-phone).

| Piece | State |
| --- | --- |
| `../common/runtime/src/device_config.{h,cpp}` — NVS `oscfg` settings, `key=value` parser, CONFIG JSON, LAN token, first-boot pairing default (display → `random`, none → random per-device `fixed`; legacy 123456 rotates) | done; `GET_CONFIG`, `SET_CONFIG` validation (`code=12`) and `GET_TOKEN` **verified over BLE** |
| `../common/connectivity/src/ble_sync.{h,cpp}` — v2 opcodes/frames, `Link` tag on requests, `random`/`fixed`/`none` pairing, `clear_bonds()`, **every BLE frame ≤ 512 bytes** (Android drops larger notifications), 4 s notify retry budget; since `-v6.3` the 2.1 advertising payload (`publish_advert`, name/UUID list in the scan response) | done; only `random` (the CoreS3 default) has run on hardware; the 512 cap fixed the phone's deterministic BLE gap; the advertisement woke the phone's dead app process once, measured |
| `../common/runtime/src/debug_log.{h,cpp}`, the `oslog` serial tee with a persistent PSRAM ring sized by `CONFIG_OS_LOG_PERSIST_RING_BYTES` (64 KiB default, 4 KiB internal fallback); every `Serial.print*` in the firmware goes through it | done; `LOG_TAIL` **verified over BLE** |
| `../common/connectivity/src/wifi_link.{h,cpp}` — LAN task: STA state machine, `WIFI_SCAN`, mDNS `_ossync._tcp`, TCP :47390 with `OSS1`+token handshake, STATUS/LIVE pushes, send failure drops the session, **token takeover** (a second connection with the valid token replaces a half-open session) | **verified on hardware** from the Mac (provisioning, rejoin after reflash/reboot, mDNS, handshake, auth refusal, LIST, 6- and 40-file syncs at 19–62 KB/s, REBOOT, takeover/wrong-token/silent challenger) and from the phone (all 776 files synced over LAN across three runs, 45–63 files/min); BLE+Wi-Fi jitter ≤ 17.7 ms, heap 82 KB free — see the bench record |
| `bringup/telemetry_logger.cpp` — link-aware dispatch (`respond`/`respond_error`/`link_payload_max`), new ops incl. `REBOOT` (finalizes the batch first), worker heartbeat (stamped per frame) + stall report, worker stack 24 KiB | done; `REBOOT` verified over LAN; `ble.clear_bonds`, `lan.rotate_token` not yet exercised |
| `bringup/bringup.ino` — page 4 shows pairing mode / default-PIN warning / Wi-Fi / LAN, header `WiFi ..|up|ok`, `start_links(display_detected)` | done, seen on the LCD |
| `tools/ble_sync.py` — `config`, `set`, `wifi-scan`, `token --save`, `log`, `reboot`; `--lan HOST[:PORT] --token-file` runs every command over TCP | done; scripted protocol tests and the retained CoreS3 LAN runs are recorded in the [bench record](../../docs/boards/m5stack-cores3/bench-verified.md) |
| Docs | contract v2, bench record (BLE, Wi-Fi, LAN, phone), lessons, `cores3-wireless.md` (Wi-Fi in use), `cores3-development.md` (footprint, wedged-task recipe, `oslog`), router rows — done |
| Android v2 (`../../../opensensor-space-android`) | commits `f16420e`…`2f4c994` (88 unit tests, lint clean) tested on a **OnePlus 7 Pro / Android 16**: BLE reconnect, automatic token fetch, LAN selection via NsdManager, `dataSync` foreground service with progress notification (survives screen-off), reconnect + resume, window-loss recovery, **Auto-sync** (WorkManager, LAN first, bonded BLE fallback, interactive-session guard proven in logcat), Reboot only via the Device-screen dialog. `2f4c994` (WifiLock, resume after transport failure, transport chip) is installed but not yet exercised. Commit `678c1c0` (2026-09-18, 151 tests, APK `3c454c74…` installed 14:03) adds the event-driven triggers — companion-device presence, the `-v6.3` advertising-flag scan, Wi-Fi arrival; the advertising wake is measured (28 s file → phone, app closed), the other two are not |

Next: the owner taps "Set up wake when nearby" on the phone (companion association) and we measure the presence event and whether the screen-off socket kill goes away; then one uninterrupted full-card run for a clean timing number, a scheduled auto-sync with the app closed and the three triggers ([background-sync-triggers](../../docs/shared/background-sync-triggers.md), "Measure before believing"), `ble.clear_bonds` / `lan.rotate_token` from the app, `fixed`/`none` pairing on a display-less board.

### Flashed 2026-09-18 07:00Z: `arduino-cores3-parquet-v6.2`, RTC hand-off

Motivation: the BM8563 at `0x51` had never been written, so every "RTC read failed" since 2026-09-08 was an unset chip, and every reboot filled `unsynced/` until a phone or the host sent `SET_TIME` (see [hardware](../../docs/boards/m5stack-cores3/cores3-hardware.md), "BM8563 RTC"). v6.2 keeps the clock contract (anchor = host UTC seconds paired with a monotonic instant; no fabricated UTC; old rows never rewritten) and adds a second, weaker source for that anchor:

| Change | Effect |
| --- | --- |
| `set_clock()` gains a source; every host sync (serial, BLE, LAN) asks the **main task** to copy the anchor into the RTC in UTC at the next whole-second boundary (`service_rtc_write`, ≤ 60 ms late, forced after 5 s, read back) | serial `PARQUET RTC write=ok utc=…Z late_ms=…`; the storage worker never touches I²C |
| `begin_logger()` seeds the anchor from the RTC before the first sample when `getVoltLow()` is false and the calendar decodes to 2020–2100 (`seed_clock_from_rtc`) | rows carry `clock_status = 2`, `clock_epoch = 1`, and go to the dated Hive tree from the first sample; `PARQUET TIME … source=rtc` on serial; otherwise `PARQUET RTC state=unusable reason=…` and the old `unsynced/` behaviour |
| A host sync on a device that already has an anchor logs the error of the clock it replaces: `PARQUET CLOCK transport=serial\|ble\|lan previous=rtc\|host skew_ms=…` | the only clock-accuracy evidence the device produces; record it in the bench file |
| `status` JSON adds `clk` (anchor source 0/1/2) and `rtc` (chip state 0 unread / 1 in use / 2 unusable); footer `clock` metadata documents code 2; `kFirmware` → `-v6.2` | the phone should offer "set time" when `clk != 1`, not only when `utc == 0` ([protocol](../../docs/shared/ble-sync-protocol.md)) |
| Device-health page shows `Clock: 2026-09-18 09:42:35Z host e1` (UTC, source, epoch) or `Clock: no UTC yet (RTC unset\|unusable)` | the device never renders local time; time zones are the reader's job |

Host-verified: `pixi run telemetry-contract-test --sanitize` (fixture asserts code 2 and the no-anchor case), `pixi run parquet-test --sanitize`, `fmt-check`, `lint`, `arduino-build` (1,415,975 program bytes / 86,852 static RAM). Schema stays v3 and the dictionary SHA-256 is unchanged (`telemetry_fields.inc` untouched; only the meaning table of the `clock_status` code column grew). **Flashed and bench-tested the same morning**: image SHA-256 `ad72b96f…`, retained at `artifacts/firmware/cores3-parquet-v6.2-ad72b96f.bin`. First boot showed the unset chip (`state=unusable reason=voltage-low`), the first `sync-time` wrote it (`write=ok … late_ms=0`), a LAN `REBOOT` came back dated (`source=rtc` 1.77 s after reset) and produced `data_0700_1d3590e1…_0-2-0.parquet` in the dated tree with `clock_status=2` and the first non-null `rtc_*` columns on this board; a second host sync measured `skew_ms=-513` (resolution-bound). Retained copy in `artifacts/rtc-v6.2/`. Full power-off retention and multi-hour drift remain unmeasured — see the [bench record](../../docs/boards/m5stack-cores3/bench-verified.md#board-1-rtc-hand-off-host-anchor--bm8563--next-boot-firmware-v62).

### Flashed 2026-09-17 12:41 local: `arduino-cores3-parquet-v6`, file schema v3

Motivated by the Android history evaluation (`../../../opensensor-space-android/docs/history-ux-and-data-layer.md` §0: one row group per device file, `null_count` only, no `TIMESTAMP` annotation, 7 KB footer per ~88 rows). Host-verified (`pixi run parquet-test --sanitize`, `pixi run telemetry-contract-test --sanitize`, `fmt-check`, `lint`, `arduino-build`: 1,412,899 program bytes / 86,828 static RAM), then **flashed and bench-tested at 1800 s**: binary SHA-256 `409987b4…`, retained at `artifacts/firmware/cores3-parquet-v6-409987b4.bin`; two two-row-group device files (105 rows cut by the window boundary, then a full 2 × 90 = 180-row half hour, 87,654 bytes, footer 15.6 %) passed PyArrow, DuckDB and the footer decoder, and the phone's auto-sync pulled one of them over LAN unchanged — see the [bench record](../../docs/boards/m5stack-cores3/bench-verified.md#board-1-schema-v3-row-groups-statistics-timestamp-firmware-v6). That first LZ4 multi-group run then **failed**: v6 kept a pointer to a stack-local `Compression` across row groups, so the first compressed group failed and the worker dropped rows for two hours (see the bench record for the full account and the data lost). `arduino-cores3-parquet-v6.1` (SHA-256 `1dadcad9…`, `artifacts/firmware/cores3-parquet-v6.1-1dadcad9.bin`) copies the struct by value; the host fixture now clobbers the caller's copy after `begin()` and ASan fails the v6 writer on it. Flashed 13:00Z; the 13:00–13:30Z window then produced a clean two-group **LZ4** file (170 rows, 38,129 bytes, 224 B/row, footer 36 % of the file) that passed all three readers. Running at **1800 s / LZ4_RAW**; the four-group 3600 s file and a hard reset between groups are still unmeasured.

| Change | What a reader sees |
| --- | --- |
| `parquet_writer.{h,cpp}` — streaming `Writer` (`begin` → `row_group` … → `finish`), up to `kMaxRowGroups = 8` row groups per file; `write_parquet()` remains the one-group wrapper | several row groups per file, each with `file_offset`, `total_compressed_size`, `ordinal` and `sorting_columns` (rows ascend by `sequence`) |
| Column statistics: `null_count`, `min_value`/`max_value` with `is_*_exact`, `nan_count` on FLOAT, one `TYPE_ORDER` per leaf in `column_orders`; NaN excluded, `-0.0`/`+0.0` normalised, no bounds when all values are NaN or null; deprecated `min`/`max` deliberately omitted (parquet-format 2.13/2.14 writer rules, checked 2026-09-17) | PyArrow/DuckDB can prune by `event_time_utc_us`, `monotonic_us`, `sequence`, … without reading pages; the phone's file index comes from the footer |
| v6 to v6.11: `event_time_utc_ns`, `clock_anchor_utc_ns` (unit `ns_since_unix_epoch`) carry `TIMESTAMP(NANOS, isAdjustedToUTC=true)`; INT64 bytes unchanged. Since v6.12 they are `*_utc_us` with `TIMESTAMP(MICROS, UTC)` | v6 to v6.11 files read as PyArrow `timestamp[ns, tz=UTC]` and DuckDB `TIMESTAMP WITH TIME ZONE` (**microseconds**, DuckDB 1.5.5 checks the UTC flag before the NANOS unit. Firmware values are µs-derived so nothing real is lost, use `epoch_ns()` for the integer). v6.12 files read as PyArrow `timestamp[us, tz=UTC]` and DuckDB `TIMESTAMP WITH TIME ZONE`, use `epoch_us()` for the integer |
| `schema_version` → `cores3-telemetry-v3` (row column `3`); `dictionary_version` stays `cores3-telemetry-v2` because `telemetry_fields.inc` and its SHA-256 are unchanged; `created_by` = `m5stack-aq-parquet version 0.2 (build arduino-cores3-parquet-v6)`; new metadata `row_groups`, `row_group_rows_max` | v2 and v3 files have identical column names/types and can be unioned; only the annotation, statistics and row-group count differ |
| Logger: a file is created as `.partial` when the first RAM batch of a window completes and grown by **one row group per completed RAM batch** (≤ 90 rows, `fsync` after each); the footer is written at the window boundary, at 8 groups, or on `flush`/codec/interval/`REBOOT`. `parquet interval` accepts **600, 900, 1800, 3600** (→ 1, 1, 2, 4 groups of ≤ 90 rows per file). `.partial` names are `<prefix>_<boot>_<first>-<attempt>.partial`; finalised names are unchanged | default 900 s behaves as before (one group per file). Status gains `open_rows`/`open_groups` (serial) and `open`/`open_rg` (JSON): rows already on the card in the open file, footer pending. `PARQUET GROUP` lines precede each `PARQUET READY` |
| `codec-test` writes its benchmark copies through a second `OutputFile` with its own workspace, so it can never disturb the open telemetry file. That file and its staging buffer are allocated from PSRAM only while the command runs, so they are not part of `psram_workspace_bytes` | since #5 under `/sd/bench/`, outside the synced archive |

Measured on the host fixture (77 columns × 90 rows, incompressible synthetic values): footer 4,484 → 6,000 bytes for one group (+1.5 KB of statistics and orders); 2 groups 10.4 KB / 70 KB file, 4 groups 19.3 KB / 139 KB. The per-column-chunk metadata (~57 B × 77, mostly the mandatory `path_in_schema` name) dominates, so **more row groups per file only take the footer share from 17 % to ~14 %**; the byte win stays with LZ4 and the phone's daily merge. What hourly files (3600 s rotation, the longest allowed, default 900 s) buy is 4× fewer files to list/sync/allocate and 15-minute statistics inside them, with the RAM loss window unchanged at one batch. Power loss mid-file leaves a footer-less `.partial` whose row groups are complete on the card; it is retained, and a host repair tool (rebuild the footer from the page headers and the known schema) is not written yet.

To extend the evidence: `parquet interval 3600` (optionally `parquet codec lz4`), sync time, wait for four `PARQUET GROUP` lines and one `READY`, `parquet-device fetch` the file and run `pixi run python tools/inspect_parquet.py <file>` (row groups, footer facts, key statistics, both readers); record sizes, `writer_us`, `sync_us` and heap in the bench record. `pixi run chip` while a file has one group on the card simulates the hard-reset case (it resets the running application) and should leave a retained `.partial` reported at the next boot.

**Flashed and bench-tested, 2026-09-16:** `arduino-cores3-parquet-v4` adds a NimBLE GATT server implementing [protocol v1](../../docs/shared/ble-sync-protocol.md): device info, live/status JSON, UTC time set, finalized-file manifest and windowed file reads, all executed by the existing single storage worker. Build 842,743 program bytes / 39,372 static RAM bytes; binary SHA-256 `d1e5151fb3d3a33cb0f4dd65c51d7359df77cf40f740df2fb54173bc2289cc4b`, retained at `artifacts/firmware/cores3-parquet-v4-d1e5151f.bin`. Passkey pairing, 766-file listing, six verified file transfers (14.7–21.6 KB/s), `SET_TIME` and `FLUSH` were measured from macOS; see the [bench record](../../docs/boards/m5stack-cores3/bench-verified.md#board-1-bluetooth-le-sync). The USB-serial command protocol is unchanged. No phone has been tested against the board yet.

At the time of this historical run, this was the only framework trial. It establishes the CoreS3 hardware baseline and tests whether a bounded C++ writer can produce interoperable Parquet from real ten-second measurements on microSD. It owns its toolchain bootstrap, exact dependency versions, board options, partition layout and build outputs. Networking and Iceberg are deferred.

**Flashed and smoke-tested, 2026-09-08:** `arduino-cores3-parquet-v3` introduces 77-column schema `cores3-telemetry-v2`, a versioned dictionary/provenance contract and four timing fields. The original 73 fields retain their names/types/order. Host sanitizer/reader tests and the pinned build pass. A three-row unsynchronized UNCOMPRESSED file and four-row UTC LZ4 file passed both readers and metadata/timing checks; [new bench evidence](../../docs/boards/m5stack-cores3/bench-verified.md#board-1-schema-v2-provenance-and-timing) is separate from earlier full-window/codec comparisons. The build uses 589,887 program bytes / 26,996 static RAM bytes; binary SHA-256 `a239223f55aae798ea3eb0839723ebb96e00e5961808eba1aa522f7d09538b1c`. Flash hashes verified; station and saved files retained. The board was left at 900 seconds with LZ4 and restored host UTC, with no reported drops/errors.

See [the observation model and Mermaid workflows](../../docs/shared/table-and-observation-model.md) for the source dictionary, units, validity rules, unknown deployment/calibration policy and static-Iceberg decision. `pixi run telemetry-contract-test --sanitize` tests the shared row contract; after flashing this revision, `pixi run parquet-device command --port <checked-port> 'parquet schema'` reads the dictionary without flushing or changing settings.

## Historical Arduino platform

| Dependency | Exact version |
| --- | --- |
| Arduino CLI | 1.5.1 |
| Arduino-ESP32 | 3.3.11, based on ESP-IDF 5.5.5 |
| M5Unified | 0.2.21 |
| M5GFX | 0.2.28 |
| NimBLE-Arduino | 2.5.1 (BLE host for the sync service) |
| Vendored LZ4, BSD-2-Clause | 1.10.0, hashes in `dependencies.lock` |
| Board FQBN | `esp32:esp32:m5stack_cores3` |

The historical dependency lock in Git history recorded these versions. The old FQBN selected 16 MB flash, QIO flash, **QSPI/Quad PSRAM**, hardware USB CDC and 115200 upload speed. `build.sh` checks the generated SDK configuration and fails if it finds Octal PSRAM, the wrong flash size or a non-custom partition table.

The custom 16 MB partition table has two 6 MB OTA application slots, NVS, OTA state, a core-dump partition and a remaining SPIFFS partition. Measurement data belongs on microSD; the flash filesystem is not the primary sample store.

## Commands

Run all commands from the repository root:

```sh
pixi install
pixi run arduino-setup
pixi run arduino-boards
pixi run arduino-build
pixi run arduino-flash /dev/cu.usbmodem101
pixi run capture --port /dev/cu.usbmodem101 --seconds 20 --until "DIAG COMPLETE"
```

The port is an example. Identify the connected board with `pixi run ports` and repeat the safety sequence in the root `AGENTS.md` before a first write to another board. Setup keeps downloaded SDKs and libraries below `$M5_TOOLCHAIN_ROOT`, which defaults outside the repository. The completed Arduino cache measured 7.5 GB on 2026-09-08; Arduino CLI adds 34 MB.

A physical RST press makes this CoreS3 disappear and re-enumerate on macOS. The capture helper reconnects to the same device path during its bounded timeout. A short press boots the application; holding RST for about three seconds enters the silent ROM downloader.

## Bring-up and live sensor firmware

`bringup/bringup.ino` emits a machine-readable `cores3-bringup-v1` report. It uses M5Unified as the sole owner of internal board services and checks:

1. chip, revision, MAC, reset cause, flash size, mode and speed;
2. internal heap and detected PSRAM;
3. documented internal I2C devices;
4. PMIC, VBUS and battery reporting through `M5.Power`;
5. RTC, IMU and touch;
6. microSD mount on the display-shared SPI bus, without formatting;
7. PMSA003 framing, length and checksum on GPIO18 RX and GPIO17 TX.

After bring-up, the UART parser continues draining the active PMS stream. Every 10 seconds the LCD and serial output refresh from the latest checksum-valid frame. The LCD has four pages: atmospheric and CF=1 PM1.0/PM2.5/PM10 mass values; all six particle-count thresholds with sensor and parser integrity; live SD, power, heap, PSRAM and uptime health; and Bluetooth. Swipe vertically or tap to move between pages. Page changes redraw immediately without changing the measurement cadence. While the board needs setup (a fixed Thing with no `location.cell`, advert flags bit 6), the footer of every page reads `Setup needed, open the OpenSensor app` in place of the swipe hint. It clears on the next redraw after a `SET_CONFIG` sets a cell or switches the Thing to moving.

The scan excludes reserved I2C addresses `0x00` through `0x07`. Probing that range stopped the ESP32-S3 controller during the first hardware run; M5Unified's own scanner carries the same restriction.

## Verified result

The first diagnostic confirmed 16 MB QIO flash, **8 MB Quad PSRAM**, all nine expected onboard I2C devices, working IMU and touch communication, a mounted nominal 32 GB SDHC card and a checksum-valid PMSA003 frame. The RTC acknowledged at `0x51` but its date/time read failed. PMIC values indicated USB input and near-zero battery voltage, so battery presence and charging remain unverified.

The full values and test method are recorded in [the bench record](../../docs/boards/m5stack-cores3/bench-verified.md). The initial verified sketch used 551,263 bytes of program storage and 26,188 bytes of static internal RAM. The single-page live-display revision used 552,795 bytes and 26,268 bytes; the pre-Parquet three-page touch revision used 554,315 bytes and 26,276 bytes. That image was flashed with hash verification, produced a fresh 10-second sensor report with zero parser failures and logged page changes from touch gestures. The pre-codec Hive logger used 579,711 bytes of program storage and 26,988 bytes of static internal RAM; the LZ4/legacy-readback revision uses 589,011 bytes and 26,988 bytes respectively. The physical layout and gesture feel still need a deliberate visual check by someone looking at the board.

## On-device Parquet feasibility

`telemetry_logger.cpp` takes one snapshot every **10 seconds**, using a monotonic deadline rather than adding processing time to the interval. Default rotation is **900 seconds / up to 90 rows**; **600 seconds / up to 60 rows**, **1800 seconds / 2 × 90 rows** and **3600 seconds / 4 × 90 rows** (firmware v6) are selectable over serial. The RAM batch is at most 90 rows in every mode; longer windows add row groups to the open file rather than rows to RAM. With UTC supplied, files follow aligned UTC windows (`:00/:15/:30/:45` by default); the initial or manually flushed file can be shorter. A sample is a snapshot, not a ten-second average. Missed deadlines advance the sequence and increment a counter instead of manufacturing historical readings.

The 73-column hardware baseline includes device/boot identity and sequence; monotonic time, schedule, jitter and clock epoch; both PMS mass-concentration variants and all six particle-count channels; framing/status/age; acceleration and angular velocity; auxiliary magnetometer raw counts; IMU die temperature; LTR-553 raw light channels and proximity; power reports; RTC calendar fields; touch; memory and storage health. Current source appends collection completion, PMS frame receipt and the two clock-anchor values for 77 columns. Names carry numeric units; `telemetry_fields.inc` is the schema source of truth. The initial flat-path bench image had 72 columns, before adding the clock epoch.

PMS measurements are null during the initial 30-second warm-up, after five seconds without a valid frame, or when the sensor reports an error. Status and parser counters remain available. Magnetic values are uncalibrated raw counts, light/proximity are raw ADC counts rather than lux/distance, and IMU temperature is **not ambient temperature**. RTC reads fail until the chip has been written once (v6.2 does so at every host sync). UTC stays null until the explicit `sync-time` command supplies the host's current clock or, from v6.2, until a boot restores an earlier sync from the RTC; subsequent rows carry `clock_status=1` (host estimate) or `2` (RTC-restored), while old rows remain unchanged. Each time update increments `clock_epoch` and separates batches. Host time is not a calibrated synchronization/uncertainty guarantee. Ambient temperature/humidity remain null because SHT20's electrical isolation is unresolved. CoreS3 battery current is unsupported by M5Unified and stays null; voltage/percentage/charging are PMIC reports, not proof of battery presence. Camera and audio streams are not acquired in this scalar telemetry trial.

Acquisition and M5 board-service reads run in the Arduino loop. An eight-record queue feeds a separate FreeRTOS storage task; a 90-row batch, column descriptors, format workspace and LZ4 state/page buffers reside in PSRAM (the exact allocation is printed at startup). This allows the scheduler to use both cores without explicit task pinning. One application mutex serializes SD and display access, including completion of display DMA. Sensor code never writes SD. The storage task has a 16 KiB stack and 4 KiB internal stdio staging; the format workspace is now 2,816 bytes, with separate bounded codec scratch and no per-page dynamic allocation. The earlier uncompressed image's allocation was 63,560 bytes; do not reuse that as the codec image's memory figure.

The writer supports flat nullable/required INT32, INT64 and FLOAT (INT64 optionally annotated `TIMESTAMP(MICROS, UTC)`, every leaf with a required positive `field_id`), PLAIN encoding, RLE definition levels, Data Page V1 and **UNCOMPRESSED / LZ4_RAW** pages. Each row group is one page per column written sequentially; the Thrift Compact footer carries per-chunk statistics (`null_count`, `min_value`/`max_value`, exact flags, `nan_count` for FLOAT), `column_orders`, row-group offsets/sizes/ordinals and `sorting_columns`. Up to eight row groups share a file (v6 source). It is a deliberately limited format writer, not Apache Arrow or a general Parquet implementation. LZ4_RAW is the default. `parquet codec none|lz4` switches the codec and saves the choice in NVS, so it survives a reboot. See [codec design, host results and tests](../../docs/boards/m5stack-cores3/compression-benchmark.md). Snappy/Zstd remain host-only candidates.

Files use **`/output/` on the card** (`/sd/output` in firmware). A station UUID is generated once and saved in the `parquet` NVS namespace; normal resets/flashes preserve it. Date partitions use UTC, never local time or the unreadable RTC:

```text
output/
└── thing=<persistent-UUID>/
    └── year=2026/month=09/day=08/
        ├── data_0900_<boot>_<first>-<last>-<attempt>.parquet
        └── data_0915_<boot>_<first>-<last>-<attempt>.parquet
```

The suffix prevents restarts, clock corrections and forced flushes from overwriting a file in the same window. Before UTC is supplied, files instead use `thing=<UUID>/unsynced/boot=<boot>/data_unsynced_...parquet`. Existing dated and unsynced files are never relocated when the clock changes. The legacy first-run files under `/parquet/` remain on the card and are now exposed by list/get as `legacy-parquet/<name>`. Since #5 benchmark duplicates live under `/sd/bench/boot=<boot>/`, outside the listed and synced archive. Older cards keep any earlier `/output/benchmarks/` copies.

The worker creates files exclusively, writes `.partial`, retries positive short writes, checks zero/error writes, calls `fflush` and `fsync`, closes, verifies structural completion and reads CRC32, then renames to `.parquet`. Files are retained; no automatic deletion or upload exists. On write failure the batch is retained in RAM and the SD space governor retries with a remount on a 10 s to 10 min backoff; `parquet flush` retries at once. A full card pauses logging with `PARQUET ALARM storage=full` and resumes when space returns. Interrupted partial files are conservatively moved out of active station trees into `/output/quarantine/<station>/<partition>/`, at boot and right after a same-boot failure; bounded status keys `qf`/`qb` report total quarantined files/bytes across boots. No repair, deletion or overwrite is attempted.

**Durability limit:** unfinished RAM samples are lost on reset, potentially the full configured batch. FAT rename and SD controller persistence have no power-loss guarantee here. The structural completion check is not a full Parquet validator; host readers perform that check. A recovery spool, power-cut tests, full-card/removal handling and sustained radio/display stress remain later work.

### Inspect the live logger

Only one host process may own the port at a time. Identify its current path first.

```sh
pixi run parquet-test --sanitize
pixi run parquet-device sync-time --port /dev/cu.usbmodem101
pixi run parquet-device command --port /dev/cu.usbmodem101 'parquet status'
pixi run parquet-device command --port /dev/cu.usbmodem101 'parquet list'
pixi run parquet-device command --port /dev/cu.usbmodem101 'parquet flush'
pixi run parquet-device command --port /dev/cu.usbmodem101 'parquet interval 900'
pixi run parquet-device command --port /dev/cu.usbmodem101 'parquet interval 600'
pixi run parquet-device command --port /dev/cu.usbmodem101 'parquet interval 3600'   # v6: four 90-row row groups per file
pixi run parquet-device capture --port /dev/cu.usbmodem101 --seconds 650 --until-ready --out firmware/esp-idf-cores3/artifacts/parquet-capture.log
pixi run parquet-device fetch --port /dev/cu.usbmodem101 '<reported-relative-path>.parquet' --out firmware/esp-idf-cores3/artifacts/output
pixi run python tools/inspect_parquet.py firmware/esp-idf-cores3/artifacts/output/<fetched-file>.parquet   # row groups, footer facts, statistics, both readers
pixi run parquet-device bench --port /dev/cu.usbmodem101 --sync-time --seconds 960 --until-ready --out firmware/esp-idf-cores3/artifacts/output --log firmware/esp-idf-cores3/artifacts/hive-bench.log
pixi run python tools/export_parquet.py --port /dev/cu.usbmodem101 --out firmware/esp-idf-cores3/artifacts/exports/new-snapshot
pixi run python tools/export_parquet.py --port /dev/cu.usbmodem101 --out exports/sd-$(date -u +%Y%m%dT%H%M%SZ)   # full-card keep-safe copy at the repo root (git-ignored)
```

`export_parquet.py` is idempotent and resumable: each file is requested up to `--retries` times (default 3) because a `PARQUET ROW` log line can, rarely, corrupt one hex transfer line, and files already published in `--out` are re-validated instead of re-downloaded, so an interrupted export is finished by re-running the same command. The manifest records `live-device-readback` versus `resumed-local-verified` per file. The 2026-09-16 full-card copy (764 files, 66,987 rows) lives in root `exports/`, which is git-ignored on purpose: it is the owner's data, not repository evidence.

Changing the interval first flushes any current rows; the setting lasts until reboot, when it returns to 900 seconds. UTC must be supplied again after reboot until a persistent trusted clock or network time source is implemented. `fetch` reads the **already-written SD file** in bounded chunks over USB, verifies length/CRC32, compares every stored value/null through PyArrow and DuckDB, and preserves Hive directories. There is no host-side format conversion. Keep evidence in git-ignored **`artifacts/`, never `build/`**: Arduino can clean its build directory. `bench` uses one serial connection; use a fresh log path, or omit `--log`. The export-all helper snapshots every listed finalized file, including the legacy prefix, without resetting/flushing/deleting; RAM-only pending rows are excluded.

`--until-ready` stops at the first finalized file, which can be a short initial window or old unsynced batch split by `--sync-time`; it does not assert 90 rows. Check the reported row count and time range before claiming full-window coverage. For a quick current-schema smoke test, use a fresh log name:

```sh
pixi run parquet-device bench --port /dev/cu.usbmodem101 --sync-time --seconds 35 --flush-after-capture --out firmware/esp-idf-cores3/artifacts/output --log firmware/esp-idf-cores3/artifacts/hive-smoke.log
```

The smoke command changes the clock anchor and manually finalizes a short batch; it is not an endurance test. `flush` does not stop sampling or unmount SD, so it is not a safe-eject command. `status`, `list` and `fetch` need no device reset. The host fetcher rejects unsafe relative paths and differing existing destinations, while accepting an identical already-verified file; it preserves both device files and local Hive structure.

For querying dated files, use DuckDB's Hive discovery explicitly; the `thing`, `year`, `month` and `day` fields come from the path:

```sql
SELECT thing, year, month, day, sequence, pm25_atmospheric_ug_m3
FROM read_parquet('output/thing=*/year=*/month=*/day=*/*.parquet',
                  hive_partitioning=true);
```

The host validator disables Hive inference when comparing stored file columns; query-time partition discovery is a separate check.

On this macOS/CoreS3 pair, opening serial with both DTR and RTS deasserted caused an unwanted USB reset. The Parquet helper keeps both asserted and clears POSIX HUPCL; separate status, flush and fetch operations preserved the boot and buffered sequence during the bench test. Other adapters/hosts need their own check.

The earlier 72-column image produced a full automatic **60-row, 28,059-byte** batch that matched in PyArrow 25.0.0 and DuckDB 1.5.5. Finalization took **122,041 µs**, including 13,317 µs in flush/sync/close; no drops, missed deadlines or storage errors were recorded. The 73-column pre-codec Hive image also produced a full **90-row, 39,869-byte** file covering the 19:00 UTC window, with zero recorded health errors and both readers matching. Its earlier short files established partition discovery, boundary splitting and normal-restart retention. These are image-specific observations, not latency bounds or power-cut qualification. See [bench results](../../docs/boards/m5stack-cores3/bench-verified.md) for artifacts/hashes and [compression tests](../../docs/boards/m5stack-cores3/compression-benchmark.md) for the later codec image.

### Compression controls

```sh
pixi run parquet-device command --port /dev/cu.usbmodem101 'parquet codec lz4'
pixi run parquet-device command --port /dev/cu.usbmodem101 'parquet codec none'
pixi run parquet-device command --port /dev/cu.usbmodem101 'parquet codec-test'
pixi run python tools/benchmark_device_compression.py --port /dev/cu.usbmodem101 --out firmware/esp-idf-cores3/artifacts/compression-new --min-rows 60 --seconds 700 --repeats 3
```

Codec changes finalize pending normal rows under the previous codec. `codec-test` instead writes both codecs from the same buffered rows, labels copies as diagnostic duplicates under `/sd/bench/` (`output/benchmarks/` before #5), and retains the normal batch. Do not mix those copies into telemetry queries. The helper validates both readers and compares paired stored values/nulls; its timings separate codec CPU, writer/sink work and full finalization. No automatic recompression or silent fallback occurs.

On this board/card, three paired 60-row comparisons measured **50.7% fewer complete-file bytes** and **21.4% lower median finalization time** with LZ4, using 6,208 bytes of explicit codec workspace. Both readers preserved all values/nulls; subsequent normal acquisition and a compressed Hive smoke file also passed. This establishes feasibility and a measured advantage over this writer's uncompressed mode, not a guarantee that every SD write is faster or that files occupy proportionally fewer FAT clusters. At the time of the bench the board was left using LZ4 at 900-second rotation, and a reboot returned it to uncompressed. Since #25 LZ4_RAW is the default and the choice is saved in NVS.

Offline acquisition does not require internet, but uninterrupted power and functioning SD remain necessary. The measured compressed rate projects to roughly 0.74 GB/year at ten-minute rotation before filesystem overhead; do not treat that as battery or card life. The full capacity assumptions and still-unimplemented object-storage uploader are in [offline capacity and reconnection](../../docs/shared/telemetry-pipeline.md#offline-capacity-and-reconnection).

The contract and later cloud design remain in [the telemetry pipeline](../../docs/shared/telemetry-pipeline.md). A second framework trial requires a concrete limitation from this one, recorded here first.

## Bluetooth LE sync

The board advertises as `OS-xxxx` (last four hex digits of the Wi-Fi MAC-derived `device_id`) with the 128-bit service UUID from [the contract](../../docs/shared/ble-sync-protocol.md). Everything is encrypted and authenticated: the first connection from a new phone or laptop shows a random six-digit passkey full-screen on the LCD (also printed as `BLE PAIR passkey=…` on serial, deliberately, for bench logs). Only a person who can read the screen can pair. The link is one connection at a time; advertising resumes on disconnect. `kEnableBle` in `bringup.ino` switches the radio off at compile time.

Implementation notes: `Command` now carries a source tag and, for BLE, a ≤ 512-byte raw request, so the struct grew to about 1 KB and the queue depth went 4 → 6 (≈ 6 KB heap). `list_directory` takes an emitter callback so LIST and the serial `PARQUET FILE` lines share one walk; `set_clock()` is shared by `parquet time` and `SET_TIME`. Worker state the phone needs (`failed`, storage readiness, retained partials, missed deadlines) is mirrored into atomics so the `status` document can be built from any task. `kFirmware` is `arduino-cores3-parquet-v4`; the dictionary and schema are unchanged.

Ownership does not change: NimBLE callbacks only queue a request on the same command queue the serial parser feeds (`Command::Source::Ble`); the storage worker executes it and emits `response` notifications itself; `live`/`status` are published by the sampling loop right after the row is queued. Page 4 of the LCD shows the device name, link state and bond count. Serial lines added: `BLE BEGIN|CONNECT|MTU|PAIR|BONDED|DISCONNECT|CMD|OPEN|READ|ERROR|LIST SKIP`.

Host client for bench and recovery without a phone (bleak, PyPI dependency in `pixi.toml`):

```sh
pixi run ble-sync scan
pixi run ble-sync --name OS-6b40 info
pixi run ble-sync --name OS-6b40 live --seconds 35
pixi run ble-sync --name OS-6b40 list
pixi run ble-sync --name OS-6b40 sync --out firmware/esp-idf-cores3/artifacts/ble-<date>
pixi run ble-sync --name OS-6b40 time    # explicit UTC write, same rules as `parquet time`
pixi run ble-sync --name OS-6b40 flush   # explicit, finalizes the RAM batch
```

On macOS run these from Terminal.app: CoreBluetooth aborts clients launched from a process without a Bluetooth usage entitlement (measured). `sync` writes the same verified layout and `manifest.json` as `tools/export_parquet.py`, so BLE and USB exports are byte-comparable. `python tools/test_ble_sync.py` checks the host frame codec offline.

## Recovery

The original 16 MB UIFlow image is preserved under `backup/`. `pixi run restore --board m5stack-cores3 --port <checked-port> backup/<matching-file>.bin` writes it back and is destructive, so name the image explicitly. Never write eFuses or raise the esptool baud on this board.

**Last verified on hardware: 2026-09-28, board MAC ending `6b:40` (`arduino-cores3-parquet-v6.5`: shared sampling/storage worker, RTC-restored boot, sensor rows and identical USB/BLE/LAN file readback). Earlier full-window and phone-wake results remain tied to their recorded images.**

## Shared module extraction (v6.4 hardware verification)

Source firmware **v6.4** consumes [OSCommon, OSRuntime and OSConnectivity](../common/README.md). BLE, Wi-Fi/LAN, configuration, logs, immutable file sessions and common control handlers live outside this trial. `telemetry_logger.cpp` supplies its worker queue, archive policy and snapshots; `ltr553.h` is a thin M5 internal-I²C adapter. Its 77-column dictionary, provenance, RTC, sampling, display arbitration and rotation/quarantine policy remain board-owned. The image was flashed and verified with a four-row sensor file, RTC restoration, configuration/log handlers, 1,079-file listing and identical CRC/SHA-256/reader results over BLE and LAN. The [dated bench record](../../docs/boards/m5stack-cores3/bench-verified.md#board-1-shared-esp32-s3-modules-firmware-v64) includes the initial Wi-Fi retry and intermittent macOS name discovery; earlier endurance/phone-wake numbers remain tied to their retained images.

## Shared sampling and archive engine (v6.5)

`telemetry_logger.cpp` is now the CoreS3 acquisition/RTC/storage adapter for [OSLogger](../common/logger/README.md). The common library owns deadlines, the queue, worker, file rotation, immutable finalization, clock epochs and sync dispatch. This trial retains M5 initialization, display/SD bus hooks, LTR553 acquisition and the 77-field dictionary. The `/sd/parquet` legacy listing route, station NVS namespace, configuration and board provenance are preserved. A seven-row file passed both readers and matched across USB, encrypted BLE and authenticated LAN; earlier v6.4 tests above remain separate evidence. See the [v6.5 bench record](../../docs/boards/m5stack-cores3/bench-verified.md#board-1-shared-logger-engine-firmware-v65).

The final v6.5 image also replaces the automatic startup filename dump with a bounded archive summary. On the measured card, unattended startup reached 17 buffered samples at 209 seconds with zero drop/error/missed counters; explicit listing still returned 1,089 entries and both legacy files. A final 30-row file passed USB/LAN readback. The earlier failed no-reader boot and its 12 visible drops are retained separately in the bench record, alongside the final image hashes.
