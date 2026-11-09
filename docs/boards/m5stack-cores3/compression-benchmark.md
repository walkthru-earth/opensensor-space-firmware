# Parquet compression experiment

**Framework scope, 2026-10-09:** Current source is native ESP-IDF 6.1 and its
build/host gates pass. No native hardware run is claimed here. The dated
physical observations, images, old API/tool commands and performance figures
below belong to historical Arduino builds; retain those identities when using
this evidence. Native qualification must add a separately dated image/readback.


[Router](../../README.md) · Implementation, host screening and device comparison verified **2026-09-08**. Exact device evidence lives in [bench-verified](bench-verified.md). This stays in the existing Arduino trial; no second framework or radio/upload implementation is introduced.

## Design and scope

This experiment's hardware numbers refer to the 73-column codec image. The later [77-column provenance/time update](../../shared/table-and-observation-model.md) passes host reader/sanitizer tests and short real SD readbacks with both codecs. Those differently sized smoke batches are not a paired compression benchmark; measure full batches before carrying forward the ratios or capacity projections below. [New image evidence](bench-verified.md#board-1-schema-v2-provenance-and-timing)

The writer now accepts **UNCOMPRESSED** or **LZ4_RAW**. The benchmark below was measured with LZ4 1.10.0. LZ4 1.11.0 is vendored now, with source hashes and BSD-2-Clause notices in the [trial pin](../../../firmware/common/vendor/lz4/README.md). 1.11.0 may emit different compressed bytes for the same rows, so the ratios below are 1.10.0 numbers. The adapter uses raw blocks and Parquet codec enum **7**, not deprecated enum 5 or LZ4 Frame. PyArrow's Python metadata API displays enum 7 as `LZ4`. [Parquet codec contract](https://github.com/apache/parquet-format/blob/master/Compression.md), [LZ4 API](https://github.com/lz4/lz4/blob/v1.11.0/lib/lz4.h)

Every DataPageV1 payload, including its definition-level stream, is compressed as one block. Page headers retain both compressed and original byte counts; footer column chunks and row-group totals distinguish compressed from uncompressed sizes. Headers/footer are not page-compressed. One codec applies throughout a file, even where a very small page grows. A codec error aborts the partial file rather than writing raw bytes labeled as compressed.

The benchmarked logger has 73 columns and at most 90 rows; newer source has 77 columns with the same row limit. Uncompressed streaming supports the larger generic writer limits; compressed calls must fit the supplied page scratch or fail before output starts. Firmware reserves 1,024 bytes of raw scratch, enough for its 90-row flat numeric schema, plus `LZ4_COMPRESSBOUND(1024)` output scratch and aligned external LZ4 state with `LZ4_MEMORY_USAGE=12`. There is no per-page codec heap allocation. The format workspace grew from 2,048 to **2,816 bytes** to track original column sizes. These buffers are in the PSRAM writer allocation, not on the task stack; the 16 KiB task stack and 4 KiB stdio staging remain unchanged. Static bounds/free-heap observations are not a whole-system allocation trace or radio-load memory guarantee.

