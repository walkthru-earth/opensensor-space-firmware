# CoreS3 microSD and logging

Current source uses native ESP-IDF 6.1 SDSPI on the existing M5GFX SPI2 bus.
Native builds pass; a [short native write/USB readback](bench-verified.md#board-1-first-native-esp-idf-flash-and-readback)
passed at 20 MHz. Sustained display/card arbitration and SD durability require
qualification. Dated measurements below remain historical Arduino
image evidence. [Current workflow](../../../firmware/esp-idf-cores3/README.md).

[Router](../../README.md) · Read for the **built-in slot**, shared SPI, file logging or data export. Card presence is optional. Snapshot **2026-09-08**; automatic 60-row Parquet batching, short Hive-file readback and normal-restart retention are bench-verified on one card. Power-loss behavior remains untested.

## Wiring and ownership

| Resource | CoreS3-specific requirement |
| --- | --- |
| Slot interface | **SPI**: SCK36, MISO35, MOSI37, CS4. Use native IDF SDSPI; the slot is not wired for native 4-bit `SD_MMC`. [M5 example][m5-sd] |
| Display sharing | LCD CS3; SCK36/MOSI37 shared. **GPIO35 is LCD DC while CS3 is low, and SD MISO while CS3 is high**. M5GFX switches its direction; a generic always-output LCD DC driver causes contention. [CoreS3 panel implementation][gfx] |
| SPI host | M5GFX's CoreS3 configuration uses `SPI2_HOST`; do not drive the same wires through another host or independently reinitialize its peripheral state. [Tagged configuration][gfx] |
| Power / detect | AXP2101 **ALDO4 = 3.3 V** powers the card; `TF_SW` is AW9523 **P0_4**, not an ESP GPIO. Board power/expander setup is required; see [hardware](cores3-hardware.md). [Power setup][power] |
| Capacity | M5 lists **16 GB maximum**. This board mounted one nominal 32 GB SDHC card, reported 31,457,280,000 bytes and passed the bounded Parquet write/readback tests. That does not qualify power-loss behavior or every larger card; the card's exact model was not recorded. Keep M5's figure as the general support envelope. [Board specification][board], [measured](bench-verified.md) |

## Bring-up paths

- Native startup initializes M5Unified/M5GFX once, completes display DMA/transactions, and attaches the SDSPI card to the existing SPI2 bus. Use the application display/SD mutex for complete transactions; do not call `spi_bus_initialize` or free the board driver’s controller. Mount with `format_if_mount_failed=false` and check every result. The historical Arduino/M5 SD example remains a wiring/startup reference only. [Shared-bus startup][sharing]
- M5GFX's CoreS3 initialization calls `_set_sd_spimode(..., GPIO_NUM_4)` before display traffic. Preserve that behavior when modifying startup. If replacing the board driver: initialize power/bus, idle other CS lines high, put the card into SPI mode, then communicate with other SPI devices. A card still in SD mode may respond to unrelated traffic despite its CS being high. [M5GFX][gfx], [IDF shared-bus startup][sharing]
- Native C/IDF: `SDSPI_HOST_DEFAULT`, `sdspi_device_config_t`, `esp_vfs_fat_sdspi_mount`; attach to the existing SPI2 bus when the board driver owns it. Preserve M5GFX bus locking; coordinate lifecycle/multi-call operations at application level too. Do not independently initialize/free the display's bus. [SDSPI guide][sdspi], [M5GFX bus backend](https://github.com/m5stack/M5GFX/blob/0.2.36/src/lgfx/v1/platforms/esp32/common.inl)
- Use a known FAT32 card first. exFAT is not enabled by default in IDF; a large factory-formatted card can fail to mount despite working electrically. Set `format_if_mount_failed=false`; report mount failure distinctly from a missing card. [FatFs][fatfs]
- The M5 example requests 25 MHz; this is not a guaranteed throughput or a universal maximum. Reduce the transfer clock when debugging integrity, and inspect rail stability, CS timing and bus loading before increasing it. Protocol probing starts slowly. [M5 example][m5-sd], [signal loading][sharing]

## Concurrent logging design

The record, Parquet lifecycle, optional recovery spool and later upload decisions live in the [telemetry pipeline](../../shared/telemetry-pipeline.md). This file owns the physical SD and filesystem constraints.

- Give storage one worker and a bounded queue. Serialize display/SD bus access with a common application mutex; complete display DMA and end any held display transaction before SD access. Keep each bus hold short; release before waiting for network or sensor work. A task per core does not make shared wires concurrent.
- Start with batched sequential writes, e.g. 4–16 KiB in 512-byte multiples, then measure worst-case write/sync latency. Keep DMA staging internal/aligned; PSRAM can hold backlog if the driver copies safely. Size the queue from measured stalls and record dropped samples. FatFs sector-aligned multi-sector I/O reduces overhead. [FatFs performance notes][appnote]
- Create new archive files with explicit POSIX exclusive-create flags (`O_CREAT | O_EXCL`) and use the native stdio sink. Retry positive short writes; treat zero/error writes as failures. Never truncate, append to or replace finalized Parquet.
- Set a durability interval in time/bytes: `fflush(FILE*)` **then** `fsync(fileno(FILE*))` through the native stdio/VFS sink, or raw FatFs `f_sync`. Check the APIs that return status. Flushing reduces the loss window; it does not make FAT transactional or guarantee survival of the card controller's own power loss. [FatFs synchronization][sync]
- Implemented application format: one scalar measurement row every 10 seconds, a bounded PSRAM batch and an eight-row producer queue. A small C++ writer emits LZ4_RAW (default, saved in NVS) or UNCOMPRESSED Parquet without Arrow, with default 900-second rotation configurable to 600, 1800 or 3600 seconds for the running session (firmware v6). With a supplied UTC estimate, files split at aligned windows and clock-epoch changes; each completed batch of at most 90 rows is one `fsync`ed row group of the open file, so a 3600-second file holds up to four. Publish after footer, `fflush`, `fsync`, close and structural readback checks; PyArrow/DuckDB supply full host conformance validation. Startup moves interrupted `.partial` files out of the active `/output/` tree into `/output/quarantine/<thing>/<partition>/` without deleting or repairing them; recovery remains host-only. The unfinished RAM batch is lost on reset; a recovery spool remains future work. See the [telemetry lifecycle](../../shared/telemetry-pipeline.md#parquet-file-lifecycle-and-optional-recovery-spool) and [bench record](bench-verified.md) for the exact tested scope. Avoid rewriting the dataset per sample and stay below FAT32's **4 GiB − 1 byte** limit. [FAT limits/power interruption][appnote]
- Eject/shutdown: stop producers → drain queue → sync/close → unmount (`esp_vfs_fat_sdcard_unmount`) → remove card/power. Unexpected removal becomes unavailable storage; never autoformat to recover. **Insertion/power cycling requires a quiesced display/bus and a fresh SPI-mode handshake before display traffic resumes**; startup-only `_set_sd_spimode` does not establish hotplug support. [Startup restrictions][sharing]
- ALDO4-off alone does not isolate an inserted card: shared SPI lines/pull-ups can back-power it. Quiesce or electrically isolate the relevant signals before power-gating, then measure the rail. [Schematic p5](https://m5stack-doc.oss-cn-shenzhen.aliyuncs.com/490/Sch_M5_CoreS3_v1.0.pdf). For USB MSC export, give the host or firmware exclusive filesystem ownership.

## File layout and time

Card files use `output/thing=<UUID>/year=YYYY/month=MM/day=DD/data_HHMM_<boot>_<first>-<last>-<attempt>.parquet`. The UUID persists in NVS; the date and window-start `HHMM` are UTC. The suffix distinguishes shortened batches, reboot sessions and clock corrections without overwriting existing files. Firmware accesses the card through `/sd`, so its absolute path begins `/sd/output/`. See [the telemetry layout](../../shared/telemetry-pipeline.md#upload-and-cloud-layout) for time/identity semantics.

UTC can come from a successful native SNTP callback or be supplied explicitly with `pixi run parquet-device sync-time --port <port>` or the bench helper's `--sync-time`; neither source has a measured synchronization accuracy guarantee. Until then, files remain under `output/thing=<UUID>/unsynced/boot=<boot>/`, with null UTC columns. An empty `thing=<UUID>/unsynced/ringless=<boot>/` folder marks a boot whose NVS boot ring append failed, so a later `LIST_SINCE` still lists that boot's unsynced files. The logger removes it, best effort, when a later append in the same boot succeeds. Otherwise it stays until the archive is erased. It holds no files. Since firmware v6.2 that estimate is also written to the BM8563 RTC and restored at the next boot (rows carry `clock_status = 2`), so `unsynced/` only fills on a board whose RTC was never set or lost power (see [telemetry pipeline](../../shared/telemetry-pipeline.md)). Subsequent clock updates create a new clock epoch and split the batch without changing previous rows. Object-storage upload, cloud compaction and Iceberg are later work; finalized SD files are retained locally.

The [provenance/time update](../../shared/table-and-observation-model.md) preserves this lifecycle and path layout, appends four row fields and references a versioned dictionary in each footer. It is flashed and passed short unsynced/UTC SD readbacks in both codecs. Existing files stayed present; larger full-window samples are still needed before updating capacity estimates. [Measured scope](bench-verified.md#board-1-schema-v2-provenance-and-timing)

### Lessons from the SD run

- Write new files exclusively; a simple `data_0900.parquet` basename would collide after a manual flush, restart or clock correction in the same window. Keep the boot/sequence/attempt suffix and do not append to finalized Parquet. Window/epoch transitions are processed when the next sample reaches the worker, not by a separate wall-clock alarm.
- The 60-row uncompressed file was 28,059 bytes; finalization took 122,041 µs, including 13,317 µs in flush/sync/close. These are single-run values, not worst-case card latency. Keep acquisition queued independently and remeasure under load. Exact image/schema and timings belong in the [bench record](bench-verified.md#board-1-on-device-parquet-and-hive-partitions).
- `PAR1`, footer bounds, size and whole-file CRC checks establish structural completion/readback integrity, not complete Parquet semantics, page-level checksums or power-loss safety. Both independent host readers were also used; normal-reset byte retention was tested separately.
- The stdio sink retries positive short writes until the requested buffer is complete and treats a zero/error return as failure; `fflush`, `fsync`, close, structural readback, size and rename are checked separately. A failed write retains its batch in RAM; subsequent samples can be dropped and counted while the SD space governor retries with a remount on a 10 s to 10 min backoff, or at once on `parquet flush`. A full card pauses logging and resumes when space returns, nothing is deleted. `.partial` files are conservatively moved to `/output/quarantine/<thing>/<partition>/` at boot and right after a same-boot failure, and reported; status exposes the bounded total quarantine file count and bytes, including files from earlier boots. No footer reconstruction, deletion or overwrite is attempted. Neither repair nor graceful eject is implemented as a user command; `parquet flush` alone does not stop producers or unmount the card. Do not remove it while the logger runs.
- Finalized-file retention and unfinished-row durability are different: an accidental host-induced reset already demonstrated RAM loss. The proposed recovery spool is not present, and the card was not power-cut, removed or filled during these tests. Preserve earlier files and avoid autoformatting while diagnosing a failure.

The current readback protocol also exposes the older `/sd/parquet/` folder as `legacy-parquet/<name>`. Diagnostic codec comparison copies now go to `Config::benchmark_directory` (`/sd/bench/`), outside the listed archive, and contain duplicate rows, not additional telemetry. Older cards may still hold earlier copies under `output/benchmarks/`. Export into the trial's git-ignored `artifacts/`, outside the rebuildable native `build/` directory; see [compression and artifact lessons](compression-benchmark.md).

The measured LZ4 comparison cut file bytes by 50.7% and median finalization by 21.4% for identical rows; this supports local compressed Parquet feasibility, not proportional allocated FAT-space savings or a worst-case latency bound. The 32 GB card's multi-year capacity projections, unknown cluster-size overhead and still-unimplemented reconnect/upload policy are in [offline capacity and synchronization](../../shared/telemetry-pipeline.md#offline-capacity-and-reconnection). No card-lifetime or battery-runtime guarantee follows from file-size arithmetic.

## Lookup / verification triggers

| Trigger | Investigate |
| --- | --- |
| Mounts intermittently | FAT vs exFAT, ALDO4 rail, TF_SW, GPIO35 direction, CS3/4, SPI clock, add-on loads; test a cold power cycle, not only software reset. |
| Slow or unstable logger | `CONFIG_FATFS_IMMEDIATE_FSYNC`, per-file cache, `max_files`, allocation unit, `disk_status_check_enable`; [FatFs options][fatfs]. Measure latency percentiles while UI, BLE and Wi-Fi upload are active. |
| Large exports | Validate device-generated Parquet on the host, then stream finalized files in bounded chunks for later object-storage upload; checkpoint acknowledgements independently. Budget row groups, encoding and footer finalization on the device. Cloud conversion is not a prerequisite; compaction/Iceberg can follow later. [Parquet format](https://parquet.apache.org/docs/file-format/). |
| Acceptance | Write/read/CRC check across rotations; full-card, removal/reinsert and power interruption during write/sync; verify previous files survive and queue overflow is observable. Record card model/capacity/filesystem and SDK versions. |

[m5-sd]: https://docs.m5stack.com/en/arduino/m5cores3/sdcard
[gfx]: https://github.com/m5stack/M5GFX/blob/0.2.36/src/M5GFX.cpp
[power]: https://github.com/m5stack/M5Unified/blob/0.2.29/src/utility/Power_Class.inl
[board]: https://docs.m5stack.com/en/core/CoreS3
[sharing]: https://docs.espressif.com/projects/esp-idf/en/v6.1/esp32s3/api-reference/peripherals/sdspi_share.html
[sdspi]: https://docs.espressif.com/projects/esp-idf/en/v6.1/esp32s3/api-reference/peripherals/sdspi_host.html
[fatfs]: https://docs.espressif.com/projects/esp-idf/en/v6.1/esp32s3/api-reference/storage/fatfs.html
[appnote]: https://elm-chan.org/fsw/ff/doc/appnote.html
[sync]: https://elm-chan.org/fsw/ff/doc/sync.html
