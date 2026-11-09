# Small Parquet files, compaction and range reads

This page records the October 2026 investigation behind the roadmap in [issue #25](https://github.com/walkthru-earth/opensensor-space-firmware/issues/25). It keeps the measured numbers, the proposals and the open decisions in the repo so they can be rerun when new firmware ships. Nothing here changes the firmware or the wire contract. Each proposal needs its own PR, bench and, where marked, phone reader work first.

The outcome is recorded in [decision 0002](decisions/0002-small-parquet-files.md), which says what the firmware does now, what it will not do, what moves to a cloud compactor and what would reopen it.

Raw results sit next to this page in [`parquet-small-files/`](parquet-small-files/). Rerun them with `pixi run size-survey` as shown at the end.

## What the writer emits today

- [`parquet_writer.cpp`](../../firmware/common/src/parquet_writer.cpp) writes one PLAIN DataPageV1 per column chunk, RLE definition levels, UNCOMPRESSED or LZ4_RAW, and per chunk statistics with exact min and max. It writes no dictionary, no column index, no offset index and no bloom filter.
- Since [#135](https://github.com/walkthru-earth/opensensor-space-firmware/issues/135) step 1 the writer can also write INT32 and INT64 columns as DELTA_BINARY_PACKED when the caller passes `IntegerEncoding::DeltaBinaryPacked`. The default is PLAIN, no board passes it and there is no NVS key or Kconfig for it, so every board file is byte for byte as before.
- The footer carries `field_id`, `TIMESTAMP(MICROS, UTC)`, `sorting_columns` on the sequence column, `TYPE_ORDER` column orders and the key value metadata.
- [`os_logger.cpp`](../../firmware/common/logger/src/os_logger.cpp) caps a row group at `kMaxRows = 90`. When this survey ran it booted with `Codec::Uncompressed` and `parquet codec lz4` was lost on reboot. Since #25 the choice is saved in NVS and LZ4_RAW is the default when nothing is saved.
- Rotation is 600, 900, 1800 or 3600 s with a 10 s sample. Row groups are 60x1, 90x1, 90x2 and 90x4 rows.

## Survey of device files

[`survey-20261016.jsonl`](parquet-small-files/survey-20261016.jsonl) covers 144 device files from the local bench artifacts, 90 of them unique by name and size. One entry is a directory and is recorded as an error. The files span Arduino v2 to v6.7, IDF CoreS3 v6.9 and v6.11, and Waveshare v2.5 and v2.6. There is no CoreS3 v6.12 capture yet.

- Every file has exactly one page per column chunk, checked by walking the page headers.
- Every file uses PLAIN and RLE only and has no page index or bloom filter.

| Source | Rows | Row groups | Columns | Bytes | Footer | Footer share |
|---|---|---|---|---|---|---|
| Waveshare v2.6 bench readback | 4 | 1 | 56 | 10,782 | 8,162 | 75.7 % |
| Waveshare v2.5 | 5 | 1 | 56 | 10,735 | 7,967 | 74.2 % |
| CoreS3 v6.11 full 900 s window | 90 | 1 | 81 | 49,839 | 9,562 | 19.2 % |
| CoreS3 v6.11 partial | 6 | 1 | 81 | 13,657 | 9,310 | 68.2 % |
| CoreS3 v6.9 partial | 1 | 1 | 81 | 11,444 | 9,173 | 80.2 % |
| Arduino v6 1800 s | 180 | 2 | 77 | 87,654 | 13,672 | 15.6 % |

- A full 90 row CoreS3 window costs about 554 B per row uncompressed.
- Key value metadata is 37 to 42 entries and 2.0 to 2.7 KB per file. The largest entries, `acquisition_config` and `time_semantics`, repeat unchanged in every file.
  Note added 2026-10-26. Since #88 the flat keys are gone and the footer carries one nested `opensensor` key of compact JSON instead, about 1.7 KB on CoreS3 and 2.3 KB on Waveshare, plus the `geo` key. The survey numbers above predate it.
- Each extra row group adds about 4.4 KB of footer on the 81 column CoreS3 schema, about 54 B per column chunk. A one off host build of the current writer gave footers of 7.9 KB for 1 row group, 12.3 KB for 2, 21.1 KB for 4 and 38.6 KB for 8. A real 3600 s file should carry a footer near 22 KB. That is an estimate until a v6.12 capture exists.
- Every 900 s file fits inside the phone's 64 KiB tail read, so the phone already reads one in a single request.

## Encoding and codec experiment

[`encodings-20261016.jsonl`](parquet-small-files/encodings-20261016.jsonl) rewrites the rows of three real files with the PyArrow writer on the host. These are host numbers, not firmware numbers. For the 90 row CoreS3 v6.11 file, data bytes without the footer are as follows.

| Variant | Data bytes | Change | Phone reads it today |
|---|---|---|---|
| PLAIN, UNCOMPRESSED (default before #25) | 43,050 | baseline | yes |
| PLAIN, LZ4_RAW | 17,842 | −59 % | yes |
| PLAIN, ZSTD | 14,303 | −67 % | Android only |
| Dictionary, UNCOMPRESSED | 21,025 | −51 % | yes |
| DELTA_BINARY_PACKED ints, UNCOMPRESSED | 12,672 | −71 % | yes |
| DELTA ints, LZ4_RAW | 11,344 | −74 % | yes |
| DELTA ints plus BYTE_STREAM_SPLIT floats, LZ4_RAW | 10,647 | −75 % | yes |

Updated 2026-11-04. The last column now shows the current phone reader. In October the three DELTA rows said no. The phone has read DELTA_BINARY_PACKED and BYTE_STREAM_SPLIT since 2026-10-18, see Phone reader limits below.

- The 86 row v6.7 file and the 180 row v6 file show the same order.
- BYTE_STREAM_SPLIT alone gains nothing without a codec.
- A page index with one page per chunk adds 1.6 to 1.7 KB per file and tells the reader nothing the row group statistics do not.

### Firmware DELTA encoder on the host

Added 2026-11-09 for [#135](https://github.com/walkthru-earth/opensensor-space-firmware/issues/135) step 1. These numbers come from the firmware writer itself, built on the host by `pixi run parquet-test`. The fixture is [`tools/delta_fixture.cpp`](../../tools/delta_fixture.cpp). It has the 84 columns of the CoreS3 dictionary in order, 43 INT32, 28 INT64, 11 FLOAT and the geometry, with synthetic values that move the way a bench window does. It is not a captured file, so treat the numbers as direction, not as a device figure. Bytes are whole files with the footer.

| Variant | 1 x 90 rows | Change | 4 x 90 rows | Change |
|---|---|---|---|---|
| PLAIN, UNCOMPRESSED | 48,469 | baseline | 187,045 | baseline |
| PLAIN, LZ4_RAW (board default) | 23,818 | −51 % | 88,543 | −53 % |
| DELTA ints, UNCOMPRESSED | 17,702 | −63 % | 64,188 | −66 % |
| DELTA ints, LZ4_RAW | 15,425 | −68 % | 55,233 | −70 % |

- Against the board default PLAIN LZ4_RAW, DELTA plus LZ4_RAW is 35 % smaller for one row group and 38 % smaller for four. The PyArrow rewrite of the real v6.11 file above gave 36 %.
- Only INT32 and INT64 change. FLOAT and the GEOMETRY BYTE_ARRAY stay PLAIN and the definition levels stay RLE.
- Blocks hold 128 values in 4 miniblocks of 32. PyArrow 25 uses the same block for INT32 and 256 values for INT64, so the INT32 streams are byte equal to the ones PyArrow writes and the INT64 streams are checked against a Python encoder written from Encodings.md.
- `pixi run parquet-test --keep-delta DIR` copies the 52 PLAIN and DELTA twins to a new directory, for example `firmware/esp-idf-cores3/artifacts/parquet-delta/`, which git ignores. Names are `edge-<rows>-<plain|delta>-<none|lz4>.parquet` and `real-<groups>-<plain|delta>-<none|lz4>.parquet`. A reader outside this repo, such as the phone `EncodingFixtureTest`, can read each DELTA file and compare it with its PLAIN twin.

## On device cost evidence

- The [CoreS3 compression benchmark](../boards/m5stack-cores3/compression-benchmark.md) (Arduino era, 73 columns, 60 rows, paired writes) measured LZ4_RAW at 28,537 to 14,080 B (−50.7 %), median finalization 118 to 93 ms, 3.3 to 3.7 ms of LZ4 per group and 6,208 B of codec workspace.
- Arduino v6.1 logs show a 90 row LZ4 group of 12,769 B with 5.5 ms of codec time, against 36,989 B for the same group uncompressed.
- The IDF CoreS3 soak log shows a 90 row, 81 column uncompressed group at 40,269 B with 118 ms of writer time, and the finished file at 49,839 B with 45 ms of sync time.
- There are no on device numbers yet for dictionary, DELTA, BYTE_STREAM_SPLIT or a page index, and no energy numbers for any codec. The DELTA encoder exists since #135 step 1 but no board turns it on yet.

## Compaction simulation

[`compact-20261016.jsonl`](parquet-small-files/compact-20261016.jsonl) merges 16 real `cores3-telemetry-v3` files, 624 rows and 77 columns, into one file sorted by event time. On the device they take 620.7 B per row.

| Compacted variant | B per row | vs device |
|---|---|---|
| PLAIN, UNCOMPRESSED | 412.7 | −34 % |
| PLAIN, SNAPPY | 141.6 | −77 % |
| PLAIN, LZ4_RAW | 149.3 | −76 % |
| Dictionary, LZ4_RAW | 129.3 | −79 % |
| Dictionary, LZ4_RAW, page index | 131.8 | −79 % |
| PLAIN, ZSTD | 92.8 | −85 % |

The sample is small, so read it as a direction. At about 93 B per row a 256 MB file holds about 2.9 million rows, roughly 333 Thing days. Partitioning by Thing and day would recreate tiny files.

## Scale

- 900 s rotation gives 96 files and 8,640 rows per Thing per day.
- 1,000 Things write about 35 million files a year, and 30,000 Things about 1 billion.
- One Thing writes about 4.8 MB a day uncompressed, and about 0.9 MB of that is repeated footers.

## Proposals

Issue #25 tracks each one as a task. Item 2 is done, the others are not decided.

1. Keep the device writer small. One page per chunk, row group statistics, `field_id`, MICROS UTC and `sorting_columns` stay. No page index or bloom filters on device.
2. Make LZ4_RAW the persisted default. Done in #25 on the strength of the Arduino bench, the host survey and the phone reader. A paired `codec-test` bench on the current IDF builds is still owed to confirm the size and timing on device.
3. Move long static key value entries behind a short versioned URI and hash. This is a wire contract change and needs its own decision.
4. Prefer fewer row groups per file, since each one costs about 4.4 KB of footer.
5. Treat DELTA_BINARY_PACKED for integers as the largest future win. The phone reader supports it since 2026-10-18 and the firmware encoder landed off by default in #135 step 1, so the next step is the on device measurement in [#135](https://github.com/walkthru-earth/opensensor-space-firmware/issues/135). Measure dictionary RAM on device before using it.
6. Register v6.12 and v2.6 files with iceberg-go `AddFiles` as a raw tier, then rewrite them on the host into Iceberg v3 data files of about 256 MB, sorted by Thing, H3 cell and event time, with dictionary plus LZ4_RAW or SNAPPY, the page index on and bloom filters on `h3_cell_id`, `device_id` and a new `thing_id` column. The October 2026 survey called it `station_id`. #108 renamed it and changed nothing else.
7. Partition by month while the fleet is small, then move to day with partition evolution.
8. Keep anything the phone may read in LZ4_RAW or SNAPPY until iOS reads ZSTD. Keep device originals as an immutable archive.

## Phone reader limits

Updated 2026-11-04. In October the reader refused DELTA_* and BYTE_STREAM_SPLIT. That changed on 2026-10-18.

The opensensor-space-mobile `:core:parquet` reader reads UNCOMPRESSED, LZ4_RAW, SNAPPY and GZIP on iOS and Android, ZSTD on Android only, and PLAIN, RLE, dictionary, DELTA_BINARY_PACKED and BYTE_STREAM_SPLIT encodings. It reads a 64 KiB tail on local links, 128 KiB on cellular and 256 KiB on Wi Fi, and it reads the page index but not bloom filters.

- The DELTA and BYTE_STREAM_SPLIT decoders came from mobile [#94](https://github.com/walkthru-earth/opensensor-space-mobile/issues/94) in [PR #103](https://github.com/walkthru-earth/opensensor-space-mobile/pull/103) (commit `1da9474b`). They live in shared `commonMain` code, so iOS and Android run the same decoder. `EncodingFixtureTest` checks both bit for bit against PyArrow and DuckDB files, on the JVM and on the iOS simulator through `./gradlew verifyIos`.
- LZ4_RAW, dictionary, DELTA or BYTE_STREAM_SPLIT on device need no reader work. DELTA on device still needs the measurement in [#135](https://github.com/walkthru-earth/opensensor-space-firmware/issues/135).
- ZSTD on iOS, bloom filters, the metadata URI and a `thing_id` column all need reader work first.

## Rerun

```sh
pixi run size-survey survey <file.parquet> ...
pixi run size-survey encodings <file.parquet> ...
pixi run size-survey compact <file.parquet> ...
pixi run size-survey-test
```

`survey` prints one JSON line per file. `encodings` and `compact` round trip every variant and fail if the rows change.

## Sources

- [Parquet page index](https://parquet.incubator.apache.org/docs/file-format/pageindex)
- [Page index pruning in Iceberg](https://datalakehousehub.com/blog/parquet-page-index-pruning-iceberg/)
- [Low latency Parquet reads](https://brokenco.de/2025/06/24/low-latency-parquet.html)
- [Iceberg small files problem](https://www.dremio.com/blog/apache-iceberg-small-files-problem-causes-fixes-and-prevention/)
- [AWS S3 Tables compaction](https://aws.amazon.com/cn/blogs/storage/how-amazon-s3-tables-use-compaction-to-improve-query-performance-by-up-to-3-times/)
- [AWS guidance on Iceberg compaction](https://docs.aws.amazon.com/prescriptive-guidance/latest/apache-iceberg-on-aws/best-practices-compaction.md)
- [DuckDB Parquet bloom filters](https://duckdb.org/2025/03/07/parquet-bloom-filters-in-duckdb.html)
- [Apache Iceberg Go 0.6.0, RewriteDataFiles](https://iceberg.apache.org/blog/apache-iceberg-go-0.6.0-release/)
- [iceberg-go AddFiles for v3](https://github.com/apache/iceberg-go/pull/1101)
- [Apache Arrow Go 18.3.0, bloom filter write](https://arrow.apache.org/blog/2025/05/09/arrow-go-18.3.0/)
- [arrow-rs Compression enum](https://arrow.apache.org/rust/parquet/basic/enum.Compression.html)
- [Impala LZ4_RAW support](https://jira.apache.org/jira/browse/IMPALA-12108)
- [PyIceberg v3 write tracking](https://github.com/apache/iceberg-python/issues/1551)