`parquet codec none` and `parquet codec lz4` first finalize pending normal rows under the old codec, then change the running session and save the choice in NVS, so it survives a reboot. A board with no saved choice boots LZ4_RAW (before #25 it booted UNCOMPRESSED). Existing files are never recompressed or overwritten. Uncompressed is an explicit user-selectable fallback, not silent fallback on codec failure.

`parquet codec-test` writes two diagnostic copies of the **same buffered rows**, first uncompressed and then LZ4, under `output/benchmarks/boot=<boot>/`. It retains the original batch for normal rotation. Copies carry `purpose=codec-comparison-duplicate-rows` metadata; normal output carries `purpose=telemetry`. Do not include benchmark copies in Thing telemetry queries. The finalized-file counter includes these diagnostic writes.

## Host screening, not device timing

Input: 54 real rows × 73 columns from `data_1845_1491409bed013e1999dc4072ac91ab0f_26-79-5.parquet`, SHA-256 `4601fe0a41bdb8d00b946c89001d2a1164484565d6d35e3be7a4ea1d7a46beb9`. PyArrow 25.0.0 rewrote identical values using PLAIN, DataPageV1, no dictionaries, no statistics and no serialized Arrow schema. Both PyArrow and DuckDB 1.5.5 matched the original stored values/nulls for every codec. Ten host writes per candidate:

| Codec | Whole-file bytes | Footer bytes | Host median write, µs |
| --- | ---: | ---: | ---: |
| UNCOMPRESSED | 27,344 | 6,139 | 346.7 |
| Snappy | 14,213 | 6,055 | 389.7 |
| LZ4_RAW | 14,164 | 6,056 | 369.9 |
| Zstd level 1 | 13,264 | 6,057 | 472.8 |

This screens candidates, not firmware speed or peak memory. PyArrow's page headers/footer differ from the custom writer, so the original 26,828-byte device file is not this table's uncompressed denominator. LZ4 was selected for the first board implementation because it was close to Snappy in whole-file size and offers explicit small external state. Zstd's additional host size saving does not establish its device memory/time tradeoff. Snappy and Zstd are **not implemented on the board**.

## Hardware result: LZ4 passed

Same Board 1, same mounted card, **60 real rows × 73 columns**, sequences 0–59, boot `e452563e96c72ca057a48924550779b6`; no radio load. Three paired writes on the device used identical buffered values/nulls. All six readbacks passed both readers and paired-table equality. The rows were unsynchronized (UTC null) during this controlled comparison; normal host time was restored afterward.

| Measurement | UNCOMPRESSED | LZ4_RAW |
| --- | ---: | ---: |
| Complete-file bytes | 28,537 | **14,080** |
| Finalization range, µs | 113,828–122,849 | **91,777–98,629** |
| Median finalization, µs (3 writes) | 118,420 | **93,036** |
| Writer/sink range, µs | 36,650–36,777 | 33,797–35,057 |
| LZ4 calls only, µs | 0 | **3,323–3,658** |
| Flush/sync/close range, µs | 13,284–15,716 | 12,505–12,658 |

LZ4 reduced total bytes by **50.7%** and median observed finalization by **21.4%**. Fixed codec workspace was **6,208 bytes**, with no per-page codec allocation; runtime reports after writes showed 308,260 bytes free internal heap, 8,281,644 bytes free PSRAM and 5,216 bytes free storage-task stack. The minimum internal heap report reached 307,424 bytes during the test. These are scoped observations and a static workspace bound, not a global peak-allocation trace.

Final comparison status: 60 normal rows still buffered, six diagnostic files finalized, zero drops/errors, queue high-water 1. The original rows remained available for normal telemetry. All three copies for each codec were byte-identical. [Exact hashes, cadence and image](bench-verified.md#board-1-lz4-compression).

Evidence is retained in `firmware/esp-idf-cores3/artifacts/compression-20260908/`: `report.json`, `device.log` and all six files. At the time of this benchmark reboot returned to UNCOMPRESSED. Since #25 the codec choice is saved and LZ4_RAW is the default when nothing is saved. Compression does not change the RAM-loss or SD power-loss limitations.

The follow-up normal 64-row file retained the original 60 rows unchanged, plus contiguous post-comparison sequences 60–63, with no recorded drops/deadline misses/storage errors. A subsequent three-row **normal LZ4 Hive file** passed both readers and UTC partition checks after host time was restored. Both codec configuration commands acknowledged successfully; the device was left at 900 seconds with LZ4 selected.

**Meaning of “better”:** smaller complete files and lower median finalization time on these three paired writes, with interoperable values/nulls preserved. It does not mean every write is faster: a later 12-row normal LZ4 file took 140,291 µs to finalize, demonstrating SD/I/O variability beyond the paired range. It also does not establish energy savings, greater card lifetime or proportional allocated-space savings. FAT cluster rounding can erase the space benefit for small files; see [offline capacity](../../shared/telemetry-pipeline.md#offline-capacity-and-reconnection). Snappy/Zstd, full compressed-window endurance and reconnect/upload tests remain open.

## Reproduce and retain evidence

Run from the repository root, with only one process owning the serial port:

```sh
pixi run parquet-test --sanitize
pixi run compression-bench <exported-file.parquet> --repeats 10
pixi run python tools/benchmark_device_compression.py --port /dev/cu.usbmodem101 --out firmware/esp-idf-cores3/artifacts/compression-new --min-rows 60 --seconds 700 --repeats 3
pixi run parquet-device command --port /dev/cu.usbmodem101 'parquet codec lz4'
pixi run parquet-device command --port /dev/cu.usbmodem101 'parquet codec none'
```

The device helper does not reset, change the clock or alter the normal interval/codec. It waits for a minimum buffered row count, creates repeated comparison pairs, retrieves and independently validates each file, then checks that each compressed table equals its baseline. A time-aligned partial window may rotate before reaching the requested count; use a sufficient bounded timeout or start just after a new window. Its JSON report/log live in the new output directory. The tests deliberately duplicate data only in the benchmark tree.

Interpret `codec_us` as time inside LZ4 calls only; `writer_us` includes serialization and sink calls; `write_us` includes complete finalization/readback/CRC/rename, and `sync_us` is the flush/sync/close subset. Since firmware v6 a file can hold several row groups: `codec_us`, `writer_us` and `sync_us` on the `READY` line are sums over all groups plus the footer, each `PARQUET GROUP` line carries that group's own `writer_us`/`codec_us`, and `write_us` runs from file creation (first group) to finalization, so for a multi-group file it includes the minutes spent waiting for later batches and is not an encoding time. Benchmark copies from `codec-test` are always one row group, so the paired comparison is unaffected. Compare complete-file bytes and all relevant timings, not codec CPU alone. Three repetitions are not p99 or endurance qualification. Record sampling jitter/drops across writes and free/minimum heap, PSRAM and stack reports alongside the explicit workspace bound.

**Artifact lesson:** Arduino cleaned the previous `build/` contents during a changed-configuration rebuild. All 13 saved Parquet files were restored from the card and revalidated under `firmware/esp-idf-cores3/artifacts/exports/restored-20260908/`, including legacy files. Earlier build-local serial logs were not recovered. Use git-ignored `artifacts/` for exports, logs, reports and retained binaries; `build/` is disposable. Current firmware exposes old `/sd/parquet/` files through the read-only `legacy-parquet/` prefix. `tools/export_parquet.py` retrieves every listed finalized file without flushing or deleting device data.

Power-cut recovery, card faults, compressed full-window endurance, radio/display stress and on-device Snappy/Zstd comparisons remain separate qualification work.
