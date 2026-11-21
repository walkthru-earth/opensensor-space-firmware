# Bench-verified board record

**Framework scope, 2026-10-10:** Current source is native ESP-IDF 6.1 and its
build/host gates pass. The dated LAN memory recovery below and the
[first native flash/readback](#board-1-first-native-esp-idf-flash-and-readback)
below qualifies a short CoreS3 run. Earlier physical observations, images,
old API/tool commands and performance figures belong to historical Arduino
builds; retain those identities when using this evidence.


[Router](../../README.md) · Everything else in `docs/` is source-checked against schematics and vendor code. This file records only what was **measured on real hardware in this project**, with the date and the method. If a claim elsewhere disagrees with this file, this file wins for the board listed here.

## Board 1, native LAN memory and complete-list recovery

Verified **2026-10-10**, CoreS3 MAC `44:1b:f6:e2:6b:40`, native ESP-IDF
6.1 (`fff9895c82d744c7237be8847347bdd1b07c6643`) and the existing SD archive.
Old v6.10 LAN refreshes returned successful prefixes of 12–13 files ending
near 02:30–02:45 local time, while USB listed 35 files including the morning
and new afternoon files. During the LAN refresh, serial captured
`sdmmc_cmd: allocate_dma_buf: not enough mem, err=0x101` and
`diskio_sdmmc: sdmmc_read_blocks failed (0x101)`. A later old-image flush
reported internal heap minimum 952 bytes. The observed fault was DMA allocation
pressure plus directory errors being treated as EOF, not missing source files.

Repeated port/chip/flash-ID/read-only eFuse checks identified the same ESP32-S3,
16 MB Quad flash and recoverable security state. Before writing, a full
16,777,216-byte backup was verified at
`backup/m5stack-cores3-flash-20261010T135448Z.bin`, SHA-256
`71179bc0634e350dbddad6aa4581f3d2c9b07a219d3053eef1dc2b6fa8da9912`.
Pending old-image rows were explicitly flushed before the backup/reset and
again immediately before flashing. NVS, pairing, station identity and SD files
were preserved; no card format, file deletion or eFuse write occurred.

Installed **v6.11**, application size 1,521,200 bytes, SHA-256
`50e8b39ccc4361c469332e9f17e7a8cf3a949e08378f9265023597e43ac39118`;
esptool verified written hashes. Station remained
`f3c3fdc1-6f18-4be8-b7bb-fb27692bb60a`; new boot ID was
`3e66a92f480d8e9adc4bfc31923c9261`. SD mounted using a persistent heap-owned
4,096-byte internal DMA buffer with eight-sector chunks, reserved before radios.
At the `sd-mounted` stage, DMA free/largest/minimum were
195,583 / 122,880 / 159,348 bytes. These startup measurements precede Wi-Fi load
and must not be read as loaded headroom. Wi-Fi/lwIP prefer PSRAM, with eight
static RX and eight static TX buffers and bounded software queues.

The updated Android debug app on the OnePlus 7 Pro connected over authenticated
LAN and automatically copied three new files (97,718 bytes), zero failures.
The phone displayed 37/37 checked files including 08:00; independent USB LIST
returned 37. A subsequent LAN refresh again returned 37, and the interactive
session stayed ready beyond five minutes without reconnection. Ten-second
sampling continued with zero reported drops during the passive capture.

At the next automatic quarter-hour boundary, v6.11 finalized
`data_1400_3e66a92f480d8e9adc4bfc31923c9261_0-89-0.parquet`: 90 rows,
one row group, 49,839 bytes, CRC-32 `bf653c17`. Background sync requested from
the phone's Files menu discovered and copied this file over LAN, zero failures;
the phone then showed 38/38 checked files. Read-only extraction from the phone
matched that size/CRC and SHA-256
`c12d21fe2dce097b4f725d3eed959bdadbe8f91ed9cae5cba6801e0dc95fa4a6`.
PyArrow and DuckDB read all 90 rows and 81 columns; sequence 0–89 was complete,
UTC ranged 14:00:06.445829–14:14:57.471461, and drop, missed-deadline, storage,
PMS checksum and PMS length error fields were all zero. Acquisition continued
after finalization. At READY under the connected LAN load, DMA free/largest/min
were 19,767 / 19,456 / 18,296 bytes; internal free/min were 27,555 / 26,084,
PSRAM free was 8,146,416 and storage-task free-stack report was 10,208 bytes.
These are one boot's observed values, not qualified worst-case margins.

Sanitized Parquet writer conformance, SD-buffer ownership/reuse/allocation-failure tests, directory failure
tests and archive incomplete/terminal-delivery/count-overflow tests passed.
Formatting, lint, both board telemetry contracts, and native CoreS3, Waveshare
logger and Waveshare diagnostic builds/target gates passed. Waveshare changes
are build/host-verified only; that board was not flashed for this change.
Android's full verify gate passed separately. This short check does not qualify
overnight endurance, full-card throughput or power-cut durability.

Evidence is ignored under
`firmware/esp-idf-cores3/artifacts/lan-memory-20261010/`: before/after phone
screenshots, serial DMA failure capture, sanitized Android receipt summary,
backup/flash logs, boot output, USB listing and the passive post-flash capture.

## Board 1, first unit

Verified **2026-09-06** over USB-C to the native USB-Serial/JTAG port, using `esptool` 5.3.1 from the pixi environment. No firmware of ours had been flashed yet, so these are read-only observations of a factory board.

| Property | Measured value | How |
| --- | --- | --- |
| Chip | ESP32-S3 (QFN56) revision **v0.2** | `pixi run chip` |
| Features | Wi-Fi, BT 5 LE, dual core plus LP core, 240 MHz | `pixi run chip` |
| Crystal | 40 MHz | `pixi run chip` |
| Base MAC | `44:1b:f6:e2:6b:40` | `pixi run chip` |
| USB mode | USB-Serial/JTAG | `pixi run chip` |
| Flash | 16 MB, manufacturer `0x46`, device `0x4018` | `pixi run flash-id` |
| Flash lines | Quad, 4 data lines, confirmed in eFuse `FLASH_TYPE` | `pixi run efuse` |
| Flash voltage | 3.3 V, set by strapping pin | `pixi run flash-id` |
| Wafer | major 0, minor 2, `PKG_VERSION` 0 | `pixi run efuse` |
| GPIO33 to GPIO37 supply | `PIN_POWER_SELECTION` is `VDD3P3_CPU` | `pixi run efuse` |
| Security fuses | Secure boot, flash encryption and JTAG disable are all **unburned**. The board is fully recoverable. | `pixi run efuse` |

Full 16 MB backup taken and kept before any write. The board shipped with a UIFlow MicroPython image, whose partition layout is recorded below for reference.

| Partition | Type | Subtype | Offset | Size |
| --- | --- | --- | --- | --- |
| nvs | data | nvs | `0x009000` | 24K |
| phy_init | data | phy | `0x00f000` | 4K |
| factory | app | factory | `0x010000` | 9792K |
| sys | data | fat | `0x9a0000` | 1024K |
| vfs | data | fat | `0xaa0000` | 4864K |
| storage | data | spiffs | `0xf60000` | 640K |

Flashing any trial in this repo overwrites that image. Inspect a saved backup with `pixi run backup-partitions backup/<file>.bin`.

## Board 1, bring-up firmware

Verified **2026-09-08** with `cores3-bringup-v1`, built from Arduino-ESP32 3.3.11, M5Unified 0.2.21 and M5GFX 0.2.28. The image was flashed at 115200 after repeating the read-only identity, flash and eFuse checks and confirming the existing 16 MB backup. Serial output was captured after a physical RST press; the helper reconnected after macOS re-enumerated USB.

| Property | Measured value | How |
| --- | --- | --- |
| Runtime board | M5Stack CoreS3, ESP32-S3 revision 2, two cores | Diagnostic chip report |
| Flash at runtime | 16,777,216 bytes, QIO, 80 MHz | Arduino `ESP` runtime APIs |
| PSRAM at runtime | **8,388,608 bytes detected**; 8,385,136 free and 8,257,524 largest free block after board initialization | Arduino `ESP` and IDF heap-capability APIs |
| Internal heap | 380,688 bytes total, 335,268 free, 329,996 minimum free and 278,516 largest free block | Arduino `ESP` and IDF heap-capability APIs |
| Internal I2C | Nine expected devices, no unexpected devices: `0x21`, `0x23`, `0x34`, `0x36`, `0x38`, `0x40`, `0x51`, `0x58`, `0x69` | M5Unified-owned internal bus at 100 kHz |
| PMIC and USB | Type 4; external 5 V on; USB in input mode; VBUS 5,134 mV | M5Unified `M5.Power` APIs |
| Battery report | 27 mV, 0 percent, discharging and 0 mA | M5Unified `M5.Power` APIs; this does not establish whether a battery is attached |
| RTC | Device acknowledged at `0x51`; the date/time read failed | M5Unified I2C and RTC APIs |
| IMU | Enabled as type 6; accelerometer and gyroscope reads succeeded | M5Unified IMU API |
| Touch | Controller acknowledged at `0x38`; zero active points during the capture | M5Unified I2C and touch APIs |
| microSD | One nominal 32 GB SDHC card mounted at 25 MHz; card size 31,457,280,000 bytes, filesystem size 31,441,764,352 bytes | Arduino SD API; no format or file write |
| PMSA003 | Checksum-valid frame, sensor error 0, firmware 151 | GPIO18 RX/GPIO17 TX at 9600, bounded five-second parser |
| Live PMS interval | Two successive reports approximately 10 seconds apart; latest-frame ages 423 ms and 804 ms; zero checksum and length failures across 23 frames | 24-second bounded serial capture after flashing the live-display revision |
| Touch UI | Final image uses 554,315 bytes of program storage and 26,276 bytes of static internal RAM. Flash hashes verified. Up and down swipes plus taps reached all three pages while four timed PMS reports continued with zero parser failures. | Flash plus a 40-second serial capture during the touch test |

The PMS frame reported atmospheric PM1, PM2.5 and PM10 values of 19, 24 and 26 µg/m³. This confirms framing and transport for the attached sensor; one reading does not validate calibration.

## Board 1, on-device Parquet and Hive partitions

Verified **2026-09-08**, on the same board and mounted nominal 32 GB SDHC card, using the active Arduino trial and the pinned versions above. Repeated port, chip, flash-ID and read-only eFuse checks before flashing; confirmed `backup/cores3-flash-20260906T102150Z.bin` is exactly 16,777,216 bytes. Flash hashes verified. No formatting, file deletion, eFuse write or radio setup was performed.

The firmware collected real scalar snapshots every ten seconds while the PMS parser and display continued running. A separate storage task wrote Parquet directly to SD; the host fetched the existing bytes over USB, checked transfer length/CRC32 and compared every stored value/null with **PyArrow 25.0.0 and DuckDB 1.5.5**. This was not host-side conversion. All files below use uncompressed pages, one row group and one page per column.

| Run | Measured result |
| --- | --- |
| Initial manual flush, 72-column flat-path image | 7 rows, sequences 0–6; 9,494 bytes, including a 5,490-byte footer; finalization 51,998 µs, flush/sync/close 12,765 µs |
| Automatic 10-minute batch, same image | **60 rows**, sequences 7–66; **28,059 bytes**; finalization **122,041 µs**, flush/sync/close **13,317 µs**; both readers matched |
| Sampling in the 60-row file | Adjacent intervals 9,990,877–10,015,182 µs; mean 10,000,237 µs; maximum deadline jitter 18,671 µs. Drop, missed-deadline, storage-error and PMS checksum/length-error counters were all zero in the file |
| Memory at that automatic finalization | Internal heap free 310,216 bytes; PSRAM free 8,290,860 bytes; reported storage-task stack free 7,480 bytes |
| Pre-codec Hive image | **73 columns**, including `clock_epoch`; program storage 579,711 bytes, static internal RAM 26,988 bytes; PSRAM batch/descriptors/workspace allocation 63,560 bytes, 664 bytes per in-memory row; default interval 900 seconds |
| UTC Hive file, manual flush | 3 rows, sequences 0–2; 8,319 bytes; finalization 58,463 µs, flush/sync/close 11,298 µs; all UTC values present with host-clock status/epoch 1. UTC date and 18:30 window agreed with the path. Both readers matched; DuckDB Hive discovery returned the expected station and date |
| Automatic quarter-hour boundary | The next file contained sequence 3 under `data_1830_…` (7,630 bytes, both readers matched); subsequent sequences 4–9 were finalized under `data_1845_…`. This verifies a boundary split, **not a full 90-row/15-minute run** |
| Hive image memory at initial finalization | Internal heap free 308,260 bytes; PSRAM free 8,288,812 bytes; reported storage-task stack free 5,632 bytes |
| Normal software restart | Station UUID remained `53315f5f-cb85-4d8d-b623-d56266084189`; boot ID changed. Three finalized dated files were still enumerated. Re-fetching the initial 8,319-byte Hive file matched the pre-restart file byte-for-byte and passed both readers again |
| No UTC after restart | A new 3-row, 8,295-byte file was written under `station=<UUID>/unsynced/boot=<new-boot>/`; all three UTC values were null and both readers matched. Unsynced rows were not assigned a fabricated calendar date |
| Host time restored after restart | The pending unsynced sequence 22 was finalized separately; sequences 23–25 formed a dated 8,343-byte `data_1845_…` file, CRC32 `7d898d53`. Both readers and per-row UTC/path agreement passed. Capture: `hive-resync.log` |
| Runtime interval commands | `parquet interval 600` and then `parquet interval 900` each finalized pending rows and acknowledged the new interval. The device was returned to 900 seconds |
| Final live status | `interval_s=900 buffered=11 finalized=5 dropped=0 errors=0 queue_peak=1 failed=false`; host time had been restored and the logger was left running |
| Scalar sensor availability | PMS values became non-null after the software warm-up gate; IMU freshness mask was 7; raw light and proximity reads were valid. RTC calendar, ambient temperature/humidity and unsupported battery current remained null. This verifies acquisition, not sensor calibration or warm-up sufficiency |
| Later complete 15-minute uncompressed window | 90 rows, sequences 80–169, **39,869 bytes**, under `data_1900_1491409bed013e1999dc4072ac91ab0f_80-169-6.parquet`. Both readers matched, every UTC row belonged to the 19:00–19:15 window, intervals were 9,992,996–10,014,823 µs and maximum deadline jitter 19,785 µs. Drop, missed-deadline, storage-error and PMS checksum/length-error fields were all zero. CRC32 `f198b241`, SHA-256 `fa514df4894cb87910362e72db9fe05804faa1b7042ea3d6e0b84f3f386ec843` |

`write_us` measures the finalization path including writing, sync/close, structural readback, CRC and rename; `sync_us` is its flush/sync/close subset. These are individual observations, not latency bounds or p99 estimates. Free-stack values are runtime reports, not a qualified worst-case margin. The workers were not explicitly pinned to cores; no dual-core speedup was measured.

The following paths describe the **original** build-local artifacts. A later Arduino rebuild cleaned that directory: the earlier serial logs and replaced binaries are no longer retained there. All 13 finalized SD Parquet files were re-fetched and validated under git-ignored `artifacts/exports/restored-20260908/`, outside the disposable build directory; that snapshot includes the original files plus the later 90-row and 52-row files. Legacy names are under `legacy-parquet/`, Hive names under `output/`, with SHA-256/CRC32/source records in its manifest. Artifacts are not included in a Git clone. Card brand/model and accessory PCB revision/population remain unrecorded, so do not extrapolate these results to all cards or M134 revisions:

- `parquet-readback/f1179d54856fa20c31038836e5ec177e-7-66-1.parquet`: CRC32 `785aa6d0`; SHA-256 `317080e640485265258462bae724d8095916368d19a66c6919e18c86c213d4e3`. `parquet-ten-minute.log` contains 56 row reports whose sequence/monotonic timestamps match their stored rows; independently refreshed display measurements were not used as exact-value comparisons.
- `output/station=53315f5f-cb85-4d8d-b623-d56266084189/year=2026/month=09/day=08/data_1830_c838cf99f4a1e867ebec9e2752bad519_0-2-0.parquet`: CRC32 `ce4bb0b8`; SHA-256 `ad0b32e81b233a68a867e31df8db45222dacf4a137ea3a244390389ee53bfd51`. Capture: `hive-first.log`.
- Restart capture/readback: `hive-restart.log`, boot `1491409bed013e1999dc4072ac91ab0f`, unsynced-file CRC32 `25a3b7f7`.
- Pre-codec Hive application binary SHA-256: `5227a989df0a9b3264117e773d3bc8a54c235a2f83d1e54037ab8966fab2c4b9`. Earlier 72-column application SHA-256: `9d5684465ec639c59fac19fa72ccb37c5429b97f22334eba6a29ee08f00a303f`.

Separately, **host-only** `pixi run parquet-test --sanitize` passed empty, one-row, 90-row × 96-column and 65,536-row fixtures, including exact integer boundaries, nulls and IEEE float edge cases, with both readers and address/undefined-behavior sanitizers. These tests do not establish device performance or SD failure behavior.

## Board 1, schema v2 provenance and timing

Verified **2026-09-08**, same CoreS3/MAC `44:1b:f6:e2:6b:40` and nominal 32 GB card. Before flashing, a 76-row old-image LZ4 batch (sequences 272–347) was finalized/exported with both readers matching; two subsequent rows (348–349) were flushed to SD immediately before the read-only chip/flash/eFuse checks. The original 16,777,216-byte backup was present. Checks confirmed ESP32-S3 rev0.2, 16 MB Quad flash and unchanged unburned secure-boot/encryption/JTAG-disable fuses. No SD format/delete or eFuse write occurred.

Flashed `arduino-cores3-parquet-v3`, schema `cores3-telemetry-v2`, with application SHA-256 `a239223f55aae798ea3eb0839723ebb96e00e5961808eba1aa522f7d09538b1c`; esptool verified the written hashes. The retained binary is `artifacts/firmware/cores3-parquet-v3-a239223f.bin` (590,032 bytes; compiler program usage 589,887 bytes, static RAM 26,996 bytes). Boot ID `763038f2396a5d68927ac324c4a744a2`; station UUID remained `53315f5f-cb85-4d8d-b623-d56266084189`. Startup enumerated existing files, reported zero partial files, **77 columns**, 696-byte RAM rows and 73,512-byte PSRAM writer allocation. Runtime detected 8 MB PSRAM. `parquet schema` completed with the 77 field definitions and the expected dictionary digest.

| Readback | Result |
| --- | --- |
| Unsynchronized, UNCOMPRESSED | 3 rows, sequences 0–2, **9,671 bytes**, footer 6,868 bytes; CRC32 `ec7f0284`; finalization 59,883 µs, sync 12,929 µs |
| Host-anchored UTC, LZ4_RAW | 4 rows, sequences 3–6, **9,821 bytes**, footer 6,863 bytes; CRC32 `75030cfc`; finalization 97,565 µs, sync 13,048 µs, codec 1,068 µs |
| Collection timing | Start-to-completion 5,354–5,402 µs across the seven rows; maximum deadline jitter 16,502 µs. Four-row LZ4 cadence 9,995,000–10,015,000 µs |
| Memory at LZ4 finalization | Heap free 307,996 bytes, minimum 307,160 bytes; PSRAM free 8,279,596 bytes; storage-task free-stack report 5,352 bytes |

Both files passed transfer length/CRC and exact PyArrow/DuckDB value/null comparisons. Additional checks matched all column names against the source dictionary, schema numeric version 2, firmware/configuration metadata, dictionary SHA-256, 900-second rotation and explicit unknown deployment/calibration. Every PMS receipt preceded or equaled snapshot start, which preceded collection completion. The unsynced file kept UTC and both anchors null; its first two rows had warm-up status and null PMS measurements. The anchored file reproduced UTC exactly from each row's anchor pair and belonged to the `2026/09/08/data_2015` UTC window. All seven rows had zero drop, missed-deadline, storage-error and PMS checksum/length-error counters. Unsupported ambient temperature/humidity and battery current stayed null.

Artifacts are git-ignored below the trial directory:

- `artifacts/schema-v2-boot.log`, `artifacts/schema-v2-preflash.log`, `artifacts/schema-v2-unsynced.log`, `artifacts/schema-v2-live.log`.
- `artifacts/schema-v2-unsynced/station=<UUID>/unsynced/boot=763038f2396a5d68927ac324c4a744a2/data_unsynced_763038f2396a5d68927ac324c4a744a2_0-2-0.parquet`: SHA-256 `e314d217bdcf07aa9045c62d4836fc6e3f2a68043ef0d85aff4dfc5772e6f949`.
- `artifacts/schema-v2-live/station=<UUID>/year=2026/month=09/day=08/data_2015_763038f2396a5d68927ac324c4a744a2_3-6-1.parquet`: SHA-256 `955a06fc2358cd11008fd517727c43ad8f0186cd819ad19a3451b3b8b5de4ece`.

The logger was left at **900-second rotation / LZ4_RAW**, host UTC restored, with later status `buffered=3 finalized=3 dropped=0 errors=0 queue_peak=1 failed=false`. Codec/time commands are still runtime-only. These are short manually flushed smoke files, not identical-row codec comparisons, full-window qualification, clock-accuracy calibration, OGC conformance or Iceberg validation. Preserve the older 73-column performance/capacity results under their original image; do not infer a new compression ratio from these differently sized batches.

## Board 1, LZ4 compression

Verified **2026-09-08** with the same pinned Arduino stack, LZ4 1.10.0 (`LZ4_MEMORY_USAGE=12`) and 73-column logger, firmware metadata `arduino-cores3-parquet-v2`. Read-only identity/flash/eFuse checks still showed Board 1, 16 MB Quad flash and unchanged recoverable security state; the original 16 MB backup was present. The pending old-image batch was saved before identity/reset checks. A later flush timed out while no application replied after the safety checks; no success was inferred from that timeout. The new flash completed with hash verification and booted normally.

- Application SHA-256: `63cd8b554e5128369723777778b7116381a615688634857bc331fb5e955116da`; retained at `artifacts/firmware/cores3-parquet-v2-63cd8b55.bin`. Program storage 589,011 bytes, static internal RAM 26,988 bytes. Boot `e452563e96c72ca057a48924550779b6`; station UUID unchanged.
- Method: `tools/benchmark_device_compression.py --min-rows 60 --seconds 700 --repeats 3`, using one serial connection. The board accumulated sequences 0–59 at ten-second cadence, then wrote three uncompressed/LZ4 pairs from the identical RAM rows to a separate `output/benchmarks/` tree. The original batch was retained. Camera/audio/radios were not acquired; UI tap/page-change reports occurred during collection.
- **Complete-file size:** UNCOMPRESSED 28,537 bytes versus LZ4_RAW 14,080 bytes (**50.7% smaller**). All six fetched files passed length/CRC checks, PyArrow/DuckDB comparison and compressed-versus-baseline value/null equality. Per-codec bytes were identical across repeats.
- **Finalization:** UNCOMPRESSED 113,828 / 118,420 / 122,849 µs; LZ4_RAW 91,777 / 93,036 / 98,629 µs. Median reduction 21.4%. LZ4 calls alone took 3,658 / 3,323 / 3,347 µs. See [metric definitions](compression-benchmark.md) rather than treating finalization as codec CPU time.
- **Memory:** explicitly allocated codec workspace 6,208 bytes; free internal heap 308,260 bytes, free PSRAM 8,281,644 bytes, reported free task stack 5,216 bytes at each finalization. Minimum internal heap reached 307,424 bytes. Codec state/raw/output buffers are persistent in PSRAM; no codec per-page heap allocation is used. This is not a radio-load or whole-system peak-memory qualification.
- **Integrity and continuity:** final comparison status `buffered=60 finalized=6 dropped=0 errors=0 queue_peak=1 failed=false`; UTC was null throughout these comparison rows. The helper checked zero stored drop, missed-deadline and storage-error counters. Compression copies are diagnostic duplicates, excluded from normal station telemetry.
- UNCOMPRESSED SHA-256 `afffb06df9ed6fcf9a654f287a4d23c2fc0a4e8ef9fe5d6a074d122eadd404fb`, CRC32 `3a5f3ba1`; LZ4_RAW SHA-256 `16caeebf844d6f7f8474694daf2516c28bbfcb8822b0c5c6daaba0b0407fed75`, CRC32 `d4c4189c`.
- **Post-write acquisition check:** the normal 64-row file, sequences 0–63, preserved its first 60 rows exactly as benchmarked, then continued without gaps. Intervals were 9,981,000–10,015,000 µs, maximum jitter 19,488 µs; drop/deadline-miss/storage-error columns remained zero through the rows after the comparisons. SHA-256 `c42961df2d7cdb67c56fb5af30fd51fc05077c3e10785908212b2df816f16427`, CRC32 `e60d0734`, retained under `artifacts/compression-postcheck/`.
- **Normal compressed Hive smoke test:** after selecting LZ4 and restoring host UTC, sequences 65–67 formed an 8,333-byte `data_1930_…` file with codec/purpose metadata `LZ4_RAW`/`telemetry`. Both readers and UTC/window checks passed; max jitter 10,488 µs, all health counters zero. Finalization 77,036 µs, codec calls 836 µs; SHA-256 `bd5d0965eb2478919059836f05443bae279d5f6bcd64326acc401e001065b589`, CRC32 `64207e6f`. Files/log retained under `artifacts/compression-live/` and `artifacts/compression-live.log`.
- **Configuration/latency follow-up:** switching to UNCOMPRESSED finalized the pending 12-row LZ4 file in 140,291 µs (codec calls 1,338 µs), outside the earlier paired range; that file was not independently fetched in this check. Switching back to LZ4 then acknowledged successfully. Final observed status: `interval_s=900 buffered=0 finalized=10 dropped=0 errors=0 queue_peak=1 failed=false codec=LZ4_RAW`. Three-pair medians are not general latency bounds.

Local evidence: `artifacts/compression-20260908/report.json`, `device.log` and six paired Parquet files, all outside Arduino's disposable build directory. The host sanitizer suite separately passed both codecs on empty, one-row, 90×96 and 65,536×8 fixtures, plus incompressible raw blocks of every length 0–1,024, capacity rejection and injected codec/sink failures. Host fault tests do not simulate SD power cuts.

The final stored smoke rows reported filesystem total 31,441,764,352 bytes and used 802,816 bytes (cached, KiB-resolution health reports). This is not an allocation-unit measurement or a retention/lifetime test. Capacity projections and unimplemented object-storage synchronization are kept separately in the [offline design](../../shared/telemetry-pipeline.md#offline-capacity-and-reconnection).

## Board 1, Bluetooth LE sync

Verified **2026-09-16** with the pinned Arduino stack plus NimBLE-Arduino 2.5.1, firmware metadata `arduino-cores3-parquet-v4`, protocol v1 from the [BLE sync contract](../../shared/ble-sync-protocol.md). Client: this macOS host, bleak 3.0.2 via `pixi run ble-sync` (`tools/ble_sync.py`), launched from Terminal.app because macOS TCC aborts CoreBluetooth access from a process without a Bluetooth usage entitlement (measured: `Abort trap: 6` from the agent shell, success from Terminal). No phone was tested; the Android app is built against the same contract but is **not** bench-verified.

- Application SHA-256 `d1e5151fb3d3a33cb0f4dd65c51d7359df77cf40f740df2fb54173bc2289cc4b`, retained at `artifacts/firmware/cores3-parquet-v4-d1e5151f.bin`. Program storage **842,743** bytes (v3: 589,887), static internal RAM **39,372** bytes (v3: 26,996). Boot `b41d37a48c04031e9397e28ce48c2f43`; station UUID unchanged after flashing. Free internal heap reported in `status` during a connected session: 198,192–198,756 bytes.
- **Advertising and discovery:** `BLE BEGIN name=AQ-6b40 service=c0a5e9f0-0001-… mtu_max=517 bonds=0` at boot; the host scanner found exactly one device advertising the service UUID, RSSI −55 dBm, within an 8 s scan. Logging continued through BLE start (`PARQUET BEGIN` preceded it, `DIAG COMPLETE` followed).
- **Pairing:** first encrypted read triggered `BLE PAIR passkey=558355`, shown full-screen on the LCD and entered on the host; `BLE BONDED … bonded=true authenticated=true bonds=1`. MTU negotiated 23 → 517 (`BLE MTU mtu=517`; bleak reported 515 usable). Subsequent connections were silent (bonded) across four separate sessions.
- **`info`/`status` reads:** JSON matched the contract key-for-key; `dict` equalled the compiled dictionary SHA-256, `station`/`dev`/`boot` matched the serial `PARQUET BEGIN` line, `max_read` 16384.
- **LIST:** 766 entries streamed plus `LIST_END count=766 partials=0`, received count equal to the device count. USB `parquet list` on the same card listed the same set.
- **File transfer:** six files (three 14,080-byte LZ4 and three 28,537-byte uncompressed benchmark files) fetched with one or two 16 KiB `READ` windows each; per-file 0.75–1.39 s, **14.7–21.6 KB/s** payload rate, 12.9 s wall for LIST + 6 files. Every file passed length, CRC-32 against `OPENED.crc32`, PAR1 head/tail, and PyArrow/DuckDB identical readback; SHA-256 of each equalled the USB-serial readback of the same file and the hashes already recorded in the LZ4 section above (`afffb06d…` uncompressed, `16caeebf…` LZ4_RAW). Evidence: `artifacts/ble-sync-1/manifest.json`, `artifacts/ble-sync-1.log`, `artifacts/ble-sync-1-serial.log`.
- **Sampling during transfer:** `PARQUET ROW` lines around the connected session showed jitter 1,555 / 2,554 / 7,546 µs and `dropped=0`; `status` reported `drop=0 err=0 miss=0` before and after. This is a six-file, 13-second observation, not a full-card or long-soak measurement.
- **`live` and `status` notifications:** delivered every 10 s while connected, each under one PDU (`live` ≈ 210 bytes with a valid PMS frame). Keys absent for null values as specified.
- **SET_TIME over BLE:** `TIME_SET epoch_s=1789581931 monotonic_us=285464289`; `status` then showed `utc=1 gen=1`; the storage worker closed the 27 unsynced rows (`data_unsynced_…_0-26-0.parquet`, 18,745 bytes) at the epoch boundary as the contract requires, and did not rewrite them.
- **FLUSH over BLE:** `FLUSHED rows=2 files_finalized=2`; USB `parquet list` then showed `year=2026/month=09/day=16/data_1800_…_27-28-1.parquet` (9,427 bytes). USB `parquet status` and `parquet list` output was unchanged in format after the refactor.
- The battery millivolt values in `live` (16,372 → 23 mV with no battery attached) are the pre-existing unqualified `M5.Power` API report, unchanged by this work.

Not measured: full 766-file sync duration and energy, Wi-Fi coexistence (Wi-Fi stays off), notification loss under RF stress, bonded reconnect latency, behaviour with a second central attempting to connect, `.partial` handling over BLE (none existed), and any phone client.

**Phone client, 2026-09-16 (owner's report, not instrumented):** the Android app (`opensensor-space-android` commit `c557350`, debug APK SHA-256 `147803f2…`) was sideloaded onto the owner's Android phone and, in the owner's words, "connected to device and all works like a charm" against the v4 firmware above: pairing, live readings and file sync. No timing, file count or phone model was recorded, so this line is a report, not a measurement; the first instrumented phone-vs-board sync is still owed.

## Board 1, protocol v2: configuration, Wi-Fi, LAN sync, phone

Verified **2026-09-16/17**, firmware `arduino-cores3-parquet-v5`. Two builds were used: `83c844f0…` for the BLE configuration checks below, then `51ead18b7ddef2d31a0eeffb27ae8540fe3e7c5c0327855f01e9f4f6f82814c1` (retained at `artifacts/firmware/cores3-parquet-v5-51ead18b.bin`; only change: the worker heartbeat is stamped per frame sent, see the LAN section) for everything from Wi-Fi provisioning on; the phone's second round used `d5f1db40f5c2e091f04952537605e3398a6ca78c1168fe77468f63beb0aa3075` (BLE frames capped at 512 bytes, 4 s notify retry budget, LAN send failure drops the session) and finally `6144ea1d864cfdf2c5a4b77792ec8b81383953a524cd4409e8d22f03d4a0e412` (LAN session takeover; **1,408,487** program bytes, **78,628** static RAM), both retained under `artifacts/firmware/`. Program storage **1,407,435** bytes (v4: 842,743; +565 KB for Wi-Fi, lwIP, mDNS), static internal RAM **78,564** bytes (v4: 39,372; includes the 8 KiB log ring, an 8 KiB log-tail staging buffer and the LAN buffers). Storage worker stack raised 16 → 24 KiB after the high-water mark read **1,592 bytes free** during a light `GET_CONFIG`. Same host client as above, protocol v2 from the [sync contract](../../shared/ble-sync-protocol.md). Evidence: `artifacts/ble-v5-config.log` (host), `artifacts/ble-v5-config-serial.log` (device).

- **Boot:** `CONFIG LOADED first_boot=true display=true pair=random pin_default=true wifi_on=false ssid_set=false lan_on=true` — the Meshtastic-style default chose `random` because the CoreS3 display was detected; `BLE BEGIN … bonds=2 pair=random` (Mac and phone bonds survived the flash); `LAN BEGIN host=aq-6b40 port=47390` with the radio staying off. Logging and the LCD ran normally through it.
- **`GET_CONFIG` over BLE:** returned the CONFIG frame (230 bytes, one PDU) with the JSON documented in the contract, `reboot_required=false`, `mac` all zeros while the radio is off (expected; a real MAC is reported once the driver is up — note for the phone UI).
- **`GET_TOKEN` over BLE:** `TOKEN port=47390` + 32 bytes; serial logged `LAN TOKEN issued=ble`. Token kept in git-ignored `artifacts/lan-token.txt`, deliberately not recorded here.
- **`WIFI_SCAN` over BLE:** ran on the LAN task with the radio brought up temporarily (`WIFI SCAN BEGIN … END found=7 reported=7`), 7 networks with RSSI −51…−83 dBm, auth WPA2/WPA-WPA2, channels 1–11, delivered as `WIFI_AP` frames plus `WIFI_SCAN_END count=7`. Sampling continued (`PARQUET ROW … dropped=0` during the scan).
- **`LOG_TAIL` over BLE:** 1,500 requested bytes returned as `LOG` slices, `LOG_END total_bytes_since_boot=149889 returned=1500`, content identical to the serial capture for the same interval.
- **`SET_CONFIG` validation:** `ble.pin=12` refused with `ERROR op=0x09 code=12 detail="ble.pin"`, nothing stored (serial `BLE ERROR op=0x09 code=12`).
- **Regression caught on the bench:** the first v5 build wedged the storage worker on the first `GET_CONFIG` (`dropped=` climbing, serial `parquet status` unanswered, `WIFI_SCAN` still fine because it runs elsewhere). The task watchdog fired after 5 s with `CPU 0: parquet-sd`, and `addr2line` on the backtrace pointed at `respond()` — a mechanical `ble::send_response(` → `respond(` rewrite had turned the new dispatch helpers into infinite recursion, which the compiler folded into a one-instruction loop. Fixed; the worker now stamps a heartbeat and the main loop prints `PARQUET ERROR operation=worker-stall seconds=N state=… stack_free=…` if it stops moving.

### Wi-Fi provisioning and LAN transport (Mac client)

Host: this Mac over `pixi run ble-sync --lan …` (`tools/ble_sync.py`, TCP needs no Bluetooth entitlement, so it ran from the agent shell). Evidence: `artifacts/ble-v5-wifi.log`, `ble-v5-wifi-serial.log`, `lan-sync-1.log`, `lan-sync-1/manifest.json`, `lan-sync-1-serial.log`, `lan-sync-2.log`, `lan-sync-2/manifest.json`, `lan-sync-2-serial.log`.

- **Provisioning over BLE:** `SET_CONFIG wifi.ssid/wifi.psk/wifi.on=1` → `CONFIG SET … wifi_on=true ssid_set=true` → `WIFI CONNECT` → `WIFI CONNECTED … rssi=-42` → `LAN LISTEN port=47390 host=aq-6b40.local mdns=true service=_aqsync._tcp`, all within the 20 s before the follow-up `GET_CONFIG`, which then reported `state=connected`, the real IP, RSSI −34 and the STA MAC. While the radio is off `GET_CONFIG` reports `mac` as all zeros (phone UI should treat that as "unknown"). Re-provisioning to a second network over the LAN session itself also worked (the session drops, as documented).
- **Persistence:** after a reflash the board rebooted with `CONFIG LOADED first_boot=false … wifi_on=true ssid_set=true` and rejoined on its own; after `REBOOT` likewise. Settings survive flash and reboot; the UTC anchor does not (RAM-only by contract, `utc=0` after reboot).
- **mDNS:** `dns-sd -B _aqsync._tcp local.` listed instance `aq-6b40`; `dns-sd -G v4 aq-6b40.local` resolved to the board's address on both networks used (a phone hotspot, then the owner's home Wi-Fi).
- **Latency:** ICMP RTT to the board 63–213 ms (avg 118–155 ms) on both networks — the cost of `WiFi.setSleep(true)` modem sleep, kept on deliberately for BLE coexistence.
- **Handshake and auth:** correct token → framed `HELLO proto=2 payload_max=1024` + info JSON identical to the BLE `info`; wrong token → framed `ERROR op=0 code=14` and close; `GET_TOKEN` over LAN → `ERROR code=13 detail="ble-only"`; a second TCP client during a session → `ERROR op=0 code=7 detail="session-active"` (serial `LAN REFUSE … reason=busy`) — behaviour replaced by the token takeover in `6144ea1d`, see the phone's second round.
- **LIST over LAN:** 774 then 776 entries, `LIST_END count` equal to received. Streaming 774 small frames over the hotspot took > 5 s inside one command and tripped the 5 s worker-stall detector (`state=blocked`) — a false alarm, fixed in `51ead18b` by stamping the heartbeat on every frame; zero stall lines in the 40-file run afterwards.
- **File transfer:** run 1 (hotspot) 6 files / 127,851 bytes in 16.5 s wall incl. LIST, per-file **5.6–39.4 KB/s**; run 2 (hotspot) 40 files / 1,377,920 bytes in **34.4 s** wall incl. LIST, per-file **19.2–61.8 KB/s**, 40.1 KB/s overall. Every file passed length, CRC-32, PAR1 and PyArrow/DuckDB identical readback; the benchmark files' SHA-256 (`16caeebf…`, `afffb06d…`) equal the USB and BLE copies. Both runs pulled the same bytes the card holds; nothing on the device changed.
- **Sampling with BLE + Wi-Fi + TCP active:** `PARQUET ROW` jitter **1.5–17.7 ms** (BLE-only session: ≤ 7.5 ms), `dropped=0`, `miss=0` throughout. Free internal heap in `status` with Wi-Fi connected and a LAN session up: **82,072 bytes** (v4, BLE only: ≈198 KB) — Wi-Fi costs ≈116 KB of heap; still ample, but this is the number to watch.
- **`REBOOT` over LAN:** `REBOOTING delay_ms=500`; the worker first finalized the RAM batch (`PARQUET READY … rows=10`), then `rst:0xc (RTC_SW_CPU_RST)`; Wi-Fi rejoined, LAN server back.

### Phone over LAN and BLE (OnePlus 7 Pro, Android 16)

Android app commit `a993268` (debug APK SHA-256 `808c4e419b8344fb…`) on a **OnePlus 7 Pro (GM1911), Android 16, API 36**, driven over adb (USB, then Wi-Fi debugging). Phone, Mac and board on the owner's home Wi-Fi. Evidence: `/tmp/aq-phone-*.png` screenshots (not retained in the repo), phone logcat excerpts quoted here, `artifacts/phone-v5-serial.log`, `artifacts/phone-v5-lan-serial.log`.

- **BLE:** silent reconnect to the bonded board, live and status documents on Today within 10 s, µm label correct. The app fetched the LAN token on its own (`BLE CMD op=0x0d` → `LAN TOKEN issued=ble`), then `GET_CONFIG` and `LIST` (`op=0x08`, `0x01`).
- **LAN selection:** on the next connect, with the station discovered by `NsdManager` and a token stored, the app chose TCP: `TcpTransport: ready: 192.168.100.174:47390 payload_max=1024 proto=2`, chip "Wi-Fi LAN". API 36 does not enforce `ACCESS_LOCAL_NETWORK`; that path is still unverified (needs an Android 17 device).
- **Sync over LAN:** "Sync all · 776 · 30.20 MiB" ran to **176 files / 6.27 MiB in ≈4 min (≈26 KB/s wall)**, every file "synced · verified", before the session died. Not a full-card run yet.
- **Why it died — Android, not the link:** twice the phone kernel destroyed the app's socket and `TcpTransport` logged `link lost: socket error: Software caused connection abort` within 6 ms: at `00:26:35.890` (`InetDiagMessage: Destroyed live tcp sockets for uids={10393}`, our uid; screen had gone off at 00:24:24, keyguard up) and at `00:31:53.645`, one second after the owner opened the Wireless-debugging settings page (`AdbWirelessDebuggingFragment onEnabled` at 00:31:52). The device side saw `LAN DISCONNECT reason=recv-error`. A foreground-only app loses LAN sockets the moment it stops being foreground on this Android 16 build; the app reconnected on its own within ~80 s in the first case. Consequence recorded as a rule in the Android repo: transfers run under a `dataSync` foreground service.

- **BLE with Wi-Fi active (coexistence), Mac client:** 12 files / 202,744 bytes in 99.5 s wall, per-file **0.7–10.2 KB/s** (BLE alone on v4: 14.7–21.6 KB/s), every offset check passing, all files verified. Evidence `artifacts/ble-v5-sync-coex.log`, `ble-v5-sync-coex/manifest.json`. Wi-Fi on costs BLE roughly a factor 2–20 in throughput on this board with modem sleep; LAN is the fast path once provisioned.
- **BLE with Wi-Fi active, phone:** the OnePlus lost runs of **16 and 18 consecutive CHUNK notifications** at the start of a READ window (`gap in stream: expected offset 32768, got CHUNK at 41894`, reproducible on the same file twice; 41894 − 32768 = 18 × 507), while the Mac read the same windows cleanly. Phone logcat shows the Qualcomm BT HAL cycling in-band sleep (`IBS_SLEEP_IND`/`WAKE_IND`) at that instant. The device is not at fault (it would have answered `READ_END status=7` had `notify` failed); the app aborted the whole sync instead of re-reading the window as the contract prescribes. Fix queued in the Android repo (retry the window from the last good offset; 4 KiB windows over BLE). A late `CLOSED` frame after the abort also surfaced as "unexpected Closed frame in response to LIST" — same fix (drain to the terminal frame before the next request).
- **Token rotation:** `SET_CONFIG lan.rotate_token=1` from the Mac over LAN → the old token is refused (`code=14`) and the phone's next connect logged `LAN connect failed (AUTH_FAILED); falling back to Bluetooth`, BLE ready 1.5 s later — but the app did **not** re-fetch the token (no `GET_TOKEN` on serial), so LAN would stay dead until reinstall. Fix queued in the Android repo (re-fetch on `AUTH_FAILED`).
- `adb run-as` is refused for this app on the OnePlus's Android 16 build (`No match for app … targetSdkVersion=37:fromRunAs`), so the phone archive could not be hashed from the host; the app's own CRC-32/PAR1 verification is the evidence for those files.

### Phone, second round: 512-byte BLE cap, foreground service, full card, LAN takeover

Same phone and network, 2026-09-17 00:10–02:10 local (UTC+3). Firmware `d5f1db40` then `6144ea1d`; Android app commits `7c3720b` (dataSync foreground service, reconnect, token renew), `3395288` (window-loss resume), `defe42a` (auto-sync; APK SHA-256 `7d6a3141e530c637…`), `2f4c994` (WifiLock, resume after transport failure, transport chip, REBOOT audit; APK `2fb6b9b418e4b5b6…`, installed 02:02, not yet exercised). Evidence: `artifacts/phone-fullsync-1-serial.log`, `phone-fullsync-2-serial.log`, `lan-takeover-1-serial.log`, phone logcat quoted here.

- **Root cause of the BLE "gap in stream":** not the Qualcomm sleep cycling blamed above. v5 sent 514-byte `CHUNK` frames (`payload_max = MTU − 3`), and Android silently drops GATT notifications above the 512-byte attribute limit while macOS delivers them, which is why the gap was deterministic on the phone and absent on the Mac. `d5f1db40` caps every BLE frame at 512 bytes; the file that had failed twice then synced over BLE on the first try. BLE throughput on the phone with Wi-Fi active: ≈**2 KB/s** (Mac in the same state 0.7–10 KB/s).
- **Full card over LAN, foreground service:** with `7c3720b`+ the sync survived screen-off (`ConnectivityService` no longer destroyed the sockets while the `dataSync` service ran). All 776 files reached "Synced · verified" across runs of 176, 305 and 158 files (≈4, ≈6 and ≈2.5 min; **45–63 files/min, 26–40 KB/s** wall, files average 40 KB); a further "Sync all" at 01:54 found nothing to do and its foreground service stopped after 3 s. Not a single-run number: the runs were interrupted by (a) an `OPENED` reply the device dropped after its 5 s send timeout while the phone's Wi-Fi power-saved — the phone waited 60 s for a frame that never came; `d5f1db40` now drops the session on any send failure (`LAN SEND FAILED … action=drop-session`, timeout 15 s) so the phone sees EOF and resumes, and (b) the owner tapping "Reboot device" mid-sync at 01:46 (`REBOOT` on serial: batch flushed, `RTC_SW_CPU_RST`, app reconnected). Neither interruption lost data; a later run picked up the missing files. The `WindowLostException` path (re-read a lost READ window instead of aborting) was exercised by (a).
- **Auto-sync worker (WorkManager, `defe42a`):** "Sync now in background" while the app was open → `SyncWorker: auto-sync AQ-6b40: skipped, the app has an interactive session with this device` (the guard works). Two minutes later, app in the background → `auto-sync AQ-6b40 over LAN: connect failed` and a BLE fallback that finished in 9 s (`0 new file(s)`). Cause: Android had destroyed the app's interactive socket without a FIN, the device still held that half-open session and refused the newcomer with code 7 — the busy rule would have locked the phone out for up to the 300 s idle limit.
- **LAN session takeover (`6144ea1d`):** a second connection is now held aside for the 5 s handshake window and a valid token wins the slot. Measured from the Mac while the phone's session was live: `LAN CHALLENGER peer=…148 result=waiting-for-token` → `LAN DISCONNECT reason=preempted authenticated=true` → `LAN TAKEOVER peer=…148` → `LAN AUTH result=ok session=2`; the `status` command answered; after the Mac closed, the phone reconnected on its own (`session=3`) within seconds. Wrong token from a challenger: `ERROR op=0 code=14`, `result=auth-rejected`, phone session untouched. Silent challenger: `result=handshake-timeout` after 5 s. Contract updated (newest token holder wins; two auto-reconnecting phones would fight over one device).
- **WifiLock, source-checked not measured:** AOSP `WifiLockManager` keeps a `LOW_LATENCY` lock active only with the screen on and the app in the foreground, and API 34+ converts `HIGH_PERF` to `LOW_LATENCY`. On Android 14+ no lock keeps Wi-Fi out of power save for a screen-off sync; the app holds `FULL_HIGH_PERF` for LAN transfers anyway (helps API 31–33) and relies on reconnect-and-resume otherwise.
- Wireless adb drops whenever the phone sleeps; re-pairing was needed twice. Evidence from the phone side is therefore logcat excerpts, not continuous logs.

Not yet measured for v2: a **single uninterrupted full-card run** with timing, the `2f4c994` fixes on hardware (WifiLock log lines, resume after a transport failure, transport chip without restart), a scheduled 15-min auto-sync run with the app closed, `ACCESS_LOCAL_NETWORK` prompt on API 37, `ble.clear_bonds`, `lan.rotate_token` end to end from the phone, `fixed`/`none` pairing (only the `random` default has run; a display-less board is needed for the real case), long-soak coexistence, and energy.

### Phone, third round: "no data today" audit, derive loop found (2026-09-18)

Same phone (OnePlus 7 Pro, Android 16, Wi-Fi adb `192.168.100.173:37631`) and board (`AQ-6b40`, station `53315f5f…`, LAN `192.168.100.174:47390`, proto 2), 02:07–02:20 local (UTC+3), Android app at commit `c2fa5fa` (the build then on the phone). Evidence: phone logcat quoted here, board `parquet status` / `parquet list` over USB serial (`/dev/cu.usbmodem20301`, read-only commands via `tools/parquet_device.py command`).

- **Report:** "the app shows nothing for today" at 02:13 local. **Finding:** the calendar, not the pipeline. The app's Day is the local day, which began at 21:00Z. `parquet list` (856 files, 81 dated: 16 on day=08, 13 on day=16, 52 on day=17) shows the previous boot `95fa1006…` ending with its `1630_…_1254-1433-9` window (16:30–16:45Z = 19:30–19:45 local) and the current boot `cbde222f…` holding exactly two files — `unsynced/…_0-13-0` (14 rows before the clock) and `day=17/…2300_cbde222f…_14-33-1.parquet` (20 rows, 23:11–23:15Z) — so **the board did not log between 19:45 and 02:09 local** (off, or reset; cause not established) and had 3.5 minutes of dated data for the new day. `parquet status`: `interval_s=900 buffered=6 open_rows=0 finalized=2 dropped=0 errors=0`. The phone's last syncs were 02:07:15, 02:11:24 and 02:11:55 (LAN, Wi-Fi lock held ≈ 1 s each), all before that file was finalized at 02:15:00; the 02:14:03 auto-sync run was skipped by the interactive-session guard (correct); the LAN link dropped at 02:14:13 (`Software caused connection abort`) and auto-reconnected in 5 s. Nothing wrong end to end — but see the next line.
- **Bug found from the same log:** `HistoryDeriver: derived 3 of 3 station-days (6393 rows)` on **every** pass (02:07:11, :17, :23, 02:11:26, :56 — after each screen open and each sync), where an unchanged day should cost one signature compare. Root cause: `DerivedDay` has an auto-generated id plus a unique `(station, epochDay)` index; Room's `@Upsert` handles the unique conflict with `UPDATE … WHERE id = ?` on the entity's id (0 for a new object), so the stored signature was never refreshed after a day's first build. Same entity shape on `archived_file`. Fixed in the Android repo (`INSERT OR REPLACE`), together with the Today card using the UTC day (at 02:00 local it still showed yesterday), the History span frozen at the day the screen was opened, and a Day re-read on every Room write. **Fix observed on the phone** (app `2c1c96c`, APK SHA-256 `4f07e7dee982ad64…`, installed 02:34): first pass after install `derived 3 of 3` (stale signatures from the old build replaced once), then a LAN sync at 02:35 that added the `2300_cbde222f…` file and re-dated one unsynced file (`estimated time for 1 files`) logged `derived 1 of 3 station-days (5180 rows)` — only the day that changed. The same sync hit the known 15 s `READ` no-response over LAN (`sync aborted by the transport`, resumed by a second run at 02:35:54), unrelated to the app change. Not yet observed: the Chart-by-file card and the hourly Day view under a human eye, and a quiet pass logging `derived 0 of N` (debug level, added after this run).

## Board 1, schema v3: row groups, statistics, TIMESTAMP (firmware v6)

2026-09-17 12:41–13:3x local (UTC+3), board `AQ-6b40` (MAC `44:1b:f6:e2:6b:40`, ESP32-S3 rev 0.2, 16 MB). Pre-flash checks per `AGENTS.md`: `chip`, `flash-id` (16 MB), `efuse` (no flash encryption, no secure boot, download mode enabled), backup `backup/cores3-flash-20260906T102150Z.bin` = 16,777,216 bytes. Firmware `arduino-cores3-parquet-v6`, binary SHA-256 `409987b4885f8d5c2c5f29717f2beb58278ad28026a2d71af910d72423c2b06b`, retained at `artifacts/firmware/cores3-parquet-v6-409987b4.bin`; 1,413,040-byte image, hash verified by esptool; 1,412,899 program bytes / 86,828 static RAM (Arduino report). Logs: `artifacts/v6-boot.log`, `v6-1800-first.log`, `v6-1800-full.log`. Host gates before flashing: `parquet-test --sanitize` (12 fixtures), `telemetry-contract-test --sanitize`, `fmt-check`, `lint`, `test_ble_sync.py`.

| Check | Result |
| --- | --- |
| Boot | `PARQUET BEGIN schema=cores3-telemetry-v3 columns=77 … max_row_groups=8 psram_workspace_bytes=172880 row_bytes=696`; SD mounted, station UUID `53315f5f-…` retained, all 776 earlier files listed, `PARTIAL retained=0` |
| `parquet interval 1800` with 1 unsynced row buffered | `GROUP … ordinal=0 rows=1` then `READY … rows=1 row_groups=1` (10,335 bytes, `write_us=94225`, `sync_us=22169`), `CONFIG interval_s=1800`; the epoch change on `sync-time` finalized the next unsynced rows the same way (`finalized=2`) |
| Status | serial `buffered=1 open_rows=0 open_groups=0`; the JSON gains `open`/`open_rg` |
| First dated file, window 09:30–10:00Z (time set at 09:42:30Z) | `GROUP ordinal=0 rows=90` (`writer_us=64299`, 36,989 bytes on card), `GROUP ordinal=1 rows=15` (`writer_us=11264`) cut by the 10:00Z boundary, `READY … data_0930_495ccef9…_2-106-2.parquet rows=105 row_groups=2 bytes=58313 crc32=c7443725 sync_us=38335 writer_us=95455 heap_free=73260 psram_free=8172364 stack_free=8480 heap_min=23956`; `write_us=160205866` is file-open (first group) to finalize, i.e. the 150 s the second batch took to arrive |
| Fetched copy | SHA-256 `1a06e93b6c25d638bf95407224970dbcbb6d19897052c53476e3de249418f3cf`; serial CRC matched; `tools/inspect_parquet.py` PASS: 2 row groups, footer 13,585 bytes (23.3 % of this short file), `column_orders` TYPE_ORDER × 77, `sorting_columns=[sequence asc]` per group, no deprecated `min`/`max`, `nan_count=0` on FLOAT chunks, `event_time_utc_ns` read as `timestamp[ns, tz=UTC]` by PyArrow 25.0.0 and `TIMESTAMP WITH TIME ZONE` by DuckDB 1.5.5; group 0 stats `sequence 2–91`, UTC `09:42:35.334–09:57:25.339Z`, PM2.5 `11–40`; group 1 `sequence 92–106`, UTC `09:57:35.333–09:59:55.332Z`, PM2.5 `38–44`; both readers agree on all 105 × 77 values/nulls; sequence strictly increasing, cadence exactly 10 s |
| Full window 10:00–10:30Z | `GROUP ordinal=0 rows=90` at 10:15Z (`writer_us=65292`), `GROUP ordinal=1 rows=90` at 10:30Z (`writer_us=80850`), `READY … data_1000_495ccef9…_107-286-3.parquet rows=180 row_groups=2 bytes=87654 crc32=4c8cbc19 sync_us=37399 writer_us=170756 heap_free=72936 stack_free=8308`; fetched copy SHA-256 `a8ceaebea9f6f3a2d12c183ae5d3c683b2902c1d10066962cbfebc458fef9d7f`, `inspect_parquet.py` PASS. **Footer 13,672 bytes = 15.6 %** of the file; 487 bytes/row uncompressed versus 466 for a v5 90-row file (41,986 bytes, 17 % footer): the statistics cost ≈ +1.5 KB per group, the two groups amortise only the schema/key-value part, as the host estimate predicted. Group statistics: `sequence 107–196 / 197–286`, UTC `10:00:05.337–10:14:55.337Z / 10:15:05.330–10:29:55.343Z`, PM2.5 `41–51 / 46–63` |
| Phone, unprompted | During that window the Android app's auto-sync connected over LAN (`LAN CONNECT peer=…173`, `AUTH ok session=2`), listed, opened the two-group v3 file (`BLE OPEN handle=6 bytes=58313 crc32=c7443725`), read it in 4 × 16,384-byte windows, `CLOSE`d, listed again and then the phone dropped the socket (`LAN SEND FAILED errno=104 … drop-session`, the known Wi-Fi power-save pattern). Unchanged app (`2f4c994`) accepts v3 files; `heap_min` 23,956 → 18,028 coincides with this session, not with the writer |
| Codec/interval switches with an open window | `parquet codec lz4` at 10:31Z finalised 5 pending rows as a one-group file; `parquet interval 3600` finalised the single LZ4 row buffered since (`bytes=10714`, one group); `finalized=6 dropped=0 errors=0` afterwards |
| Host tool impact | `parquet_device.py validate_file` failed on the annotated column (`duckdb` needs `pytz` to build TIMESTAMPTZ Python objects); fixed to compare the UTC fields as INT64 nanoseconds on both sides (`epoch_ns()`), which is also the exact comparison |

**Failure found by the run, 2026-09-17 10:47–12:58Z (v6 defect, fixed in v6.1).** After switching to LZ4_RAW + 3600 s, the first compressed row group at ~10:47Z failed and the worker entered its documented failed state: `parquet status` at 12:58Z read `buffered=90 open_rows=0 open_groups=0 finalized=6 dropped=767 errors=1 failed=true codec=LZ4_RAW`. Root cause, from the source: `create_file()` handed `Writer::begin()` the address of a stack-local `Compression` struct, and the writer kept that pointer for `row_group()` minutes later — a dangling pointer that only the LZ4 path dereferences (uncompressed passes `nullptr`), and that the one-shot `write_parquet()` of v5 never exposed. The 1-row LZ4 file written at 10:32Z by the interval command happened to succeed because its group followed `begin()` immediately. The original `PARQUET ERROR operation=row-group` line had scrolled out of the 8 KiB log ring by the time it was noticed; the ring did show the Mac and the phone alternately taking over the LAN session (`LAN TAKEOVER` × 6), as the protocol predicts for two auto-reconnecting clients. **Data lost: the 90 rows in RAM plus 767 dropped rows (≈ 2 h 20 min of a test run)**; the `.partial` of the failed file is retained and reported at boot (`PARQUET PARTIAL retained=1`). Fix (`arduino-cores3-parquet-v6.1`, binary SHA-256 `1dadcad91e58cd594d69b6a745c6f42d93702b02fb025a67d60ec2dfc4834724`, retained as `artifacts/firmware/cores3-parquet-v6.1-1dadcad9.bin`, 1,413,003 program bytes / 86,828 static RAM): the writer copies the `Compression` struct by value in `begin()`; the host fixture now clobbers the caller's copy right after `begin()`, which makes ASan report `stack-use-after-scope` on the v6 writer and passes on v6.1. Lesson for this record: the host suite proved the format, not the call pattern the firmware uses; any writer API that keeps caller state across calls needs a lifetime test.

Notes. `heap_min` (minimum free internal heap since boot) went 58,408 → 23,956 → 18,028 across the session while `heap_free` stayed at 73 KB; the drops coincide with the BLE/Wi-Fi stacks starting and with the phone's LAN sync, and v5 showed the same range — not attributed to the writer (PSRAM workspace, +2 × 4 KiB internal stdio staging). After the v6.1 flash (13:00Z) the board was set to **1800 s / LZ4_RAW with host UTC** to reproduce the failing configuration. Window 13:00–13:30Z: `GROUP ordinal=0 rows=90 bytes=12769 writer_us=56254 codec_us=5457`, `GROUP ordinal=1 rows=80 … writer_us=49465 codec_us=5425`, `READY … data_1300_95fa1006…_4-173-1.parquet rows=170 row_groups=2 bytes=38129 crc32=bcb9ab99 sync_us=41468 codec_us=10882 heap_free=72788 stack_free=9536`, no `PARQUET ERROR` in the 30-minute capture (`artifacts/v6.1-lz4-1800.log`). Fetched copy SHA-256 `25f83baf1f235c5abe58179417253b406120b764910a4a9fdf3e5eba16a94fea`, `inspect_parquet.py` PASS (group 0 uncompressed 36,948 → 12,765 bytes, group 1 33,068 → 11,774; UTC `13:01:44.570–13:16:34.571Z / 13:16:44.576–13:29:54.572Z`). **224 bytes/row compressed versus 487 uncompressed** for the same 77-column rows; the 13,586-byte footer is then **36 %** of the file, which is the strongest argument yet for the phone's daily merge and for keeping per-chunk metadata lean. The board is left at 1800 s / LZ4_RAW (on this firmware, before #25, a reboot returned to 900 s / UNCOMPRESSED). Not measured yet: a four-group (3600 s) file, a hard reset between row groups (footer-less `.partial`), `FLUSH` over BLE with `open_rows > 0`.

## Board 1, RTC hand-off: host anchor → BM8563 → next boot (firmware v6.2)

2026-09-18 06:58–07:08Z, board `AQ-6b40` (same board, backup and eFuse state as above). Before flashing: `parquet status` read `buffered=14 … finalized=34 dropped=157` on the running v6.1 (900 s / UNCOMPRESSED after an earlier reboot; a phone sync was in progress over BLE+LAN), `parquet flush` finalised those rows (`data_0700_cbde222f…_2824-2838-34.parquet`, 15 rows, 16,359 bytes) so nothing in RAM was lost. Firmware `arduino-cores3-parquet-v6.2`, image SHA-256 `ad72b96ff8b03475e54f4b188b5e999020252418b8b1b284fa69fbe72d664083`, retained at `artifacts/firmware/cores3-parquet-v6.2-ad72b96f.bin`; 1,415,975 program bytes / 86,852 static RAM; esptool "Hash of data verified". Host gates: `telemetry-contract-test --sanitize`, `parquet-test --sanitize`, `fmt-check`, `lint`.

| Check | Result |
| --- | --- |
| First boot on v6.2, chip never written | `DIAG rtc enabled=false_or_read_failed` (as every boot since 2026-09-08), then `PARQUET RTC state=unusable reason=voltage-low`; the boot started unsynced exactly as v6.1 would. Confirms the standing "RTC read failed" was an unset chip (VL flag set), not a bus fault |
| Host sync writes the RTC | `parquet-device sync-time`: `PARQUET TIME epoch_s=1789715023 source=host monotonic_us=41302342`, then `PARQUET RTC write=ok utc=2026-09-18T07:03:44Z late_ms=0` (read from the device's log ring over LAN — the sync helper stops at the TIME line, so the write line is not in its output). `late_ms=0`: the main task hit the anchor's second boundary on the first 20 ms loop pass. LAN `status`: `"utc":1,"gen":1,"clk":1,"rtc":1` |
| Software reboot, RTC restored | `REBOOT` over LAN → `rst:0xc (RTC_SW_CPU_RST)`, `DIAG rtc enabled=true date=2026-09-18 time=07:05:33`, **`PARQUET TIME epoch_s=1789715134 source=rtc monotonic_us=1774264`** before `PARQUET BEGIN`, i.e. the first sample of the boot was already dated (1.77 s after reset) |
| Rows and file of the restored boot | Epoch change on the next host sync finalised them: `data_0700_1d3590e1…_0-2-0.parquet` under `year=2026/month=09/day=18/` (dated tree, not `unsynced/`), 3 rows, 11,437 bytes, fetched over LAN, SHA-256 `e0d6ebc5c7470af5828fdbac936bbf3a11a827507929c2504afd1ff6c13f57b7`, `readers_match=true`, `inspect_parquet.py` PASS. Columns: `clock_status=[2,2,2]`, `clock_epoch=[1,1,1]`, `event_time_utc_ns` `07:05:44.005 / 07:05:54.019 / 07:06:04.016Z`, `clock_anchor_utc_ns=07:05:34Z`, `clock_anchor_mono_us=1774264`, **`rtc_read_ok=1`, `rtc_date_yyyymmdd=20260918`, `rtc_time_hhmmss=070544/070554/070604`** (first non-null RTC columns on this board; they agree with `event_time_utc_ns` to the second). Footer `clock` metadata carries the new code-2 text, `firmware=arduino-cores3-parquet-v6.2` |
| Skew of the restored clock | Second `sync-time` 2 min 39 s after the boot: `PARQUET TIME epoch_s=1789715173 source=host`, **`PARQUET CLOCK transport=serial previous=rtc skew_ms=-513`**, `PARQUET RTC write=ok utc=2026-09-18T07:06:13Z late_ms=20`; status `"gen":2,"clk":1,"rtc":1`. −513 ms is inside the ±1 s that two whole-second sources allow (host sends truncated seconds; the boot reads the RTC at an arbitrary sub-second), so it bounds the resolution, not drift. A drift number needs hours to days between the RTC write and the next host sync |
| Phone | Unchanged app (`2c1c96c`) kept syncing across the reboot; it ignores the new `clk`/`rtc` keys (`ignoreUnknownKeys`). Its "Set time" offer now never appears on this board (it keys on `utc == 0`), which is the harmony gap recorded in the protocol doc |

Not measured: retention across a **full power-off** (battery out / long unplugged — whether the BM8563 has backup power on the CoreS3 is still unknown), RTC drift over hours/days, `late_ms` when the display is redrawing at the write instant, and the out-of-range/invalid-calendar branches (the chip was never in those states here). The board is left on v6.2 at 900 s / UNCOMPRESSED (the boot default before #25) with host UTC (gen 2), RTC written.

## Board 1, advertising payload wakes the phone (firmware v6.3, protocol 2.1)

2026-09-18 11:01–11:08Z (14:01–14:08 local, UTC+3), board `AQ-6b40` on `/dev/cu.usbmodem20301`, same backup and eFuse state as above. Before flashing, the running v6.2 read `buffered=7 … finalized=17`; `parquet flush` over serial finalised them first (`data_1100_1d3590e1…_1406-1413-17.parquet`, 8 rows), so the reflash lost no rows. Firmware image SHA-256 `74d4a172059d12a8…`, retained at `firmware/esp-idf-cores3/artifacts/firmware/cores3-parquet-v6.3-74d4a172.bin` (1,417,339 program bytes / 86,868 static RAM). Phone: **OnePlus 7 Pro (GM1911), Android 16 / API 36**, app commit `678c1c0` (docs `e0af5e4`), debug APK SHA-256 `3c454c7416ab073b…`, installed over Wi-Fi adb (`192.168.100.173:40091`). Evidence: `artifacts/adv-v6.3/{boot,wifi,wake}-serial.log`, phone logcat quoted here. Design: [background-sync-triggers](../../shared/background-sync-triggers.md); contract: [advertising payload](../../shared/ble-sync-protocol.md#advertising-payload-v21).

| Check | Result |
| --- | --- |
| Boot on v6.3 | `BLE BEGIN name=AQ-6b40 … bonds=2 pair=random adv_ver=1 boot16=bcb4`, then **`BLE ADV flags=0x24 fin=0`** = `sd` + `clk_restored` (the RTC-dated boot from v6.2, `PARQUET TIME … source=rtc` 13.7 s after reset). No advertising error; the 31-byte ADV and the 27-byte scan response were accepted by the controller. |
| Phone still finds the device with the UUID list in the scan response | App opened (Scan screen `BleScanner: startScan` → `stopScan` 1.5 s later), device chosen, and — because `_aqsync._tcp` resolved — `TcpTransport: ready: 192.168.100.174:47390 payload_max=1024 proto=2` at 11:04:23, `LIST`/`OPEN`/`READ`×2/`CLOSE` on serial: the flushed 8-row file was pulled. |
| Trigger registration (process start) | `SyncTriggers: Wi-Fi arrival trigger registered`; **`AdvertFlagScan: flag scan registered for 1 device(s), 2 filters`** — the OnePlus controller reports offloaded filtering, so the hardware wake is available on this phone; `AutoSyncScheduler: auto-sync on: periodic work registered`. The Wi-Fi callback fired immediately at both process starts ("Wi-Fi available") and was throttled the second time (39 s), as designed. |
| **Device-flagged wake, app process dead** | `HOME`, `am kill` (not force-stop, which blocks broadcasts), `pidof` empty. `parquet flush` over serial at **14:06:55** → `PARQUET READY …_0-29-0.parquet` (30 rows), `fin` 0 → 1, `new_files` set. Phone logcat: **14:07:14 `ActivityManager: Start proc … for broadcast {…AdvertScanReceiver}`** — the controller's FIRST_MATCH woke the app 19 s after the file closed (low-power scan duty cycle, no app involvement) — `AdvertFlagScan: AQ-6b40: advertisement bcb4:1:2 new_files=true no_utc=false lan=true; enqueueing a sync`, `SyncWorker: auto-sync run: trigger=ADVERTISEMENT`, `LanDiscovery: browsing`, `TcpTransport: ready` at 14:07:16, **`auto-sync AQ-6b40 over LAN: 1 new file(s), 22498 B, 0 failed`** at 14:07:23, `Worker result SUCCESS`. **28 s from file close to verified copy on the phone**, with the app closed and the phone idle. |
| Device side of the same run | `wake-serial.log`: `LAN CONNECT peer=…173`, `LAN AUTH result=ok session=4`, `LAN CMD op=0x01` (LIST), then **`BLE ADV flags=0x2c fin=1`** — `new_files` cleared by the LIST, `sd`+`lan`+`clk_restored` remain — then OPEN/READ/READ/CLOSE, a second LIST, and the known `LAN DISCONNECT reason=dropped` when the phone's socket went away. The bit re-arms on the next `PARQUET READY`. |
| `setForeground` in the worker | Still **refused** (`ForegroundServiceStartNotAllowedException`, plain job) — expected: no Companion Device association exists yet on this phone, so none of the `REQUEST_COMPANION_*` exemptions apply. The transfer completed anyway (a running job has network for its duration). |
| Pre-existing, not new | One LAN `LIST` aborted with `BLE LIST ABORT after=16 reason=notify-refused` → `LAN DISCONNECT reason=dropped` during the interactive session at 11:04, then the phone reconnected (`session=2`, `3`) — the Wi-Fi power-save drop pattern recorded on 2026-09-17, unrelated to advertising. |

Not measured: the Companion Device Manager path (association dialog, `EVENT_BLE_APPEARED`, the power-save / background-data exemption against the screen-off socket kill, `setForeground` no longer refused) — the dialog needs the owner's tap; a Wi-Fi-arrival run observed end to end (both firings here were at process start and the second was throttled); `no_utc` as a wake reason (this board now always boots RTC-dated); wake latency distribution over more than one event; the flag scan surviving a Bluetooth toggle (the periodic worker re-registers it, untested); phone battery over a day with the scan registered.

## Measured facts that changed how we work

- **A wedged worker is invisible unless something watches it.** In v5 the storage worker spun in a tight loop while sampling, display, BLE callbacks and the LAN task all kept working, so the only symptom was `dropped=` creeping up in `PARQUET ROW` lines. The worker now publishes a heartbeat and the main loop reports a stall with the task state and stack headroom. Also: `eTaskGetState` from the other core reports a spinning task as `ready`, not `running`; the task watchdog's "Tasks currently running: CPU 0: parquet-sd" plus `xtensa-esp-elf-addr2line -pfiaC -e build/bringup.ino.elf <addresses>` is what actually named the function.
- **Android kills a backgrounded app's TCP sockets; BLE does not save you there either.** Two LAN syncs died within milliseconds of `ConnectivityService` destroying the app's sockets — once two minutes after screen-off, once one second after the owner opened another settings screen. The link, the board and the protocol were fine (`LAN DISCONNECT reason=recv-error` on the device). Any transfer that outlives a glance at the phone must run in a `dataSync` foreground service with a notification, and the transport must reconnect on its own.
- **GATT notifications above 512 bytes vanish on Android.** The MTU negotiation said 517, macOS delivered 514-byte frames, Android dropped them without any error on either side. Cap BLE frames at 512 regardless of MTU, and treat a deterministic "works on the Mac, fails on the phone" gap as a size problem before blaming the radio.
- **The device can wake the phone; the phone cannot be woken over the LAN.** Android starts a dead app process from an offloaded BLE scan filter (`ActivityManager: Start proc … for broadcast {…AdvertScanReceiver}`, 19 s after the board flipped `new_files` in its advertisement), and nothing on Android does the same for an inbound TCP/UDP packet. So the advertisement carries the "something changed" bits and mDNS/LAN carries the bytes: file close → copy on the phone in 28 s with the app closed (firmware v6.3, 2026-09-18), versus up to a 15-minute period plus Doze before.
- **A one-client server needs a way for the rightful client to get back in.** Android destroys a backgrounded app's sockets without a FIN, so the device kept a half-open session and refused the same phone's next connection until the idle timeout. The fix is authentication-based takeover (newest valid token wins), not a shorter idle limit — an idle interactive session is legitimate.
- **Never let a bulk search-and-replace touch the helper it is introducing.** The recursion above came from one Python script that first inserted `respond()` (calling `ble::send_response`) and then replaced every `ble::send_response(` in the file. Re-read the diff of any mechanical edit before building; cppcheck and `-Wall` did not flag the infinite recursion.

- **The USB hex transfer shares the line with live logging, and very rarely loses a byte.** Exporting all 764 SD files on 2026-09-16 aborted once, at file 160, on a single truncated `PARQUET DATA offset=768 hex=…` line while the sampling loop was also printing `PARQUET ROW`. The strict parser correctly refused the line rather than guessing. `tools/export_parquet.py` now retries a file up to `--retries` times and resumes from already-published, re-validated local copies; the second pass completed 764 files / 66,987 rows with 159 resumed and 605 fresh, every file passing PyArrow/DuckDB identical readback (that copy is the owner's, kept outside repository evidence). Treat one malformed line as a retry, not as data loss, and never relax the parser to "accept" it.
- **macOS kills un-entitled CoreBluetooth clients.** A Python BLE client started from a non-app process died with `Abort trap: 6` before any output. The identical `pixi run ble-sync` command launched through Terminal.app got the standard Bluetooth permission prompt and worked. Bench BLE from a Terminal window, and read the passkey from the serial log if the LCD is out of sight.

- **Do not force a high baud on this board.** Reading the full 16 MB at the esptool default succeeded in **97 seconds**, about 1382 kbit/s. The same read with `--baud 921600` aborted at roughly 1.8 percent with `Serial data stream stopped, possible serial noise or corruption`, and wrote no file. The transport here is native USB-Serial/JTAG, so the requested baud buys nothing and costs reliability. The `backup` and `restore` tasks therefore pass no `--baud`.
- **A failed backup can look like success.** The first attempt exited zero because the failure was hidden behind a shell pipeline, while esptool had actually aborted and produced no file. Always confirm `backup/` really contains a 16777216-byte file before flashing anything.

- **A board in download mode is silent, and that is normal.** Held in the ROM download bootloader the board enumerated fine, answered `esptool chip-id` and `flash-id` every time, yet returned zero bytes on the CDC port across two attempts, including after a DTR and RTS reset pulse and a REPL interrupt. No application is running there, so there is nothing to print. Do not read that silence as a failed board. `pixi run capture` reports this case explicitly rather than hanging.
- **Physical RST re-enumerates USB.** macOS removed and recreated the serial device after a short press. The bounded capture helper now reconnects during its timeout.
- **Serial open can reset this native-USB board.** The initial Parquet host helper deasserted both DTR and RTS and caused an unwanted reset on each open, losing buffered rows. Keeping both asserted and clearing POSIX HUPCL preserved boot identity and buffered sequences across separate status, flush and fetch operations on this macOS/CoreS3 pair. Other hosts/adapters remain unqualified.
- **Build directories are not artifact storage.** Arduino's changed-configuration rebuild removed the first local export copies and serial logs. The SD originals survived; all 13 saved files, including both legacy files, were re-exported and matched both readers. Captures, exports, reports and retained binaries now belong in git-ignored `artifacts/`, outside `build/`. The old logs were not recovered, so do not claim they are still available.
- **Do not scan reserved I2C addresses on ESP32-S3.** Probing `0x01` stopped the first diagnostic. Restricting the scan to `0x08` through `0x77`, as M5Unified itself does, completed and found all nine expected devices.
- **A blank core-dump partition logs one checksum error.** The first boot after flashing reported an expected stored checksum of `0xffffffff` from the unused partition; the application then completed normally. Keep crash diagnostics, but distinguish an empty partition from a new panic in log ingestion.
- **`PIN_POWER_SELECTION` reads `VDD3P3_CPU`.** That is the expected setting for GPIO33 to GPIO37 on a Quad-memory board, and it is consistent with those pins being available to the onboard LCD and microSD circuits rather than consumed by Octal PSRAM.
- **A first SDK bootstrap costs tens of minutes.** About 1.3 GB downloaded in roughly 20 minutes without either an Arduino or an ESP-IDF toolchain finishing. Run one bootstrap at a time, and do not move the cache mid-download, which restarts transfers already in flight. Sizes are recorded in `firmware/esp-idf-cores3/README.md`.

## PSRAM eFuse and runtime results

The eFuse block reports `PSRAM_CAP = None`, `PSRAM_VENDOR = None` and the derived `PSRAM_CAPACITY = 0`. **This is not evidence that the board lacks PSRAM.** Those fuses describe PSRAM packaged inside the ESP32-S3 module, and CoreS3 carries its 8 MB Quad PSRAM as a separate part. The only sound check is at runtime from firmware, by reading the detected SPIRAM size.

The diagnostic detected exactly **8,388,608 bytes**, so the separate 8 MB PSRAM is now bench-verified. The build configuration selected QSPI/Quad PSRAM and explicitly did not select Octal. A future zero reading should be treated as a build or initialization regression.

## Board 1 shared ESP32-S3 modules (firmware v6.4)

**2026-09-28 local (Africa/Cairo; captures span 2026-09-27 UTC)**, both boards attached. CoreS3 was identified as `/dev/cu.usbmodem101`, MAC `44:1B:F6:E2:6B:40`; Waveshare had a separate CH343 serial port. The retained 16 MB original backup was confirmed present. The v6.3 RAM batch was flushed before flashing; a preflash file was saved and validated with both readers, and a final flush saved sequence 14–16 on the card immediately before upload.

Firmware **`arduino-cores3-parquet-v6.4`**, binary SHA-256 `763b835b9e5de042428791ed5b1842385d3a95b1101efd382708cef643bb8644`, retained as `artifacts/firmware/cores3-parquet-v6.4-763b835b.bin`; ELF SHA-256 `f18556f58673900e8c2fde4cc875b70d640a3ef7e6714534cc3b1e0bf6517bb3`. Upload used the pinned Quad-PSRAM/16 MB CoreS3 configuration at 115200 and reported **Hash of data verified**. Build: 1,422,279 program / 86,036 static RAM bytes; schema v3 and the 77-field dictionary digest unchanged.

| Check | Observation |
| --- | --- |
| Boot and identity | Station UUID preserved; new boot `5769e2a9b957a2480d0870807d3a5368`; SD mounted; UTC restored from BM8563 before first row. Existing two unfinished files were quarantined, not repaired/deleted. |
| Sensor and shared LTR553 adapter | PMSA003 valid with zero frame checksum/length errors; four readback rows have valid light/proximity status 3, light raw 5/2 and proximity 0. IMU reports remained enabled/fresh. |
| Sampling | Four-row readback sequence 0–3 has jitter 7,756 / 10,734 / 10,757 / 10,750 µs, no dropped rows or storage errors, clock source 2. Later bounded serial captures continued to report valid sensor rows and zero drops. This is a short observation, not an endurance bound. |
| Wi-Fi/LAN | Existing settings and bearer token worked. Initial association failed with status 6, then the automatic retry connected; mDNS/TCP service worked. INFO identifies v6.4; GET_CONFIG and 2,048-byte LOG_TAIL succeeded. |
| BLE | After the owner disabled phone Bluetooth/Wi-Fi, Mac bonded sessions from Terminal.app read INFO/CONFIG, listed files and retrieved 2,048-byte logs. MTU reported 515 on host / 517 on device. Advertising service data decoded version 1, SD/LAN/new-files flags, file count and boot tag. |
| Shared archive enumeration | Both BLE and LAN LIST completed with **1,079 files and two retained partials**. |
| Same file over both links | Four rows, 11,897 bytes, CRC32 `1a69992a`, SHA-256 `4e6057cf6497f313b0e6e584331ba57e978a4315cfbaf7363eb32661c93f1ce7`; identical BLE/LAN copies, validated by PyArrow and DuckDB. |

File: `station=53315f5f-cb85-4d8d-b623-d56266084189/year=2026/month=09/day=27/data_2330_5769e2a9b957a2480d0870807d3a5368_0-3-0.parquet`. It records v6.4 provenance and RTC-source UTC without altering prior files. The first LAN fetch took **12.390 s** while phone activity was present; a later fetch with the phone disconnected took **1.237 s**. BLE direct-address fetch took **8.817 s**. These small-file timings include OPEN/CRC/control overhead and do not replace earlier throughput measurements or prove the cause of the difference.

Mac name-based BLE discovery was intermittent even when a separate scan saw the device. The host filter was corrected to accept either ADV service data or the UUID list in SCAN_RSP; its regression/frame/LAN tests passed. The final readback used the UUID observed by the successful scan (`--address`), so this run does not establish that the name-discovery issue is fully resolved.

Artifacts live in ignored `artifacts/shared-v6.4/`. Selected identities:

| Artifact | Bytes | SHA-256 |
| --- | ---: | --- |
| `flash.log` | 7,613 | `6a8fbcc18cebc27bb1d9133e0a8577b7ed71d7bad6047375c51336be2e8b4f5a` |
| `postflash-serial.log` | 199,320 | `6a3c5e0c64c48f56b0aca14560d06dad8e14fc31e87585667506ff890d4b6fb4` |
| `rejoin-serial.log` | 4,212 | `dacbc361b17e63cf2c034660c88d1945ee49f5c9720a6da429b650c925f77808` |
| `lan-list.json` (text listing) | 192,662 | `7aff27d5679637c54c329d3772734f15db26e3227f45105e3b97da4586909abf` |
| `lan-fetch.json` | 571 | `1a652c6e4251725d0a4b4a25d746be9c97fb35663bcb9eaca95f4d8a721d2c21` |
| `lan-isolated-fetch.json` | 572 | `51862739d3fce72fb47a3fda084b9a58c6c95aff0b4f7d610e67e0d1beca7515` |

No new power-cut, full-window, compression-endurance or phone-background-wake claim follows from this test. Waveshare remained on its UART/TF diagnostic during this check.

## Still unverified on hardware

Nothing below has been observed on a real board yet. Do not promote any of it into this file without a measurement.

- microSD power-cut recovery, injected short-write failures, card removal/full-media behavior and long-running radio/display stress. Small-batch writes, sync and readback are verified above; normal-reset retention is not a power-loss guarantee.
- Compressed full-window endurance, midnight rollover and arbitrary clock corrections. Host time accuracy/drift, on-device Snappy/Zstd and object-storage upload. The 90-row uncompressed window above is now verified separately.
- Firmware v6 / schema v3 beyond the 1800 s runs above: 3600 s four-group files, LZ4 multi-group files, a hard reset between row groups (footer-less `.partial` retained and reported), `FLUSH` over BLE while `open_rows > 0`, and the phone against v3 files. The 8-group cap and the `-0.0`/NaN statistics rules have host evidence only (`parquet-test --sanitize`).
- Battery presence, charging behavior and current measurement with a known battery state.
- RTC retention across a full power-off and drift over hours/days. The write, read-back, dated software reboot and a 3-minute skew (−513 ms, resolution-bound) are measured above (firmware v6.2, 2026-09-18); backup power for the BM8563 with the main battery removed is not.
- LCD page text and layout for clipping under explicit visual inspection.
- Long-duration PMSA003 sampling, physical warm-up sufficiency and optional SHT20 isolation; the ten-minute acquisition run above is not lifetime qualification.
- Upload, OTA and sleep behavior; sustained radio/display/storage stress on the extracted v6.4 image. Wi-Fi association, mDNS, LAN sync and BLE are measured above; earlier phone/coexistence results stay tied to their recorded firmware images.
- The Companion Device Manager side of background sync (association, presence events, the exemptions) and the Wi-Fi-arrival trigger end to end; the advertising-flag wake is measured above for one event on one phone.

Move a line out of this list only after a dated measurement records the method and result.

## Board 1: shared logger engine, firmware v6.5

**2026-09-28, MAC `44:1B:F6:E2:6B:40`, original card and M134 attached.** Flushed the v6.4 RAM batch before flashing. The first v6.5 image (`2b4b092cff97e48f64a43be0e28f96c7b7f8cb61549bd3908899b4b3690ec677`, ELF `2a5bf50142763ec447a937a6e6c28b85b3c2b940aa6743b4f5380239b2255350`) moves the sampling/storage/clock/sync worker into AQLogger; the trial keeps acquisition, M5 bus arbitration and RTC hooks. It built at 1,426,331 program bytes / 89,260 static RAM bytes.

- Boot `f4067bfc4d2381394ae5d3f61bee812b` preserved station `53315f5f-cb85-4d8d-b623-d56266084189`, pairing bonds, Wi-Fi/LAN settings and token. RTC readback restored the earlier host estimate (`clk=2`, `rtc=1`). PMSA003, IMU, LTR553 and SD initialization succeeded. Legacy listings and the two existing quarantined partials remained available. No display visual inspection or new power-cut claim is made.
- A manual flush produced `station=53315f5f-cb85-4d8d-b623-d56266084189/year=2026/month=09/day=28/data_0015_f4067bfc4d2381394ae5d3f61bee812b_0-6-0.parquet`: seven rows, one row group, 77 columns, 13,139 bytes, CRC32 `d9817653`, SHA-256 `0b0e97188e336a81a3d04ac089e9ad12761682cf6f5a5cf2b963c4cbb08a8503`. USB, encrypted BLE (Mac's existing bond) and authenticated LAN copies matched exactly; PyArrow and DuckDB agreed. Stored light status was 3 and PMS status 4 in all rows; unavailable ambient temperature/RH stayed null.
- Stored jitter was 921–19,915 µs; acquisition intervals deviated from ten seconds by at most 18,993 µs. STATUS reported zero drop/error/missed deadlines. The short file and transfer checks are not a new endurance or full-window benchmark. Wi-Fi initially retried before reconnecting; previous credentials were retained.
- Evidence is retained outside `build/` in `firmware/esp-idf-cores3/artifacts/shared-v6.5/` (`postflash-serial.log`, `usb.log`, `usb.json`, `ble-info.json`, `ble-fetch.json`, `lan-info.json`, `lan-fetch.json` and their readback trees). The BLE small-file transfer took 6.958 seconds; it is not a sustained throughput result.

The fixed-PIN callback and small-MTU JSON guard were subsequently compiled into the final v6.5 image. Both preserve authentication/encryption settings; oversized STATUS/LIVE values remain readable in full but are not notified. Final image identities and post-flash checks follow below.

### Final image and unattended startup correction

A follow-up boot of the intermediate MTU image (`2fabe6ffa2d93c8454fc14fd68c9a85fe3c78580381c58e3e7f2e0b79faa4016`) without a USB reader exposed a pre-existing startup behavior: the worker printed every archived filename before accepting samples. With this card it stalled behind roughly 1,100 paths until a serial capture drained the output. STATUS showed zero buffered rows and increasing drops, and BLE file requests timed out. The preserved 28-row file from boot `47a43a0874b9fa3d01c8ab655037ac32` contains sequences 0–7 then 20–39 and the visible drop counter 12; no missing rows were fabricated. That file (`21,718` bytes, SHA-256 `8a5a8922920d10c128b9598f1079cd739f1d8906cfe6325a2a63c5ef1cf3cae0`) remains on card and in `stalled-boot-readback/`. Opening the reader resumed the worker. This failed run is separate from the successful runs above.

AQLogger now scans/stats/counts at startup and emits one bounded summary. Full filename output is preserved only for explicit `parquet list`. Final v6.5 BIN SHA-256 `d5af58814f15c5361cecae99389d8f19b378ce8e25a03b9093072ed8ec8ce478`, ELF `a5d8f4b717b68236ed9da157bb2fa6f04e7afe315abe07ec6676140335017bd3`; build 1,426,575 program bytes / 89,260 static RAM bytes. Archive and RAM rows were flushed before installing it.

- Boot `74482e87bee9e969d696d309942a9685` was left with **no serial reader**. Authenticated LAN at uptime 119 s reported buffered 8 / drop 0 / error 0 / missed 0. Encrypted BLE at uptime 209 s reported buffered 17 with the same zero counters and successfully fetched the original seven-row file with unchanged SHA-256. This exceeds the eight-row queue slack and verifies the startup correction under that condition.
- Explicit serial listing then completed with 1,089 finalized-file entries, both legacy files, the two retained partials and `PARQUET LIST END`. No file was deleted or repaired.
- New final-image file `data_0045_74482e87bee9e969d696d309942a9685_0-29-0.parquet` has 30 rows, 77 columns, one group, 22,514 bytes, CRC32 `53ef22c7`, SHA-256 `a2ebfb759460a320901b001082e7018f1e5be347074d1fc3e70c9f8129bbaf66`. USB and authenticated LAN readbacks passed both readers and matched. Sequence 0–29 is complete; drop/error/missed counters are zero. Jitter ranged 183–118,304 µs and acquisition intervals 9,897,886–10,017,794 µs, including the BLE transfer/list check. This remains a short run rather than a full-window or endurance claim.
- A real fresh LAN INFO request initially returned empty cached STATUS before the next push. The host client now requests existing OP_STATUS and waits for a nonempty snapshot under its deadline. Actual delayed mock TCP tests passed; real final-image INFO at uptime 449 s then returned current status with zero drop/error/missed counters and one finalized file.

Selected artifact hashes: `no-reader-info-1.json` (816 bytes) `a45baa5e3f07e1df189c1ff35d616436ca743320fe5f8804bf0ead3393f0c1f2`; `scan-ble-info.json` (770 bytes) `26b915b6db65572964cb88fff2c583f9967706b9808e055ecfc7966cb11225ef`; `scan-explicit-list.log` (198,627 bytes) `b34054bca8c5468aaeff64f4b154f9368c07c877030825f4509fcc757997b3b6`. Final binaries, failed-run rows and successful readbacks remain in separate artifact subdirectories. The board was left running this final logger.

## 2026-10-08 — first iOS companion phone observation

Companion app commit `e1a3c6d` from `opensensor-space-android`, installed on the
owner's iPhone 16 Pro running iOS 26.6.2. The supplied phone screenshot showed
Bonjour discovery of `aq-6b40`. That name matches this board's recorded service
name; its station UUID and running firmware image were not read back in this
session. The last image recorded above is final v6.5, SHA-256
`d5af58814f15c5361cecae99389d8f19b378ce8e25a03b9093072ed8ec8ce478`;
this phone observation does not independently verify that image is still running.

After reporting that the other phone was disconnected, the owner reported
"connected" and confirmed that the iOS app was showing live readings. This
qualifies an owner-observed foreground Bluetooth connection and displayed
readings. There is no captured BLE trace, recorded numeric value, measured
notification cadence or comparison with the board's screen. A fresh pairing/PIN
flow, STATUS notifications, stale timeout, background/resume, simultaneous phones
and sustained operation remain unqualified for this iOS app.

The Wi-Fi card was discovery-only and did not connect when tapped. No
authenticated LAN transfer occurred. The permission prompt itself and
allow/deny/revoke paths were not reported. No firmware write, archive mutation or
new power/storage/endurance result follows from this observation.


## Board 1, concurrent LAN collectors (firmware v6.6)

Verified **2026-10-08** on the same CoreS3 at `/dev/cu.usbmodem20301`.
`ports`, `chip`, `flash-id` and read-only `efuse` confirmed ESP32-S3 rev0.2,
16 MB Quad flash and unchanged recovery/security state. Before flashing, a new
**16,777,216-byte** backup was verified at
`backup/m5stack-cores3-flash-20261008T073546Z.bin`, SHA-256
`0e2c9e694e956abf157b00bee6c78f3de919119f7dc6930201bb3d9a8ad82b4f`.

The CoreS3 v6.6 application image SHA-256 is
`8bdb849389789c20a55565d9af651cda1b2e3623d7128849164279ff35dcde21`;
ELF SHA-256 is
`f9d221a450bc9821468e64b95d1107e497309732b09d40f8cfdc20c728e7f602`.
The pinned build reported 1,431,459 program bytes and 92,324 static RAM bytes.
Upload verified the written flash hash. Binaries, filtered secret-free logger
status, result summary and Parquet readback are retained in ignored
`firmware/esp-idf-cores3/artifacts/lan-multi-v6.6/`.
The flashed working tree also included the owner's pending CONFIG_CHUNK/configuration
changes; commit `ae12c85` records only this task's concurrency changes.

- Bonjour advertised `_aqsync._tcp`, `proto=2`, the preserved station
  `53315f5f-cb85-4d8d-b623-d56266084189`, device `441bf6e26b40` and
  `fw=arduino-cores3-parquet-v6.6` at `aq-6b40.local:47390`.
- An existing client already occupied one slot. Two authenticated host TCP
  collectors connected alongside it; CONFIG reported **three clients**. A fourth
  connection returned authentication-handshake ERROR code 7 without takeover.
  The existing third client's platform and transfer state were not established.
- `pixi run lan-concurrency-bench --clients 2 --host aq-6b40.local
  --token-file <private ignored token file> --out <ignored readback directory>`
  downloaded the same immutable 180-row file on both connections:
  `station=53315f5f-cb85-4d8d-b623-d56266084189/year=2026/month=09/day=17/data_1000_495ccef9f3eb8c5917070c921c9baec8_107-286-3.parquet`.
  Both copies had **87,654 bytes**, CRC-32 `4c8cbc19` and SHA-256
  `a8ceaebea9f6f3a2d12c183ae5d3c683b2902c1d10066962cbfebc458fef9d7f`.
  PyArrow and DuckDB agreed on all rows. The two-download interval was
  49.801 seconds (approximately 1.8 KB/s per collector in this short check).
- Both collectors reopened the file before one disconnected. The survivor's
  original handle still returned the correct first window and closed normally.
- Logger status before and after collection showed `dropped=0 errors=0
  failed=false`, with buffered rows increasing from 11 to 31. Three observed
  successive sample deadlines remained approximately ten seconds apart.

An initial attempt preceded successful hostname resolution after boot; a later
three-host attempt was refused because the existing client occupied one slot.
The final two-host check completed without interrupting that third connection.
Direct Python BLE access from the non-app process aborted under macOS's existing
Bluetooth permission boundary; no BLE test is claimed for this image.

This establishes concurrent authenticated host TCP collection and independent
handles on **this CoreS3 image**. It does not qualify simultaneous Android/iOS
phones, BLE+three-LAN transfers, multi-sensor phone runs, sustained throughput,
background behavior, power-cut durability or Waveshare radio behavior.
Both board builds and sanitizer archive/socket fixtures passed; the Waveshare
image was compiled but not flashed in this session. The CoreS3 was left running
v6.6 with its existing Wi-Fi/LAN profile.


## 2026-10-08 — CoreS3 v6.7 LAN latency and USB backpressure

Owner authorized the upgrade and speed check on AQ-6b40 at
`/dev/cu.usbmodem20301`. In order, ports/chip/flash-id/eFuse read-only checks
confirmed ESP32-S3 rev0.2, MAC `44:1b:f6:e2:6b:40`, 16 MB Quad flash,
secure boot/encryption disabled and download access enabled. Before writing,
a full **16,777,216-byte** backup was verified at
`backup/m5stack-cores3-flash-20261008T081738Z.bin`, SHA-256
`629cc82d07ba6ba2f9a220f9ed6a063f23b9115aa522bee34bf6962970ef8dd9`.
No eFuse or card-format operation was performed.

The deployed v6.7 application SHA-256 is
`d914f4e6834a0abefb4f1bbdb2ac98f0f91f252305f120ea0dbb5f9db033935c`;
ELF SHA-256 is
`6e1dc74dddccbe83acdfb8f5a9db2e77b952439e5be5bd88b00f62df6a57da4b`.
Build size: 1,431,859 program bytes / 92,332 static RAM bytes. Upload verified
the written hash. INFO then reported `arduino-cores3-parquet-v6.7` and the
preserved station/full hardware identity; the existing private LAN token worked.
The image includes the owner's pending common configuration work as well as
scheduling commit `c0d5c1e` and diagnostic/provenance commit `be8f52f`; it is a
working-copy image, not a claim of a reproducible clean-commit binary.
Both board builds and relevant sanitized host gates passed. Waveshare was not
flashed; its unrelated pending schema firmware-identity assertion still fails.

All transfers below fetched the same immutable **87,654-byte**, 180-row file
`station=53315f5f-cb85-4d8d-b623-d56266084189/year=2026/month=09/day=17/data_1000_495ccef9f3eb8c5917070c921c9baec8_107-286-3.parquet`,
using six READ windows at advertised `max_read=16384`. Every copy passed
CRC-32 `4c8cbc19`, PAR1, SHA-256
`a8ceaebea9f6f3a2d12c183ae5d3c683b2902c1d10066962cbfebc458fef9d7f`,
and independent PyArrow/DuckDB validation. Timings include OPEN through CLOSE,
excluding subsequent local reader inspection; KB/s below means decimal bytes/s.

| Image / console condition | Host collectors / total connections | Seconds | Bytes/s per collector |
| --- | --- | --- | --- |
| v6.6 initial baseline, USB not drained | 1 / not captured | 33.672 | 2,603 |
| v6.6, USB actively drained and discarded | 1 / 2 | 2.149 | 40,793 |
| v6.6 repeat, USB not drained, same boot as preceding row | 1 / 2 | 16.459 | 5,326 |
| v6.7, USB actively drained | 1 / 1 | 0.912 | 96,096 |
| v6.7, USB not drained, three sequential runs | 1 / 1 | 1.014 / 1.045 / 0.925 | 86,427 / 83,846 / 94,764 |
| v6.7, USB not drained, simultaneous collectors | 2 / 2 | 1.519 / 1.524 | 57,708 / 57,503 |

The state/transfer activity of the second v6.6 connection was not established;
client counts and radio/load differ between images. These are short real-board
host results, not a controlled sustained-throughput or phone-speed multiplier.
The old-image drained/undrained pair nevertheless supports USB-console
backpressure as a material contributor. The cached SDK can block debug writes
for seconds; LAN command and archive READ diagnostics previously gated request
queueing and READ_END. They now remain in LOG_TAIL without console IO; bounded
socket readiness and queue-first worker wakeups remove fixed scheduling waits.
No radio sleep setting, READ/frame limit or integrity check was relaxed.

New-image status snapshots advanced from uptime 62–63 s / buffered 2 to buffered
8 after collection, with `drop=err=miss=fail=0`. A subsequent physical serial
fetch of the same file also passed both readers, CRC and identical SHA-256,
confirming the serial DATA path still works. The original file's acquisition
interval statistics describe its original boot, not this new-image sampling run.
Retained binaries, secret-free reports, upload log and readbacks live in ignored
`firmware/esp-idf-cores3/artifacts/lan-performance-20261008/`.

The sensor remains on v6.7. Android/iPhone throughput after the upgrade, BLE,
background/coexistence/endurance and other boards still need their own dated
qualification. The mobile app was separately rebuilt/installed and now shows
measured average KiB/s; a host result is not a phone observation.


## Board 1, automatic Bluetooth UTC and owner-authorized fresh SD start

Verified **2026-10-09** on CoreS3 MAC `44:1b:f6:e2:6b:40`, native USB
`/dev/cu.usbmodem101`, with firmware source commit `01e6468` and Android mobile
commit `5d5db51`. Repeated ports, chip, flash-ID and read-only eFuse checks in
order: ESP32-S3 rev0.2, 16 MB Quad flash, secure boot/encryption disabled. A new
full backup `backup/m5stack-cores3-flash-20261009T103351Z.bin` was verified at
16,777,216 bytes, SHA-256
`4f55308171dbb27a8dba932d92c223f894ef92d91156dc35daa74e71257a147f`,
before any write. Backups are private ignored artifacts, not repository content.

The owner explicitly requested deletion of old SD Parquet recordings. An
isolated maintenance image, guarded by this exact board MAC and an explicit
serial token, removed only regular `.parquet` files under `/output` and
`/parquet`. It used the production M5Unified/SPI2 initialization and matching
partition table, with logging/radios off and formatting disabled. It enumerated
before deletion, closed directory handles before each bounded removal batch,
then verified counts and unmounted SD. Result: **1,267 files / 52,315,296 bytes
removed, zero Parquet remaining, zero errors**. Two quarantined `.partial`
files totaling 17,291 bytes were retained unchanged by count/size. Other roots,
NVS, owner settings and the phone archive were not cleared. The maintenance
image was immediately replaced with production firmware; no permanent delete
API was added.

Production application SHA-256:
`03ce045f2edf3abbb7ae2151fddf16a1ae43e0da977c12c83ec082beb03424cf`.
Normal separate-component Arduino upload verified all written hashes and
preserved NVS. Runtime station remained
`53315f5f-cb85-4d8d-b623-d56266084189`, boot
`95c96aca68fe09d683a7df86669bc33f`; startup scanned zero finalized files and the
two retained partials. Initial status reported zero drops/errors and
`failed=false`.

The updated OnePlus 7 Pro (GM1911, Android 16) connected over BLE without a
manual time action. Serial recorded `source=host`, epoch `1791542381`, monotonic
`55205803` microseconds, `transport=ble previous=rtc skew_ms=854`, followed by
`RTC write=ok`. The initial RTC batch (sequences 0–2) finalized automatically,
then the first genuine phone-timed measurement (sequence 3) finalized without
waiting for the quarter-hour rotation. Both paths used `2026/10/09/data_1030`.
No USB time-setting command was used.

Network SNTP subsequently supplied source 3 at monotonic `82289782` µs with
UTC anchor `1791542410190961000` ns, and the RTC write succeeded. On reconnect,
the phone selected authenticated Wi-Fi LAN and automatically supplied another
host anchor (`1791542442235000000` ns at `115493452` µs); serial recorded
`transport=lan previous=network skew_ms=-1159` and another successful RTC write.
These offsets describe different clocks; subsecond storage precision does not
establish equivalent absolute UTC accuracy.

All six new files were fetched unchanged over USB with transfer length/CRC
validation. **PyArrow 25.0.0 and DuckDB 1.5.5 matched every value/null across ten
rows (sequences 0–9)**. Exact row UTC equaled the stored anchor plus monotonic
delta in every row, and all UTC dates/quarter-hour windows agreed with their
paths. Sources included RTC (2), phone (1) and network (3). The BLE anchor
retained the phone's fractional milliseconds (`1791542381919000000` ns);
monotonic deltas and the SNTP anchor retained microseconds. Every file carried
reconciliation footer anchors. Drop, missed-deadline and storage-error fields
were zero. Final status: six finalized files, zero drops/errors,
`failed=false`. Android's Files screen reported the new day's **6/6 files,
65.1 KiB**, all synced and checked; its older archive was retained.

Private ignored artifacts are under
`firmware/esp-idf-cores3/artifacts/utc-fresh-20261009/`: the exact production
application, sanitized runtime/cleanup logs, upload log, six readbacks,
`readback-report.json`, `utc-validation.json` and `final-status.txt`. The
isolated maintenance source/binary is in adjacent `sd-cleanup-20261009/`.
This run establishes real automatic BLE/LAN/SNTP UTC and fresh collection on
this CoreS3/OnePlus pair. Its RTC was available at boot: complete power loss,
unknown-UTC recovery, midnight repartition and iOS hardware behavior were not
physically exercised in this run; those paths retain their separate host and
simulator evidence.

## Board 1, first native ESP-IDF flash and readback

Verified **2026-10-09, 20:37–20:52 UTC**, CoreS3 MAC
`44:1b:f6:e2:6b:40`, USB Serial/JTAG `/dev/cu.usbmodem20301`. In order,
`ports`, `chip`, `flash-id` and read-only `efuse` confirmed ESP32-S3 rev0.2,
16 MB Quad flash, secure boot and flash encryption disabled. A fresh full
default-baud backup is retained privately at
`backup/m5stack-cores3-flash-20261009T203744Z.bin`: **16,777,216 bytes**,
SHA-256 `b4f4934bb0d3e935b395d116c48ef0349cfeb9f8e035003d3eea82fdd55da9b6`.
Never publish that image: it contains owner settings and keys.

Final native application is **`idf-cores3-parquet-v6.9`**, schema
`cores3-telemetry-v4`, dictionary v3, built from the fixes committed as
`9baf1ca` with ESP-IDF 6.1, M5Unified 0.2.25 and M5GFX 0.2.31. Application size
**1,517,552 bytes**, SHA-256
`33bb767839825a247ec6dc9c82a6485d5273e6f8481105e939203172e246f0f9`.
The manifest-checked flash verified its written hashes; default-baud readback
of the entire application at `0x10000` matched the retained binary byte for
byte. Only bootloader, partition table, OTA metadata and app0 were written;
NVS was not erased and the card was not formatted, repaired or deleted.

The first physical native attempts exposed four defects, corrected before
leaving the device running:

- ESP-IDF 6.1 rejected the obsolete `idf.py --no-deps` flag before any write.
  The backup/manifest-gated uploader now consumes checked `flash_args` with
  pinned Pixi esptool, avoiding an implicit rebuild.
- At a requested 25 MHz, native SD initialization failed its high-speed CSD
  reread with `ESP_ERR_INVALID_RESPONSE` (`0x108`). Standard-speed 20 MHz
  mounted this card. This identifies a working setting, not a proven
  electrical cause or qualification of other cards/high-speed modes.
- Native default four-universal-MAC policy changed Bluetooth base+1 to base+2,
  preventing existing local IRK lookup (rc 27/17). Restoring the earlier
  two-MAC policy loaded address `44:1b:f6:e2:6b:41` and **two existing bonds**
  without the IRK warning or any key erasure.
- FatFs rejects `mkdir` at its mounted root with `EINVAL`; shared archive creation now checks existing
  directories first. The failed intermediate export was reported as failed;
  unfinished RAM samples in those attempts were lost on reflash.

Final physical checks:

| Check | Observed result |
| --- | --- |
| SD | Mounted, card 31,457,280,000 bytes; capacity error 0; native clock 20,000,000 Hz; SPI2/pins unchanged |
| Owner state | Same station UUID `53315f5f-cb85-4d8d-b623-d56266084189`; `first_boot=false`, configured PIN/Wi-Fi retained, display enabled |
| Acquisition | RTC restored an anchor; IMU/light and PMSA003 available, valid PMS frames with zero checksum/length errors |
| Native network time | Exported rows had `clock_status=3`, epoch 2 and network reconciliation source 3; anchor 20:50:56.150058Z. No host-time command was sent |
| Parquet export | Three rows, sequences 1–3, all 81 columns, UNCOMPRESSED; PyArrow/DuckDB agree; length/CRC verified |
| Sampling | Intervals 9,994,079–9,999,090 µs; maximum deviation from 10 s was 5,921 µs in this short file |
| Health | All exported drop/storage-error/missed-deadline values 0; final STATUS finalized 2, buffered 1, dropped 0, errors 0, failed=false |
| PSRAM | Writer workspace 190,048 bytes allocated; finalization reported 8,185,472 free PSRAM bytes |
| Privacy | All four H3 columns null; cell/country unknown; default maximum resolution 5, awaiting explicit provisioning |

Exported file:
`station=53315f5f-cb85-4d8d-b623-d56266084189/year=2026/month=10/day=09/data_2045_6f40c72a790f8323b18003bda9f0c957_1-3-1.parquet`,
**12,210 bytes**, CRC32 `191a3eca`, SHA-256
`c3320ca999d810ddaf731e4d56023bece8b2cbea18727c14aa77cac72a6bb791`.
The second finalized file reflects the RTC/network-anchor split; it is retained
on the card. No old finalized archive was rewritten.

Evidence is ignored under
`firmware/esp-idf-cores3/artifacts/idf-v6.9-flash-20261009/`; `final/` holds
the production image/ELF/manifest, flash/application-readback logs, bounded
bench capture, reader summary, file copy and final status. Host gates:
`fmt-check`, `lint`, `logger-directory-test` (ASan/UBSan), `cores3-build` and
`waveshare-build` passed. Waveshare was not flashed.

Limits: a short manual-flush USB export, not endurance, automatic full-window
rotation, power-cut durability or display stress. Existing bonds loaded, but
bonded phone reconnect/BLE or LAN transfer were not tested on this native
image. Network time worked, but its selected endpoint, router option 42,
local/public failure transitions and accuracy were not independently measured.
H3 mobile provisioning and location changes were not exercised on hardware.

## Board 1, repeated phone clock updates fragment one interval

On **2026-10-10** (Cairo local date; captured rows are **2026-10-09 UTC**),
read-only USB inspection of `/dev/cu.usbmodem20301` and ADB inspection of the
connected GM1911 compared the six `data_2100` files visible at local midnight.
Their metadata reports `idf-cores3-parquet-v6.9`, schema v4, 900-second rotation,
and boot `6f40c72a790f8323b18003bda9f0c957`, matching the preceding native run.
This investigation did not re-read the flash image hash or flash/reset the board.

| Sequence range | Rows | Clock epoch | Clock source |
| --- | ---: | ---: | --- |
| 55–95 | 41 | 4 | Host |
| 96–96 | 1 | 5 | Host |
| 97–113 | 17 | 5 | Host |
| 114–114 | 1 | 6 | Host |
| 115–117 | 3 | 6 | Host |
| 118–118 | 1 | 7 | Host |

All six already existed on SD and their Android copies were byte-identical.
Serial transfers passed length/CRC checks and PyArrow/DuckDB agreed on every
decoded column. The 64 rows cover consecutive sequences 55–118 without overlap
or gaps. Host anchors changed at 21:06:50.515Z, 21:09:58.576Z and 21:10:40.679Z.
Source inspection identifies the mechanism: Android sends `SET_TIME` on each
authenticated connection; AQLogger finalizes pending rows on the update and
then finalizes the first sample in the new epoch separately. Repeated updates
therefore produce the observed chunk/singleton pairs. This is fragmentation,
not duplicated downloads or duplicated observations.

Evidence is ignored under
`firmware/esp-idf-cores3/artifacts/duplicate-interval-20261010/`: `sd-list.log`,
the six verified files under `sd/`, and `summary.json` with file hashes, clock
anchors, reader results and Android comparisons. No clock/flush command or
archive mutation was performed. Limits: one partial interval and existing
Android copies; this does not qualify iOS, reconnect stability, endurance or
power-loss durability. Buffered serial output also contained SD DMA allocation
failures (`0x101`); their cause and relationship to reconnects were not tested.

## Board 1, v6.10 interval fix and owner-authorized factory reset

On **2026-10-10** (Cairo), the owner explicitly requested deletion of archive,
pairing, Wi-Fi and station settings. Before writing, ports/chip/flash-ID/eFuse
inspection identified this CoreS3, ESP32-S3 revision 0.2, 16 MiB flash, with
flash encryption and secure boot disabled. A default-baud full-flash backup
was verified at **16,777,216 bytes**, SHA-256
`333ebd09003eaf9601bf9f7c555c251bff1570e7aa799e8199b56af657e5aab5`.
The backup is private and ignored; it does not back up the SD card.

The checked native v6.10 image was flashed first to enable physical-serial
archive maintenance. `parquet erase-archive CONFIRM` acknowledged `ok=1`,
removing **130 files and 63 directories**, and parked acquisition until reboot.
The card was not formatted. Full flash erasure then removed NVS identity,
configuration and bonds; the same checked image was reflashed with verified
flash hashes. Image SHA-256:
`0776f1e5c465de80bce0c7ea418f5fc0b0dc68afc6c278ef4637e0eef540cdd8`.

Fresh boot reported station `f3c3fdc1-6f18-4be8-b7bb-fb27692bb60a`, boot
`a11ea18e5ff5c2341ee6226296d7fa63`, `first_boot=true`, random displayed pairing
PIN, zero BLE bonds, Wi-Fi disabled and no configured SSID. SD mounted at
20 MHz; the initial scan found zero archive files, retained partials or
quarantine files. The hardware RTC retained UTC as expected. A physical serial
host anchor was subsequently accepted at epoch seconds `1791581620`, monotonic
`124760508` microseconds.

The firmware removes automatic anchor-triggered flushes and singleton rows;
initial UTC, RTC upgrades and corrections larger than two seconds still split
clock epochs. Small refreshes retain the current epoch while future rows keep
their exact anchors. Host rotation tests cover row-group fullness without
premature file closure. Format/lint, common/status/rotation/archive-reset,
control/archive-sync, writer/telemetry and board contract gates passed, including
sanitizer checks where supported. Both native board applications built; only
this CoreS3 was flashed. Independent review found no blocking issue.

Evidence is ignored under
`firmware/esp-idf-cores3/artifacts/interval-fix-20261010/`: checked image,
ELF/manifest, flash/reset logs and fresh boot capture. Limits: these checks
establish erase, deployment, clean boot and host clock acceptance. Full-window
rotation after the fix, bonded phone reconnect/transfer, endurance and power-cut
durability are not established by this entry.


## 2026-10-11 — main 7384e03 USB reflash and bounded readback

CoreS3 MAC `44:1b:f6:e2:6b:40`, checked native USB port
`/dev/cu.usbmodem112301`. In order, ports/chip/flash-ID/eFuse checks confirmed
ESP32-S3 rev0.2, 16 MB quad flash and an available download path with secure
boot and flash encryption disabled. A fresh full backup,
`backup/m5stack-cores3-flash-20261011T084924Z.bin`, is 16,777,216 bytes,
SHA-256 `0b3a8c4aa0e36b96c0d9d620e5027cdc7575bcde0a6e2eaff4537a5c4abe19e5`.

Pulled `main` at `7384e03`, regenerated the disposable build sdkconfig from
current defaults after the earlier generated FAT lock limit failed compilation,
and built through `pixi run cores3-build`. The checked v6.11 app BIN SHA-256 is
`7e25d7148d36d550f6791c217c67878f61df58176f6e21cab60c3c03016ba40f`.
Buffered samples were flushed before the backup-gated flash wrapper; esptool
verified all written regions. NVS was outside the flash plan, no card format or
archive erase was performed, and the observed station ID stayed unchanged.
Images, build/flash logs, configuration, status and reader evidence are retained
in ignored `firmware/esp-idf-cores3/artifacts/reflash-20261011/`.

A manual-flush USB export contained six rows, 81 columns and one row group,
13,657 bytes, CRC32 `20d77efb`, SHA-256
`85d6b25458bf36608f46b4edc704a32e38c53494598c7b9fe376fa6c0d6c649b`.
Transfer integrity passed and PyArrow/DuckDB agreed. Sequence 0–5 was complete;
intervals were 9,998,103–10,003,014 us. Drops, storage errors and missed
sampling deadlines were zero. PMS warm-up nulls preceded valid frames; light
and proximity status were valid. Stored clock status was network SNTP (3).
Battery voltage readings were implausible and variable (5–2,140 mV), with 0%
reported; battery measurement accuracy is not established by this run.
This is a short USB logging/readback check; phone reconnect/transfer, LAN load,
display behavior, full-window rotation, endurance and power-cut durability were
not tested. Host format/lint and 18 additional relevant sanitizer/contract gates
passed before flashing; their logs are in the same artifact directory.


## iOS bonded reconnect reboot, NimBLE host stack (2026-10-12)

Symptom. Android synced normally, but an iPad reconnect rebooted the CoreS3.
The serial capture showed `BLE BONDED authenticated=true bonds=3`, then
`A stack overflow in task nimble_host has been detected.`, and the next boot
reported `reset=panic(4)`. The host task ran on the 4096 byte NimBLE default
while it built JSON snapshots and parsed control requests on its stack.

Fix (`1f2778c`, on main). `tools/idf-connectivity-sdkconfig.defaults` sets
`CONFIG_BT_NIMBLE_HOST_TASK_STACK_SIZE=8192`, and `ble_sync.cpp` keeps the
snapshot JSON buffer and the control request in static storage instead of host
task locals. Stale `CONFIG_BT_NIMBLE_HOST_TASK_STACK_SIZE` lines were removed
from the disposable build sdkconfig files so the rebuild picked up 8192.
Flashed with the backup gate against
`backup/m5stack-cores3-flash-20261011T084924Z.bin`, NVS kept.

After the flash, three iPad connects (two bonded) completed with no panic, and
`DIAG tasks` reported `nimble_host=4736` bytes free at 62 s uptime. The capture
ends at about 112 s because the board was unplugged, so endurance and a full
iPad transfer on this board are not established. The BLE link sequencing change
recorded for the Waveshare on the same date is not flashed to this board yet.
Logs are in ignored `firmware/esp-idf-cores3/artifacts/ble-reboot-20261012/`.

## Main 67f25dc USB install, fixed PIN and owner archive reset (2026-10-18)

CoreS3 MAC `44:1b:f6:e2:6b:40`, checked native USB port `/dev/cu.usbmodem1401`.
`pixi run ports`, `chip`, `flash-id` and `efuse` confirmed ESP32-S3 QFN56
rev v0.2, flash manufacturer 46 device 4018 at 16 MB, `SECURE_BOOT_EN=False`,
`SPI_BOOT_CRYPT_CNT=0` and `DIS_DOWNLOAD_MODE=False`. Buffered rows were
flushed into one more file before the backup. The full backup
`backup/m5stack-cores3-flash-20261018T071245Z.bin` is 16,777,216 bytes,
SHA-256 `caa0e62548b033e455c4ee19a8220c6c7052e3264411cd3065779072f812ccfe`.
Its partition table matches the current build, with NVS at `0x9000` and
`coredump` at `0xc10000`.

A phone app connected to the board over BLE by mistake at 07:12:46Z, sent
STATUS and a successful SET_TIME, and the link closed at 07:12:51Z. The backup
started at 07:12:45Z, so the phone set the board clock in the same second
esptool reset it into the ROM download path. The 54 s read ran under the
esptool stub with the application halted, so no flash write could happen during
the read. The image still parses, is the full 16,777,216 bytes, and its NVS
region passes the IDF `nvs_tool.py` integrity check, with every written page
CRC32 OK and the `aqcfg`, `parquet` and `nimble_bond` namespaces present. A
reread cannot be compared because the board was flashed afterwards.

`pixi run cores3-build` on `main` at `67f25dc` passed the target checks. App
BIN SHA-256 `3cbc304a2a5a6ebd537069ed6fd1990d51c139477e10a19afbbd1e03a722b420`.
`pixi run cores3-flash` wrote only bootloader, partition table, otadata and
app0, and esptool verified every region hash. No eFuse write, NVS erase, full
chip erase or card format.

| Check | Before | After |
| :--- | :--- | :--- |
| Image | `idf-cores3-parquet-v6.11`, app version `7384e03-dirty`, compiled 2026-10-12, schema `cores3-telemetry-v4` | `idf-cores3-parquet-v6.13`, app version `67f25dc`, schema `cores3-telemetry-v5`, 81 columns, config `cores3-acquisition-v3` |
| Protocol | Not read over radio | Proto 3 from `kProtocolVersion` in the flashed source. Not read over BLE or LAN in this run |
| Settings and NVS | `CONFIG LOADED first_boot=false pair=random pin_default=false wifi_on=false ssid_set=true lan_on=true` | Same line after the flash, same station `f3c3fdc1-6f18-4be8-b7bb-fb27692bb60a` |
| SD and archive | Mounted at 20 MHz, `PARQUET SCAN files=209`, then 210 after the flush | Mounted at 20 MHz, `PARQUET SCAN files=210`, no partials or quarantine |
| Sensors | PMSA003 valid frames | PMSA003 valid frames, for example PM2.5 22 ug/m3 atmospheric, zero checksum or length failures. LTR553, BMI270, AXP2101 and BM8563 answer on I2C |
| Watchdog | Not present on v6.11 | `PARQUET WATCHDOG mode=armed consecutive=0` |

After the flash and verification the owner asked for two changes. Physical
serial `parquet config-hex` set `ble.pair=fixed` and an owner-chosen
`ble.pin`. The board answered `CONFIG SET pair=fixed pin_default=false ...
reboot_required=true` and `AQ CONFIG ok=1`. The PIN digits are kept out of
every log and this record. Then `parquet erase-archive CONFIRM` returned
`PARQUET ERASE_ARCHIVE ok=1 files=210 directories=13 reboot_required=1`. No copy
was kept, at the owner's request. After a reset the board booted the same
image with `CONFIG LOADED ... pair=fixed pin_default=false`, `PARQUET SCAN
files=0`, card used bytes 131,072 and new PMSA003 readings.

Limits. `DIAG rtc enabled=false_or_read_failed` and implausible AXP2101 battery
values (9 to 16,381 mV) appear on both the old and the new image, so this
install did not cause them. BLE pairing with the new PIN, phone sync, LAN and
Parquet readback were not run. Logs and the image are in ignored
`firmware/esp-idf-cores3/artifacts/install-20261018/`.

## Main 689bb61 USB reflash, Thing id and first `geo` footer (2026-10-26)

CoreS3 MAC `44:1b:f6:e2:6b:40`, checked native USB port `/dev/cu.usbmodem11101`.
`pixi run ports`, `chip`, `flash-id` and `efuse` ran before the flash. The
ports listing from the same session is kept with the Waveshare artifacts in
`firmware/esp-idf-waveshare-sim7670g/artifacts/bench-20261026/01-ports.txt`
and shows `/dev/cu.usbmodem11101` as the ESP32-S3 built in Espressif
`USB JTAG/serial debug unit`, with the CoreS3 MAC as its serial number. The CoreS3 eFuse summary
shows `SECURE_BOOT_EN=False`, `SPI_BOOT_CRYPT_CNT=0` and
`DIS_DOWNLOAD_MODE=False`, and the esptool header in the flash log shows
ESP32-S3 QFN56 rev v0.2. The CoreS3 chip and flash-id output was not kept, so
this entry does not quote a flash ID readback. The owner waived a new backup.
The flash helper still needs a backup path for its NVS layout check, so
`backup/m5stack-cores3-flash-20261025T220032Z.bin` from 2026-10-25 was passed
only to satisfy that check.

`pixi run cores3-build` on `main` at `689bb61` (#108, the Thing rename, the
`geometry` column and the `geo` footer key) passed the target checks. App BIN
SHA-256 `6e2492fc98e635179988869c100516c2163bd46dac3d9068a33776ea6998cdf4`.
`pixi run cores3-flash` wrote only bootloader, partition table, otadata and
app0, and esptool verified every region hash. No eFuse write, NVS erase, full
chip erase or card format.

| Check | Result |
| :--- | :--- |
| Image | `idf-cores3-parquet-v6.21`, app version `689bb61`, compiled Oct 26 2026 19:53:16, ESP-IDF v6.1. `PARQUET BEGIN schema=cores3-telemetry-v6 columns=81 ... psram_workspace_bytes=169824 row_bytes=880 ... codec=LZ4_RAW crc32_check=ok`. `parquet schema` printed `sha256=3b4d8500a19f42c6743b0f7d725276c96ca14a0c249f613048603afbc3cfaff9 desc=1c9511bd6836876bf90bbdbe3224b00bd5d176e0fdd5293d5ee6dd73ef6ea45a`. The file footer names dictionary `cores3-telemetry-v7` |
| Settings | `CONFIG LOADED first_boot=false display=true pair=random pin_default=false wifi_on=false ssid_set=false lan_on=true`, `BLE BEGIN name=OS-6b40 ... bonds=0 pair=random` |
| Thing id | Both captured boots (`boot_seq=5` and `boot_seq=6`) and `parquet status` print the same Thing id, which differs from the station id in the 2026-10-18 entry. The id itself is left out of this record |
| Time | `PARQUET TIME ... source=rtc` on both boots. Every row of the first file has `clock_status=2` and a non-null `event_time_utc_us` |
| Watchdog and SD | `PARQUET WATCHDOG mode=armed ... consecutive=0`, `PARQUET STORAGE state=ready`, `PARQUET RECOVERY quarantined=0 repaired=0 deleted=0`, `PARQUET SCAN files=15` before the archive reset |
| Core dump | Both captured boots, `boot_seq=5` and `boot_seq=6`, printed `Core dump data checksum is correct` and `Found core dump 25376 bytes in flash @ 0xc10000`. The 2026-10-25 backup `backup/m5stack-cores3-flash-20261025T220032Z.bin` already holds a dump of 25,376 bytes at 0xc10000, so it predates this image. It was not decoded |
| Archive reset (owner requested) | `parquet erase-archive CONFIRM` returned `PARQUET ERASE_ARCHIVE ok=1 files=15 directories=6 reboot_required=1`. The capture tool refuses that command, so a one-off pyserial script sent it. After a reset the board printed `PARQUET SCAN files=0 retained_partials=0 quarantine_files=0`. Card not formatted |
| Sensors | PMSA003 valid frames with `checksum_failures=0 length_failures=0`, for example PM2.5 35 ug/m3 atmospheric |

| First file | Result |
| :--- | :--- |
| File | `data_1645_4e1530a2..._0-70-0.parquet` in the dated tree, 71 rows, 1 row group, 81 columns, 20,535 bytes, CRC-32 `c30c3179` (matches `PARQUET READY`), SHA-256 `c89062aff53f4b83b49641bf1efe9b595f269f87f9bbf0e471d888c76ffe8e08`. `PARQUET READY ... write_us=187401 sync_us=36662 codec=LZ4_RAW` |
| Cadence | `sequence` 0 to 70, `event_time_utc_us` 16:48:11 to 16:59:51 UTC, the end of the 16:45 window. `scheduled_us` steps exactly 10,000,000 us. `monotonic_us` intervals 9,985,796 to 10,010,024 us, at most 14.2 ms from ten seconds. `rows_dropped`, `storage_errors` and `sample_deadlines_missed` are 0 |
| Footer key order | The raw thrift `key_value_metadata` holds `opensensor` before `geo`. PyArrow 25 returns the keys in a different order, so the raw bytes are the evidence |
| `geo` key | `{"version": "2.0.0", "primary_column": "geometry", "columns": {"geometry": {"encoding": "WKB", "geometry_types": ["Point"]}}}` |
| Location columns | Column 81 `geometry` is BYTE_ARRAY with the Parquet `Geometry` logical type (PyArrow 25 prints `Geometry(crs=)`). Column 78 `h3_cell_id`, column 79 `h3_resolution` and column 80 `location_source` (UINT8) are present. Location is unset, so all four location columns are null in all 71 rows, and the `geometry` chunk statistics carry only `null_count=71` with no bounds |

Limits. Location was unset, so a non-null `geometry` value and geospatial
statistics with bounds are not proven on hardware. No BLE, LAN or phone sync
was run. This short USB readback does not establish endurance, SD or power-cut
durability, and host tests do not prove those either. Logs, the image hashes
and the fetched file are in ignored
`firmware/esp-idf-cores3/artifacts/2026-10-26-v621/`.

## Header battery icon, built only (2026-11-21)

Branch `feat/cores3-battery-icon` from `main` at `4f10521`. The screen header
now draws a battery outline with a fill for the fuel gauge percent, the percent
as text and a yellow bolt while the charge state counts as charging. The
header moved to two status rows. Row 1 at y 4 holds `WiFi` at x 196, `BT` at
x 244 and the page counter at x 296. Row 2 at y 17 holds the battery outline
at x 196 to 217 with its nub to x 219, the percent at x 224 and the bolt at
x 251 to 257. The icon reads the same `read_power_now()` values as the device
page and is left out when `battery_mv` or `battery_percent` is null.

`pixi run cores3-build` passed with the new layout asserts in
`bringup/screen_header.h`. The image was not flashed and no board ran it, so
nothing about the icon, its colours, its update rate or the charging mark is
claimed on hardware. The owner bench test is in the pull request.
