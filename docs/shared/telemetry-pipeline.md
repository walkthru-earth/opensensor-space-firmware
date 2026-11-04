# Sensor telemetry, Parquet and cloud pipeline


> **Board scope:** OSCommon/OSLogger implement the shared pipeline for both native
> ESP-IDF 6.1 board adapters. CoreS3 uses the 81-column `cores3-telemetry-v6`
> schema and Waveshare V2 the 62-column PMS5003T `waveshare-sim7670g-telemetry-v6`
> schema. Firmware and dictionary versions are in the [status table](../../README.md#status). Schema identities and hardware
> acquisition remain board-owned. Dated SD/throughput results below are historical
> Arduino image evidence. A [short native CoreS3 readback](../boards/m5stack-cores3/bench-verified.md#board-1-first-native-esp-idf-flash-and-readback)
> passed; Waveshare, native endurance and durability remain pending.

[Router](../README.md) · Read for the measurement record, bounded buffer, on-device Parquet, SD durability and later object-storage upload. Hardware-level SD rules remain in [CoreS3 storage](../boards/m5stack-cores3/cores3-storage.md). Native migration snapshot **2026-10-09**; earlier firmware-v6 hardware evidence was recorded on **2026-09-17**; measured runs belong in [bench-verified](../boards/m5stack-cores3/bench-verified.md), separately from planned validation.

## Current decision

The native ESP-IDF applications implement **real-sensor Parquet on microSD** through the shared C++ writer and storage worker. Native builds and host gates pass, plus a short CoreS3 USB readback; earlier hardware readbacks retain their historical image scope. It acquires one row every **10 seconds**, containing available scalar measurements and explicit validity/status fields. Rotation defaults to **900 seconds** and can be set to **600, 1800 or 3600 seconds** for the running session; reboot restores 900 seconds. The logger buffers up to 90 rows in RAM; a completed batch becomes one **row group** of the open file, and the file is finalized at the window boundary (1, 1, 2 or 4 row groups for the four intervals):

```text
10-second rows -> bounded PSRAM batch -> Parquet writer -> finalized SD files
                                                               |
                                                               v
                                                later idempotent HTTPS upload
                                                               |
                                                               v
                                                    object storage / queries
```

At this cadence, complete 10- and 15-minute windows contain 60 and 90 rows respectively when no samples are missed. The implementation uses a PSRAM row batch, an eight-row producer queue and a small C++ Parquet writer with no Arrow runtime. Sampling continues into that queue while the storage worker finalizes the batch. Boot, explicit flush, interval/location changes and genuine clock discontinuities can produce shorter files. File rotation follows the UTC boundary rather than a nominal row-group count, so a small clock refresh can leave an extra row/group in the same file.

File rotation and reset durability are separate choices. The implemented baseline loses its unfinished RAM batch after reset. It moves `.partial` files into an explicit quarantine directory on startup without attempting repair or deletion. A small SD recovery spool remains a later option if the required loss window is shorter than one rotation. Finalized-file survival still requires power-cut tests before a production durability claim. A real 60-row automatic batch and shorter current-schema Hive files have been read back from SD and opened with both PyArrow and DuckDB. UTC partition agreement, a quarter-hour split and Thing id and file retention across a normal restart were checked separately; a later complete 90-row uncompressed Hive file also passed both readers and UTC-window checks. See [bench-verified](../boards/m5stack-cores3/bench-verified.md) for exact firmware, schema and run details.

The motivating [ESP32-S3 Rust trial](https://github.com/walkthru-earth/esp32s3-parquet-test/tree/1f3a6c706f85d54a0abe3105b2eed4c6378d814d) uses the Apache Rust Parquet crate with default features/Arrow disabled and `snap` enabled; its lockfile resolves **56.2.0**. The audited source generates 178 synthetic rows and ten columns, builds a complete in-memory file including its footer, and uploads the bytes. It does not implement SD persistence, continuous real-sensor acquisition or power-loss recovery, and the reviewed checkout contains no independently verifiable device-memory captures or generated-file artifacts. Its documented Zstd comparison is a macOS experiment. This is credible implementation evidence for investigating device-side Parquet, not a CoreS3 benchmark. [Dependencies](https://github.com/walkthru-earth/esp32s3-parquet-test/blob/1f3a6c706f85d54a0abe3105b2eed4c6378d814d/Cargo.toml), [writer](https://github.com/walkthru-earth/esp32s3-parquet-test/blob/1f3a6c706f85d54a0abe3105b2eed4c6378d814d/src/main.rs#L266), [host comparison](https://github.com/walkthru-earth/esp32s3-parquet-test/blob/1f3a6c706f85d54a0abe3105b2eed4c6378d814d/index.md#L591).

Object-storage synchronization follows local file validation. Apache Iceberg is explicitly deferred; plain Parquet files can be uploaded and queried without a table catalog.

The [table and observation model](table-and-observation-model.md) records the static-Iceberg assessment and SensorThings V2 draft findings. Keep table publication on the host/cloud; adopt explicit measurement/provenance semantics in firmware without claiming OGC API compliance. Its first implementation phase is distinct from the 73-column hardware evidence below.

The tested conclusion is now affirmative: this CoreS3 can create interoperable Parquet directly from real ten-second rows on SD, with a full 90-row uncompressed window verified. In three identical-60-row comparisons, LZ4_RAW reduced whole-file bytes by 50.7% and median finalization time by 21.4% versus the same writer uncompressed. That is evidence for this design/workload, not a benchmark against CBOR/JSON, an energy result, a claim about allocated FAT space, or proof of production durability. [Measurements and limits](../boards/m5stack-cores3/compression-benchmark.md#hardware-result-lz4-passed)

The active board applications now use pure [ESP-IDF 6.1](https://docs.espressif.com/projects/esp-idf/en/v6.1/esp32s3/get-started/index.html),
with native M5 components on CoreS3 and native peripheral drivers on Waveshare.
The migration preserves dictionaries, NVS identity/config types, immutable
archives and the phone wire contract. Use the [current development workflow](development.md)
and qualify each native image on hardware before extending historical bench
claims. Common components remain reusable and board-neutral; peripheral pins,
controller ownership and mount policy stay in each adapter.

## Measurement contract

Every sample has an identity independent of wall-clock quality. Never turn a timeout, bad checksum or stale value into a zero measurement.

| Field | Requirement |
| --- | --- |
| `schema_version` | INT32 schema version of the board that wrote the file. The current schema name, dictionary version and column count are not repeated here, because they move with every dictionary bump. Read `kSchemaName`, `kDictionaryVersion` and the `static_assert` on `field_count` in `firmware/esp-idf-cores3/bringup/telemetry_contract.h` and `firmware/esp-idf-waveshare-sim7670g/logger/telemetry_contract.h`. CoreS3 v6.12 wrote `cores3-telemetry-v4`, and the next version kept the same columns and added LIVE keys to the dictionary bytes. Every leaf carries Parquet `field_id` = its 1-based dictionary position. Native v6.9 to v6.11 wrote version 4 with the same leaves under the old `*_utc_ns` names. Native v6.8 retained historical schema v3/77 columns. Firmware v3–v5 wrote version 2 with the same 77 columns; earlier bench images used version 1 with 72/73 columns. Check the actual schema and image identity. |
| `thing_id` | UUID generated once and persisted in NVS (`parquet` namespace, `thing` key, owner decision 31); carried in the footer `thing.id` and the Hive directory `thing=<uuid>`, not repeated as a numeric row column. Erasing NVS creates a new Thing identity. The first boot of the Thing release mints a new id, the old `station` key stays unread. |
| `device_id` | INT64 containing the board's 48-bit Wi-Fi station MAC; a hardware identifier, not an anonymized UUID. |
| `boot_id_hi`, `boot_id_lo` | Two INT64 fields carrying a random 128-bit identifier generated once per boot. |
| `sequence` | Monotonic INT64 counter within one boot; device, boot and sequence identify the row. Missed sample deadlines create observable sequence gaps. |
| `monotonic_us` | Acquisition time from the monotonic clock; always present. |
| `event_time_utc_us` | Nullable INT64 UTC estimate in microseconds, with units explicit in the name. Since CoreS3 v6.12 / Waveshare v2.6 it and `clock_anchor_utc_us` carry `TIMESTAMP(MICROS, isAdjustedToUTC=true)` so Apache Iceberg reads them as `timestamptz` in every format version, including v3. PyArrow reads `timestamp[us, tz=UTC]`, DuckDB reads `TIMESTAMP WITH TIME ZONE` (`epoch_us()` recovers the integer). Older files name them `*_utc_ns` with `TIMESTAMP(NANOS, UTC)`. Absent until an external host/SNTP anchor arrives or a usable RTC is restored. |
| `clock_status`, `clock_epoch` | Status 0 = unsynchronized, 1 = host estimate supplied on this boot, 2 = restored at boot from the BM8563 RTC (firmware v6.2, source 2026-09-18). Status 3 = a successful network SNTP response. Code 2 is the last external estimate the RTC was written with, at whole-second resolution plus RTC drift since that sync; it is dated (Hive tree, not `unsynced`) but a reader that wants host-fresh time should filter on `clock_status IN (1, 3)` or on `clock_epoch` after the sync. Since CoreS3 v6.10 / Waveshare v2.4, the epoch increments for initial UTC (RTC seed included), an RTC-to-external upgrade, or a correction strictly larger than two seconds; small host/network refreshes preserve the epoch. Files split when the epoch or UTC window changes. Per-row anchors and source retain each accepted estimate; old rows are never relabeled. Older firmware incremented at every anchor. No measured clock-uncertainty bound or synchronized status is claimed. |
| Measurements | Fixed-width typed fields with units in the schema, not encoded into display strings. |
| Validity | Per-sensor status and validity bits; preserve missing, warming, stale, checksum error and device error distinctly. |
| Provenance | File metadata records firmware, board, Thing, boot, cadence, status meanings and unavailable measurements. New source adds dictionary version/SHA-256 and acquisition configuration; deployment/calibration identifiers remain explicitly unknown. Owner-declared sensor/H3 provisioning is implemented; external deployment history and calibration remain future work. |
| Additional v2 timing | `collection_completed_mono_us`, `pms_received_mono_us`, `clock_anchor_mono_us`, `clock_anchor_utc_us`; see [semantics and validity](table-and-observation-model.md#timing-and-validity-dictionary). |

Use `pixi run parquet-device sync-time --port <port>` to explicitly supply this host's current UTC estimate, or `--sync-time` with `parquet-device bench`. The serial `parquet time <epoch-seconds>` command anchors that value to the device monotonic clock and reports the anchor. This is not NTP; command/transport latency and host error are not measured. The common Android/iOS session refreshes unset/RTC clocks, external anchors older than six hours, or current estimates more than two seconds from the phone, with microsecond precision when supported. Wi-Fi SNTP also establishes UTC without a phone. Accepted anchors affect future rows only and do not flush the current interval. TIME_SET supplies immediate reconciliation evidence. Since #88 the footer holds no anchor. Every row written after an accepted anchor carries it in `clock_anchor_mono_us` and `clock_anchor_utc_us`, which are evidence for that row's device, Thing and boot only. Routine sync lists completed files; explicit FLUSH closes the unfinished window when current stored readings are requested. No synthetic measurement is created. A separate uncertainty-bearing anchor journal is not implemented.

The anchor uses the command's monotonic receipt time, not the later time at which the storage worker handles it. This avoids adding worker-queue delay to the clock mapping, but host integer-second truncation and USB latency remain. Thing UUID persists across normal resets; the runtime interval choice does not. Source: [current logger](../../firmware/esp-idf-cores3/bringup/telemetry_logger.cpp).

**RTC hand-off (firmware v6.2).** Every host or network sync is also copied into the onboard BM8563 RTC in UTC: the main task (the only I2C user after startup) waits for the anchor's next whole-second boundary (≤ 60 ms late on the 20 ms loop, forced after 5 s) and logs `PARQUET RTC write=ok utc=… late_ms=…` after a read-back. At the next boot, `begin_logger` reads the RTC before the first sample; if the chip reports no voltage-low event and the calendar decodes to 2020–2100, it becomes anchor epoch 1 with `clock_status = 2` (`PARQUET TIME … source=rtc`), so a power-cycled logger dates its rows from the first sample instead of filling `unsynced/`. Otherwise it logs `PARQUET RTC state=unusable reason=voltage-low|invalid-calendar|out-of-range` and behaves as before. A later host sync upgrades an RTC epoch or starts a new epoch for a correction larger than two seconds, and logs the measured error of the clock it replaced (`PARQUET CLOCK transport=… previous=rtc|host skew_ms=…`, new host time minus the device's previous estimate of the same monotonic instant) — this is the only clock-accuracy evidence the device produces; collect it in `bench-verified.md` before trusting RTC-restored time to better than seconds. The RTC never holds local time. Whether the CoreS3 RTC keeps running with the main battery removed is **not verified**; see [hardware](../boards/m5stack-cores3/cores3-hardware.md).

**Time zones.** Nothing on the device knows a time zone: rows, Hive paths, file names, the `status`/`live` documents and the Device-health screen are UTC (the screen shows `Clock: 2026-09-18 09:42:35Z host e1`, source and epoch included). Local time is a reader's job — DuckDB `AT TIME ZONE`, PyArrow `tz` conversion, the phone's `ZoneId.systemDefault()` — and any "today" grouping done locally must say which day boundary it used.

Schema versions: **v1** 73 columns; **v2** 77 columns (`cores3-telemetry-v2`, firmware v3–v5); **v3** (`arduino-cores3-parquet-v6`, flashed 2026-09-17) keeps the 77 v2 leaves and their dictionary (`dictionary_version` stays `cores3-telemetry-v2`, same `telemetry_fields.inc` SHA-256) but changes what a reader sees: TIMESTAMP annotations on the two UTC fields, min/max statistics on every column chunk, several row groups per file, `created_by` with the firmware build, and `row_groups`/`row_group_rows_max` metadata. v2 and v3 files union cleanly on names/types. Both v2 and v3 have **77 numeric columns**, appending four timing fields to the earlier 73-column codec image. PMS fields include atmospheric and CF=1 PM1.0/PM2.5/PM10, six cumulative particle-count channels, sensor error and framing/checksum counters. Onboard fields cover IMU readings, raw magnetic counts, raw LTR-553 light/proximity counts, power, RTC calendar, touch and memory/storage health. Unsupported battery current and ambient temperature/humidity remain null; camera frames and microphone audio are outside this scalar schema. Availability and conflicts are documented in [hardware](../boards/m5stack-cores3/cores3-hardware.md) and the relevant accessory references. Carry source age/status when a row snapshots a sensor whose acquisition cadence differs from 10 seconds; these snapshots are not interval averages. The new contract passes host tests and short real SD readbacks; see [schema-v2 measured scope](../boards/m5stack-cores3/bench-verified.md#board-1-schema-v2-provenance-and-timing).

## Deployment cell and country (native schemas)

Both boards hold four nullable location leaves next to each other. From the
Thing release (#83, CoreS3 schema `cores3-telemetry-v6`, Waveshare schema
`waveshare-sim7670g-telemetry-v6`) they are

| Field | Physical type | Meaning |
| --- | --- | --- |
| `h3_cell_id` | INT64 | Official H3 published cell ID, safely within signed range, hex in config |
| `h3_resolution` | INT32 | Actual published cell resolution, potentially coarser than the configured maximum |
| `location_source` | INT32 with `INTEGER(8,false)` (UINT8) | Where the row location came from. 0 none, 1 configured, 2 gnss, 3 phone. Classes live in `telemetry_classes.inc` |
| `geometry` | BYTE_ARRAY with the Parquet `GEOMETRY` logical type, no crs (OGC:CRS84) | 2D WKB Point, little endian, 21 bytes, at the H3 cell centre |

All four are null while the deployment cell is unset, and `geometry` is null
exactly when `h3_cell_id` is null. Since the Thing release (#83) the centre is
no longer two integer columns (decision 32). Each `geometry` chunk carries
Parquet `GeospatialStatistics` (bbox and geometry type) and no min or max. A
second footer key, `geo`, holds compact GeoParquet 2.0 JSON naming `geometry`
as the primary column, with a bbox only when the file has a location. Firmware
derives the centre from the published cell, no raw GPS is stored. The
`opensensor` footer key carries no location (#88). Country is not written to
the file and is not inferred from H3. A fixed Thing writes
`location_source` 1. A moving Thing with a GNSS hook writes 2 with a fresh fix
and four nulls without one ([moving Things](time-and-location.md#moving-things-and-gnss)).
No phone path writes 3 yet.

Acquisition snapshots cell, maximum precision and country into each queued row.
The worker splits the RAM batch and open file before appending a different
snapshot, including country-only/cap-only changes, so each file holds one
location snapshot. Old finalized files retain their original content and
identity. Changing location does not rewrite earlier samples or archives.
The mobile app requests an OS location fix only on an explicit action, computes
H3 locally at selected precision, previews its derived center and sends only
the cell through authenticated SET_CONFIG. See [time and location](time-and-location.md).

## Parquet file lifecycle and optional recovery spool

The storage worker creates a uniquely named `.partial` file exclusively when the first RAM batch of a window completes (`<prefix>_<boot>_<first>-<attempt>.partial`). Each completed RAM batch (≤ 90 rows) is written as one row group — one PLAIN page per column plus the chunk's statistics kept in PSRAM — followed by `fflush` and `fsync`, so those rows are on the card before the next batch starts. At the window boundary, after `kMaxRowGroups` (8) groups, or on an explicit flush/codec/interval/reboot command, the worker appends the footer, checks write counts, calls `fflush` and `fsync`, and closes. It then reads the file back to check length, leading/trailing `PAR1` and footer bounds, and computes a whole-file CRC32 before renaming to `<prefix>_<boot>_<first>-<last>-<attempt>.parquet`. With the default 900-second window a file has exactly one row group, as before; 1800/3600 s give two/four. A reset between two row groups leaves a footer-less `.partial` whose earlier groups are complete on the card; at boot, or immediately after a same-boot write failure, it is moved under `/output/quarantine/<thing>/<partition>/` with a boot and counter suffix and reported. A `.partial` whose finalized sibling already exists is left untouched and logged as `crosslink-suspect` (`qf`/`qb` in status count all quarantined files/bytes, including prior boots; `part` remains the retained-partial scan count). `pixi run rebuild-partial` rebuilds a footer host side from the page headers and the board `telemetry_fields.inc`, keeps every complete row group, drops the torn tail and writes the result to a separate directory, never back to the card. Rows still in the RAM batch at the cut were never written and cannot be rebuilt. This structural check is not a full Parquet decoder: PyArrow and DuckDB perform host conformance validation. Only finalized files are eligible for later upload; never append rows to one. Quarantine preserves bytes while preventing interrupted files from being confused with an active writer or publishable data; firmware never repairs or deletes them. FAT rename is not assumed crash-atomic, and automatic formatting is disabled.

The RAM-only baseline makes the unfinished batch expendable and reports that limitation. A recovery spool is an optional subsequent durability feature. If selected, version 1 should use the record and lifecycle below; keep it distinct from the primary Parquet export format.

Serialize the spool manually with explicit byte order. It does not write a native compiler struct or rely on padding.

Each record has:

1. a synchronization marker and format version;
2. header and payload lengths;
3. record type and validity flags;
4. identity, sequence and time fields;
5. fixed-schema payload bytes;
6. standard CRC32 over the versioned header and payload.

Wrap the ESP-IDF CRC API and test it against host-generated known vectors. Its seed, continuation and final inversion rules are easy to apply incorrectly.

The final spool trailer records first and last sequence, record count, byte count and SHA-256 of the committed content. CRC catches a damaged record during streaming recovery; a separate SHA-256 over the finalized Parquet file gives the later uploader a stable content identity.

Use this lifecycle:

1. create a uniquely named `.open` file;
2. append complete records through one storage worker;
3. batch writes in 4–16 KiB, keeping the DMA staging buffer in internal RAM;
4. at a durability boundary, check the write count, call `fflush`, then `fsync`;
5. rotate on the configured Parquet batch boundary or a bounded size;
6. write and sync the trailer, close, validate the file, then rename it `.ready`;
7. build and validate the corresponding immutable Parquet file locally;
8. retire the spool only after that Parquet file is durable under the tested recovery policy.

FAT rename is not assumed to be crash-atomic. At boot, scan both suffixes, validate complete records, recover the longest valid prefix of an `.open` file, and quarantine anything ambiguous. Never autoformat after a mount or recovery failure. Keep upload acknowledgements in a separate small journal so a reset cannot confuse “sent” with “durably accepted.”

An optional spool can remain uncompressed. Measure complete Parquet bytes, CPU time and peak working memory before choosing its page codec; compression is independent of whether a spool is present.

## Memory, tasks and backpressure

- Sensor code parses into typed records and never writes the filesystem directly.
- The 90-row batch and writer workspace reside in PSRAM, Quad on CoreS3 and OPI on Waveshare. The sample queue defaults to eight rows through `ossync::ServiceConfig` and is allocated by FreeRTOS. Allocation failure disables logging with an error. Queue depth is not a qualified stall budget.
- One storage task owns FatFs and all SD transactions. CoreS3 shares SPI2 with the display through the application bus lock. Waveshare owns one-bit SDMMC. Neither waits for network work while holding the bus lock.
- The writer streams through a 4 KiB stdio buffer on the storage task's internal stack. Each archive read handle has a separate 4 KiB PSRAM stdio buffer. Both boards reserve a heap-owned 4 KiB internal DMA SD transfer buffer before radios and retain it for the mount. The shared mount helper and descriptor budget cover writers, transient streams and four persistent archive readers.
- The uploader reads only finalized dated Parquet files. Its software landed in #129, off by default and not wired into any board, see [uploader software](#uploader-software-129). No retention deletion exists.
- Queue overflow counts dropped rows. Since #5 an SD space governor (`firmware/common/logger/src/storage_governor.h`) classifies the card as ready, degraded, full or retry. The hard reserve is 32 MiB, the high mark is 5 percent of the volume capped at 256 MiB, and 16 MiB of hysteresis stops flapping (all scaled down on tiny cards). Below the reserve the worker finalizes the open file, logs `PARQUET ALARM storage=full`, sets STATUS `fail` and `sd_state`, and stops normal card writes. Full/retry retain a bounded RAM batch and count new rows as dropped when it fills or a partition/window change cannot be committed. A write, close, rename or mount failure enters retry, which remounts the card (when the board supplies the hook) and probes after 10, 30, 60, 120, 240 and 480 s, then every 600 s. The RAM batch is kept until a probe writes it. Full is re-measured every 60 s with a remount, so space freed on a PC is seen without a reboot. No file is ever deleted automatically. There is no gap-record journal.

Record sample jitter, queue high-water mark, drops, internal and PSRAM minimum free space, writer time, file bytes/row, SD write p50/p99/max latency, sync latency, incomplete-file count and, once implemented, recovery and upload retry counts. Label measurements by firmware build, configuration, sample count and card model; keep measured results in [bench-verified](../boards/m5stack-cores3/bench-verified.md).

## Upload and cloud layout

> **Implemented transport (2026-09-16):** finalized files can be pulled over Bluetooth LE by a paired phone or laptop, whole-file and verified — see the [BLE sync protocol](ble-sync-protocol.md). It is a pull of immutable files, not the acknowledged upload described below; nothing is deleted or marked on the card. Offline-first ordering: device → local archive (phone/laptop/hub) first, then optionally archive → cloud when the owner turns that on; a device or phone with Wi-Fi or a SIM may shortcut, but must not skip the local copy.

The SD layout already uses the requested Hive partition directories, ready to preserve as object keys when upload is added. UTC-dated files use:

```text
output/thing=<UUID>/year=YYYY/month=MM/day=DD/data_HHMM_<boot>_<first>-<last>-<attempt>.parquet
```

Only these dated `data_HHMM_` files are meant for publication. Unsynced files,
quarantine and bench copies stay in the local archive (stac-opensensor
PROFILE at 83787768cc8451109d48a03d4ef5493ffcd403dd).

`HHMM` identifies the start of the UTC-aligned 10-, 15-, 30- or 60-minute window, not the upload time. The boot, sequence-range and attempt suffix prevents distinct files in the same window from overwriting each other after reboot, manual flush or a clock correction. The extension remains `.parquet`; the interval is configuration, not an extension suffix. New windows, midnight and clock-epoch changes split batches. Startup mid-window produces a short first file. Firmware paths include the `/sd` mount prefix; the card/object path starts at `output/`.

The worker detects window/epoch changes on arrival of the next sample; there is no independent wall-clock finalization alarm. Since CoreS3 v6.10 / Waveshare v2.4, accepted external anchors do not independently finalize files: a genuine epoch/window transition closes the old file on the next sample, while the first newly timed sample starts the normal interval. Since #5, unanchored rows rotate on the same configured window measured on this boot's monotonic clock, so a day without UTC yields about 96 files at 900 s instead of about 1,440 one-minute files. A `READY` line proves that one file finalized, not that a full configured window was collected. In particular, `bench --sync-time --until-ready` may retrieve the old unsynced or shortened batch closed by the new anchor. Check row count, sequence range and timestamps before labeling a run a full 10/15-minute test.

Before a real time anchor exists, rows have null UTC and files go to `output/thing=<UUID>/unsynced/boot=<boot>/data_unsynced_<boot>_<first>-<last>-<attempt>.parquet`. The `monotonic_us` column preserves microsecond resolution. Raw files are never assigned a guessed calendar date or retroactively renamed. The mobile archive owns reconciliation: using an acknowledged or persisted same-boot anchor, derive UTC microseconds as `clock_anchor_utc_us + (monotonic_us - clock_anchor_mono_us)`, then rebuild per-key UTC day partitions atomically from immutable raw files. Row anchors in a later file of the same boot also let a later download reconcile after the source device has rebooted. Reconciliation must use matching device, Thing and boot identities; a powered-off interval without RTC or an anchor from that boot cannot be reconstructed. Such files remain unresolved. No internet or server is required once the phone has the files and valid anchor evidence.

Unanchored rows now follow the normal row-group cadence, so an abrupt power cut can lose up to one RAM batch (at most 90 rows, 15 minutes at the 900 s default) before UTC exists, the same bound as dated rows. Abrupt power loss can still lose the current RAM batch or leave an incomplete `.partial`, which remains quarantined rather than silently repaired. Finalized-file durability under arbitrary power cuts is not yet bench-qualified.

For the later HTTPS uploader, create-only semantics such as `If-None-Match: *`
make retries idempotent. After a timeout, query or retry the same finalized key.
Never invent a new identity for the same bytes. Stream bounded reads from SD,
releasing the shared-bus lock before network waits. The ingestion endpoint
should verify length, SHA-256, readable Parquet pages, sequence range and schema
version before acknowledging. An acknowledgement never authorizes automatic
archive deletion. Owner archives remain immutable and owner-controlled.
SHA-256 and acknowledgement persistence are not implemented by the current
CRC32 serial-readback helper.

### Uploader software (#129)

The shared uploader in `firmware/common/upload/` landed in #129. It is off by
default through `CONFIG_OS_UPLOAD_ENABLE`, and no board wires a session into
the storage worker yet. So no board uploads today. What follows is the current
code in `firmware/common/upload/src/`, mainly `os_upload_policy`,
`os_upload_session` and `os_upload_ledger`.

- It sends finalized dated `data_HHMM_` files only, oldest first, one at a time,
  and only after UTC comes from a host or the network.
- Each put sends `If-None-Match: *`. An SD ledger, append only, records what was
  sent. The uploader never lists or reads the bucket.
- The object key ends with the SD relative archive name, verbatim. That name
  starts at `thing=<uuid>/`.
- A `Dev` key is `<product>/dev/opensensor-space-firmware/<board>/<name>`. The
  `<board>` segment is any lowercase slug of 1 to 32 characters from `[a-z0-9-]`
  that does not start or end with `-`, and the caller passes it. The header
  comment uses `cores3` and `waveshare-sim7670g`, and the host tests use
  `cores3`. These are not the footer `thing.board` slugs
  `m5stack-cores3` and `waveshare-esp32-s3-sim7670g`, and no code compares them.
  A board that wires the uploader picks the segment.
- A `Prod` key is `<product>/<prod_prefix><name>`. The prefix must not be empty,
  must end in `/` and must not start with `dev/`. So a prod key is never
  `opensensor-space/thing=<uuid>/...`, the path the stac-opensensor PROFILE
  publishes. Prod is meant for a private intake product that a publish Action
  copies into place (#81). The intake product and prefix wait on the owner.
- Since [decision 0001](decisions/0001-standalone-thing-stac.md) the board also
  writes STAC. Each dated file gets a sidecar `<file>.json` File Item next to it
  on SD, and it is meant to travel with the Parquet under the same key prefix.
  Today the uploader sends only `.parquet` names, and the BLE and LAN name rule
  accepts only `.parquet` too. Sidecar transport is future work in a separate
  issue. The board
  writes the Thing Collection from its upload ledger. Neither writer is built
  yet.

MQTT QoS 1 can carry live gauges, alarms and device health. It is not the durable measurement source because duplicates and reconnect gaps are normal. Deduplicate any live copy by the same row identity.

The ingestion service can accept the device's finalized Parquet directly. Uninterrupted 10/15/30/60-minute rotation produces 144/96/48/24 files per Thing per UTC day (the longer windows hold 2 or 4 row groups each); extra splits produce more. Cloud compaction can follow as fleet size and query costs warrant. Apache Iceberg catalog and snapshot management remain later work.

For later deployment, OpenTelemetry belongs at the gateway and ingestion services. The current device emits acquisition/storage counters, heap/queue health and a startup reset report. The new source adds an acquisition configuration identifier and dictionary digest; actual calibration/deployment history, RSSI and upload success remain future work. Translate device health into cloud metrics, logs and traces when networking is added.

## Offline capacity and reconnection

Internet is not required for acquisition or SD finalization. With continuous power and working storage, the current firmware keeps collecting offline; configured Wi-Fi tries router DHCP42 and local servers before permitted public fallback; successful local NTP can anchor time without internet. Without Wi-Fi, an authenticated phone supplies UTC over Bluetooth or LAN. The most recent UTC anchor continues from the monotonic clock during an uninterrupted boot, including disconnections, with unmeasured drift. After reboot, a usable RTC is restored when available; otherwise UTC is null and files use the unsynced tree until a new time anchor arrives. The Thing UUID remains persistent. Power loss can discard the unfinished RAM batch; this is independent of available card space.

**Capacity estimate, not lifetime qualification.** The tested nominal 32 GB card reports a 31,441,764,352-byte filesystem. At one row every ten seconds, there are 8,640 rows/day. Assuming future file sizes resemble the measured files and reserving 20% of the filesystem:

The following file sizes are from the **73-column hardware baseline**. Current 81-column source adds timing, location and footer metadata; measure its real compressed files before reusing these rates for deployment planning.

| Measured file basis | Rotation assumed | File bytes/day, excluding FAT allocation overhead | Decimal GB/year | Capacity-only years with 20% reserve |
| --- | --- | ---: | ---: | ---: |
| 60 rows, LZ4_RAW, 14,080 bytes | 10 minutes / 144 files/day | 2,027,520 | 0.74 | ~34 |
| Same 60 rows, uncompressed, 28,537 bytes | 10 minutes / 144 files/day | 4,109,328 | 1.50 | ~17 |
| 90 rows, uncompressed, 39,869 bytes | 15 minutes / 96 files/day | 3,827,424 | 1.40 | ~18 |

These are arithmetic projections, **not claims that the board/card will run for decades**. The current normal interval is 15 minutes; its full 90-row compressed size is not yet measured. Compression varies with values/validity. Directory entries, allocation units, other files, benchmark copies, partial files and short forced batches consume additional space. The actual FAT cluster size is not recorded. For illustration only, if each 10-minute file occupied a 32 KiB cluster, both 14,080 and 28,537-byte files would allocate 32,768 bytes: ~1.72 GB/year and ~15 capacity-years with the same reserve. Thus a 50.7% byte reduction does not automatically halve allocated SD space; it does reduce the bytes a future uploader would send. Measure actual allocated growth before making a retention guarantee.

Card endurance, bit retention, environmental conditions, continuous power, clock accuracy and long-run firmware reliability were not qualified. This is not a battery-runtime estimate. No automatic deletion/retention policy exists: on storage failure the batch is retained in RAM and later drops/errors are counted. A free-space reserve/alarm and tested recovery policy must be implemented before unattended deployment.

**Can it sync afterward? Architecturally yes, but no board uploads yet.** The immutable files and Thing and date keys can be copied to object storage without re-encoding. The uploader software landed in #129 and is off by default, see [uploader software](#uploader-software-129). No device has performed an upload. The remaining work is listed below.

1. provision an endpoint, TLS trust and narrowly scoped credentials, without blocking acquisition;
2. enumerate finalized normal files (exclude diagnostic copies/partials), stream bounded SD reads and retry the same object identity after disconnects;
3. verify remote size/content digest and persist acknowledgement locally; never equate a send attempt with durable acceptance;
4. retain local files until that acknowledgement, with an explicit free-space/deletion policy and replay-safe recovery tests;
5. test disconnects, uncertain upload completion, expired credentials, clock changes and power cuts, plus the shared SD/display/radio workload.

Unsynced files must remain explicitly unsynchronized unless a defensible later clock reconstruction exists. Iceberg/catalog management and cloud compaction can follow basic object upload; neither is needed to establish local Parquet feasibility. Network upload, long offline soak and card lifetime therefore remain open validation questions, not completed features.

## C and C++ library assessment

These are dated observations, not floating dependencies.

| Project | Version checked | Fit for CoreS3 |
| --- | --- | --- |
| [Apache Parquet format](https://github.com/apache/parquet-format/releases/tag/apache-parquet-format-2.14.0) | 2.14.0 (2026-09-11), Apache-2.0 | The wire specification. Metadata and page headers use Thrift Compact Protocol; it is not an embedded writer library. 2.13 added `Statistics.nan_count` and `IEEE_754_TOTAL_ORDER` with new float-statistics writer rules; 2.14 adds only the `FILE` logical type and `INT96_TIMESTAMP_ORDER`. The writer follows the 2.13 `TYPE_ORDER` rules (checked 2026-09-17). |
| [Apache Arrow C++](https://arrow.apache.org/blog/2026/08/10/25.0.1-release/) | 25.0.1, Apache-2.0 | Production Parquet implementation for host/cloud. Its full runtime/dependency graph is not the proposed firmware writer; its footprint does not establish the cost of a bounded Parquet-only implementation. |
| [nanoarrow](https://github.com/apache/arrow-nanoarrow) | 0.9.0, Apache-2.0 | C runtime compiles to a few hundred KiB and writes Arrow IPC through `FILE*`, but it does **not** write Parquet or compressed IPC. |
| [Carquet](https://github.com/Vitruves/carquet) | 0.7.0 in the earlier release snapshot, MIT; re-pin before integration | C11 Parquet reader/writer candidate. Upstream's roughly 200 KB binary claim is not a CoreS3 RAM measurement. Audit its build, codec dependencies and allocator requirements before selecting a firmware subset; no ESP32-S3 qualification is established here. |
| [DuckDB](https://github.com/duckdb/duckdb/releases/tag/v1.5.5) | 1.5.5, MIT | Excellent host validator, but its database surface and roughly 125 MB per-thread memory guidance are beyond the device budget. |
| [zcbor](https://github.com/NordicSemiconductor/zcbor/releases/tag/0.9.1) | 0.9.1, Apache-2.0 | Low-footprint C CBOR with CDDL-generated codecs, static-friendly operation and fragmented buffers. |
| [Espressif LZ4](https://components.espressif.com/components/espressif/lz4/versions/1.10.0/readme) | 1.10.0, BSD-2-Clause/Apache-2.0 integration | Block state is about 1–16 KiB; Frame defaults need at least 64 KiB and can use PSRAM. Parquet requires raw blocks under `LZ4_RAW`, never LZ4 Frame output. |
| [Zstandard](https://github.com/facebook/zstd/releases/tag/v1.5.7) / [IDF wrapper](https://components.espressif.com/components/rderr/esp-idf-zstd/versions/1.5.7/readme) | 1.5.7, BSD-3-Clause option | The wrapper reports about 65–220 KiB heap plus a dedicated task with roughly 16 KiB stack. Current IDF packaging is lightly used and third-party. |
| [Espressif zlib](https://components.espressif.com/components/espressif/zlib/versions/1.3.2~1) | 1.3.2~1, zlib License | Mature official component. Parquet `GZIP` requires a gzip wrapper, not a raw deflate or zlib stream. |
| [Snappy](https://github.com/google/snappy/releases/tag/1.2.2) | 1.2.2, BSD-3-Clause | Fast and widely readable by Parquet tools, but there is no official Espressif component. |

The archived standalone `parquet-cpp` project is not a path forward; its work moved into Apache Arrow. Full Arrow, Velox and DuckDB remain host/cloud tools.

Secondary findings: ESP-IDF's ROM `miniz.h` is a restricted legacy subset and must not be treated as current [miniz 3.1.2](https://github.com/richgel999/miniz/releases). [heatshrink](https://github.com/atomicobject/heatshrink) is small but not a Parquet codec. Current [MessagePack C/C++](https://github.com/msgpack/msgpack-c/releases) is maintained, while its available ESP registry wrapper is immature and offers no clear advantage over a generated fixed schema. [xxHash 0.8.3](https://github.com/Cyan4973/xxHash/releases/tag/v0.8.3) is useful elsewhere but cannot replace the standard CRC32 required by Parquet page checksums.

## Direct Parquet experiment

Parquet allows single-pass creation because column chunks are written before the footer, but readers locate metadata from the final footer. A power cut before that footer leaves an incomplete file, and appending rows to a closed file requires replacing the old footer. Use immutable rotations and an explicit unfinished-batch policy.

The current implementation in the active C/C++ trial uses:

- one immutable file per rotation window holding one row group per completed 90-row batch (up to eight; one at the default 900 s);
- flat INT32/INT64/FLOAT columns; the logger provides definition levels for every column, always populating identity/status fields and leaving unavailable measurements/time null; the two UTC INT64 fields carry `TIMESTAMP(MICROS, isAdjustedToUTC=true)`; every leaf carries a positive, unique `field_id`;
- PLAIN encoding and Data Page V1, one page per column chunk;
- column-chunk statistics per row group: `null_count` always; `min_value`/`max_value` with `is_min_value_exact`/`is_max_value_exact` when at least one non-NaN value is present; `nan_count` on FLOAT; `FileMetaData.column_orders` = `TYPE_ORDER` for every leaf (mandatory once bounds are written). Floats follow the parquet-format 2.13 `TYPE_ORDER` rules — NaN excluded from bounds, `-0.0` written for a zero minimum and `+0.0` for a zero maximum — because PyArrow 25 and DuckDB 1.5 still ignore statistics under the newer `IEEE_754_TOTAL_ORDER`. The deprecated `Statistics.min`/`max` pair is omitted to keep the 77-chunk footer small; every targeted reader uses the `*_value` fields;
- row-group metadata `file_offset`, `total_compressed_size`, `ordinal` and `sorting_columns` (ascending `sequence`); `ColumnChunk.file_offset` stays 0 as the 2.14 thrift now asks;
- LZ4_RAW by default using pinned LZ4 1.10.0, or UNCOMPRESSED, chosen with `parquet codec` and saved in NVS across reboots;
- bounded page-codec buffers and correct compressed/original byte counts; no Parquet page CRC32 yet (deferred on 2026-09-17), while serial/BLE readback has a separate whole-file CRC32;
- a fixed 90-row logger batch and bounded streaming buffers; footer records for eight row groups × 96 columns live in the PSRAM workspace (about 50 KB per open file);
- a minimal writer-only Thrift Compact implementation in `parquet_writer.cpp` (streaming `Writer` class; `write_parquet()` wraps one group), without Arrow or Carquet dependencies; `tools/parquet_footer.py` is the matching host-side decoder used by the tests;
- `.partial` -> row group -> sync (repeat) -> footer -> sync -> close -> footer validation -> final rename.

Footer cost was measured on the host fixture with 77 columns × 90 incompressible rows, not real data. It is 4,484 bytes before statistics and 6,000 with them. It is 10.4 KB for two groups and 19.3 KB for four. Per-chunk metadata (~57 bytes × 77, mostly the required `path_in_schema` string) dominates, so several row groups per file cut the footer share only from about 17 % to 14 %. Hourly files (3600 s rotation, the longest allowed, default 900 s) are worth having for four times fewer files and per-quarter-hour statistics, not for bytes. Bytes come from LZ4 and from the phone's derived daily files.

Host conformance checks use PyArrow and DuckDB, including nulls and exact integer values, plus a field-level decode of the footer. Empty, one-row, boundary-value, wide/maximum-row, four-group "hourly" and eight-group fixtures pass the host sanitizer test, including `-0.0`/`+0.0`, all-NaN and all-null bounds; unsynced and dated single-group files pass real SD readback, and the first two-group v6 device file (105 rows across a window boundary) passed both readers and the footer decode on 2026-09-17. Add multiple-page cases only if that feature is introduced. The automatic 60-row run and current Hive-schema checks are recorded separately in [bench-verified](../boards/m5stack-cores3/bench-verified.md); longer runs and failure cases remain open. The [compression experiment](../boards/m5stack-cores3/compression-benchmark.md) screens Snappy/LZ4/Zstd on the host and adds an identical-row LZ4_RAW comparison on the board. Measure complete-file bytes, CPU time and memory bounds; on-device Snappy/Zstd and longer codec runs remain open. Separately run repeated power cuts during data, page header, footer, sync and rename before claiming recovery guarantees. The cited Zstd wrapper's memory figures are specific to its dictionary workload, not a CoreS3 budget.

Do not use Parquet's deprecated `LZ4` enum, LZ4 Frame payloads, dictionaries, nested schemas, append-to-finalized files or one file per sample in the first experiment.

## Operational cases to implement

### Lessons established by the feasibility work

- **Format feasibility is narrower than runtime size.** The user's Rust experiment justified revisiting the earlier cloud-only recommendation. The local bounded C++ writer now has real-sensor SD evidence without importing Arrow; that does not make its limited schema support a general-purpose Parquet implementation.
- **Measure complete files.** The first seven-row file had a 5,490-byte footer in 9,494 total bytes; the 60-row file was 28,059 bytes. Metadata overhead matters for small batches. Compare codec candidates on identical full files and distinguish finalization time from encoding-only time.
- **Prove the bytes at each layer.** Writer fixtures, device footer/CRC readback, serial length/offset/CRC checks and independent reader comparison test different failure surfaces. A transfer CRC is neither a Parquet page checksum nor a cryptographic upload identity. Host query partition columns must be checked separately from stored schema columns.
- **Reset, missing data and clock changes need explicit semantics.** Correct serial control-line handling stopped accidental resets; it did not make RAM durable. Null unsupported readings, unsynced paths and immutable clock epochs preserve uncertainty rather than hiding it.
- **More cores do not remove shared-bus ownership.** A separate storage worker absorbed the tested finalization workload, with no measured reason yet to pin cores. Keep DMA completion and the common SPI lock; profile codecs and future radios separately.
- **Keep evidence scoped.** The complete 60-row run used the earlier 72-column image. Current 73-column Hive files, quarter-hour splitting and normal-restart retention have their own short checks. A later full 90-row uncompressed Hive file now has separate readback evidence; compressed endurance, clock-correction stress and power-cut recovery remain open. [Dated artifacts and results](../boards/m5stack-cores3/bench-verified.md#board-1-on-device-parquet-and-hive-partitions)

### Remaining operational checklist

[Open Air's firmware](https://github.com/Open-Air-Foundation/firmware-one-openair) was inspected only as a checklist of field cases; no source, schema, protocol or architecture was imported. Our implementation must cover:

- independent sensor schedules and warm-up state;
- checksum resynchronization, invalid and stale measurements;
- bounded queues, explicit drop policy and offline retention;
- watchdog and reset-reason reporting;
- SD removal, full media, short writes and partial-tail recovery;
- Wi-Fi reconnect without blocking acquisition;
- idempotent upload and duplicate handling;
- configuration/calibration versions and safe rollback;
- signed OTA with health confirmation before marking a new image valid;
- visible counters for heap, RSSI, sensor failures, storage failures and upload backlog.

## Implementation order

1. **Implemented:** live 10-second measurement collection, bounded C++ Parquet writer, SD finalization and host conformance/readback tooling in the shared native ESP-IDF applications; historical Arduino images supplied the recorded physical evidence.
2. **Hardware readback verified:** automatic 60/90-row uncompressed files, three identical-row LZ4 comparison pairs and a normal compressed Hive smoke file open in both readers; exact scope and measured timings are in [bench-verified](../boards/m5stack-cores3/bench-verified.md).
3. **Implemented, partly bench-verified:** persistent Thing UUID, UTC Hive layout, clock epochs and configurable 600/900/1800/3600-second windows with one row group per completed batch. Thing id persistence, normal-reset file retention, UTC partition agreement, a quarter-hour boundary split, a full 90-row uncompressed window and (v6) two-row-group half-hour files with statistics passed readback; still test midnight, arbitrary clock corrections, four-group LZ4 files and a reset between row groups.
4. Measure actual memory, bytes/row, encoding time, SD latency and sampling jitter across longer runs and failure cases.
5. Compare configured LZ4_RAW and UNCOMPRESSED against identical buffered rows, retaining diagnostic copies outside Thing trees. Continue with on-device Snappy/Zstd and longer-run memory/latency qualification; see [compression](../boards/m5stack-cores3/compression-benchmark.md).
6. Implement the chosen reset-loss policy, adding a recovery spool if needed, and verify controlled reset/power-cut behavior.
7. Add immutable HTTPS upload and acknowledgement retention. Cloud compaction and Apache Iceberg remain later milestones.

Useful primary references: [Parquet file layout and recovery](https://github.com/apache/parquet-format/blob/master/README.md), [Parquet compression rules](https://github.com/apache/parquet-format/blob/master/Compression.md), [ESP-IDF FatFS behavior](https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-reference/storage/fatfs.html), [ESP-IDF filesystem resilience guidance](https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-guides/file-system-considerations.html), [ESP32-S3 heap capabilities](https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/api-reference/system/mem_alloc.html), and [Apache Iceberg specification](https://iceberg.apache.org/spec/).
