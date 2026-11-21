# 0002 Small Parquet files, compaction and range reads

| | |
| --- | --- |
| Status | Proposed, pending owner sign off |
| Date | 2026-10-27 |
| Issue | [#25](https://github.com/walkthru-earth/opensensor-space-firmware/issues/25) |
| Evidence | [Small Parquet files](../parquet-small-files.md) and its raw results in [`parquet-small-files/`](../parquet-small-files/) |
| Related | [Table and observation model](../table-and-observation-model.md), [0001](0001-standalone-thing-stac.md), bridge decisions 13, 14 and 16 |

## Context

Boards write one small Parquet file per UTC window. At the default 900 s
rotation that is 96 files per Thing per day. The October 2026 survey read 145
entries from the local bench artifacts. 144 are device files, 90 of them unique
by name and size, and one is a directory recorded as an error. It found partial
files where the footer is 68 to 80 percent of the bytes, and a full 90 row
CoreS3 window of 49,839 B with a 19.2 percent footer.
The numbers, method and rerun commands are in
[Small Parquet files](../parquet-small-files.md). Issue #25 asked how far the
device writer should go, and what moves to a host or cloud compactor.

The owner decided on 2026-10-27 that #25 becomes a decision record instead of
code, see the
[#53 merge plan](https://github.com/walkthru-earth/opensensor-space-firmware/issues/53#issuecomment-6428830174). What follows is a proposal until the owner signs it off on #25.

## What the firmware does now

| Behaviour | Where |
| --- | --- |
| One PLAIN DataPageV1 per column chunk, RLE definition levels, exact min and max per chunk | `firmware/common/src/parquet_writer.cpp` |
| No page index and no bloom filter, with the reason in the header comment | `firmware/common/src/parquet_writer.h` |
| LZ4_RAW is the default when no codec is saved, and the choice persists in NVS key `parquet/codec` | `firmware/common/logger/src/os_logger_codec_store.h` |
| At most 90 rows per row group, which is also the RAM loss window | `cadence::kMaxBatchRows` in `firmware/common/logger/src/os_logger_cadence.h` |
| At most 8 row groups per file | `kMaxRowGroups` in `firmware/common/src/parquet_writer.h` |
| Rotation of 600, 900, 1800 or 3600 s, never longer than one hour | bridge decision 16, `docs/shared/telemetry-pipeline.md` |
| Stable `field_id`, append only from the Thing baseline, `TIMESTAMP(MICROS, UTC)`, `sorting_columns` on the sequence column | `docs/shared/table-and-observation-model.md`, `docs/shared/parquet-small-files.md` |
| One `opensensor` footer key of compact JSON plus the `geo` key, instead of the 37 to 42 flat keys the survey measured | #88, `docs/shared/table-and-observation-model.md` |
| Footer decoding into Iceberg v3 `DataFile` metrics, host verified, not called by any board | `firmware/common/src/parquet_reader.h` |
| DELTA_BINARY_PACKED for INT32 and INT64 behind the Kconfig switch `CONFIG_OS_LOGGER_DELTA_INTEGERS`, default n, on in no board | `firmware/common/logger/Kconfig`, `firmware/common/logger/src/os_logger_encoding.h` |

Finalized files are immutable and the card is the origin. Each uploaded file
stands alone with a full footer (bridge decision 16).

## What the firmware will not do

- No page index and no bloom filters on device. With one page per chunk and
  about 90 rows per group a page index repeats the chunk statistics and adds 1.6
  to 1.7 KB per file.
- No DELTA_BINARY_PACKED and no BYTE_STREAM_SPLIT yet, although DELTA integers
  were the largest host win in the survey. The phone reader decodes both on iOS
  and Android since 2026-10-18 (mobile
  [#94](https://github.com/walkthru-earth/opensensor-space-mobile/issues/94),
  [PR #103](https://github.com/walkthru-earth/opensensor-space-mobile/pull/103)).
  What is left is the on device measurement in
  [#135](https://github.com/walkthru-earth/opensensor-space-firmware/issues/135).
  Updated 2026-11-04. In October this line said the phone reader refused both.
  Updated 2026-11-09. The writer has a DELTA_BINARY_PACKED integer encoder
  since #135 step 1. It is off by default and no board turns it on, so this
  line still holds for every file a board writes.
  Updated 2026-11-21. Since #135 step 2 every board can turn it on at build
  time with `CONFIG_OS_LOGGER_DELTA_INTEGERS`. The switch defaults to n and no
  board sets it, so this line still holds. What the host proves and what the
  board still owes is in DELTA integers below.
- No ZSTD in any file the phone may read. The phone reads ZSTD on Android only.
- No dictionary encoding until there are on device RAM numbers for it next to the
  TLS and BLE buffers.
- No footer diet that moves static entries behind a URI. Decision 16 requires
  every uploaded file to describe itself. The template link and digest in the
  `opensensor` key already point at the long column metadata.
- No compaction, rewrite or relabel of finalized files on the board.
- No Iceberg manifests, manifest lists or catalog commits on the board.
- No rotation window longer than one hour.

## DELTA integers

Added 2026-11-21 for #135 step 2. Turning DELTA on for a board is a one line
change in that board's `sdkconfig.defaults`, made only after the owner signs
off on the board numbers below.

### Proven on the host

- The encoder writes INT32 and INT64 values as DELTA_BINARY_PACKED in blocks
  of 128 values and 4 miniblocks of 32. Every value stream is byte equal to a
  Python encoder written from parquet-format 2.14.0 Encodings.md, and every
  single group INT32 stream is byte equal to the one PyArrow writes
  (`pixi run parquet-test`).
- PyArrow and DuckDB read every DELTA file bit for bit equal to its PLAIN twin,
  and every integer edge column equal to absolute values from an independent
  Python copy of the fixture generator. The edges are 1, 2, 3, 32, 33, 90, 128,
  129, 130, 257 and 1000 rows, nulls in a run and every third row, all null
  columns, INT32 and INT64 min and max on alternating rows, deltas that need
  the full 32 or 64 bits, random values and zero width miniblocks, with and
  without LZ4_RAW.
- Footer null_count, min_value and max_value match the generator, and every
  footer field outside the page layout equals the PLAIN twin. FLOAT, GEOMETRY
  and the RLE definition levels stay as they are.
- The worst case INT64 page equals `delta_binary_packed_bound()`, LZ4 scratch
  one byte short fails closed with no byte written, and an unknown encoding is
  refused before any byte is written.
- The board switch, off and on, through `create_file()`'s own source of the
  encoding (`pixi run logger-encoding-test`). Off is the writer default byte
  for byte. On changes only the integer chunks, keeps every statistic and reads
  back the same values in both readers. No shipped `sdkconfig.defaults` sets
  it, and the `bench-delta` CI leg compiles the CoreS3 and Waveshare V2 images
  with it on.
- On host fixtures shaped like the CoreS3 dictionary, DELTA plus LZ4_RAW is 35
  percent smaller than PLAIN LZ4_RAW for one 90 row group and 38 percent for
  four, see [Small Parquet files](../parquet-small-files.md).

### Owed on the board, in #61

- Paired 900 s windows on CoreS3 and Waveshare V2, the shipped PLAIN LZ4_RAW
  image against the `OS_BENCH=delta` image, with file and footer bytes,
  `writer_us` per row group, finalize and sync time, and free heap, PSRAM and
  worker stack watermarks. The host fixture is synthetic, so its sizes are a
  direction, not a device figure.
- The image size and OTA slot share of the bench image against the shipped
  image on each board.
- A DELTA file pulled over BLE and over LAN on Android and on iOS, opened in
  phone History, with the values compared against the card copy.
- On a cardless board, the flash spool volume per day with DELTA against
  PLAIN ([decision 0004](0004-flash-fallback-archive.md)).

### Before any board turns it on

- The CrowPanel chart history restore decodes PLAIN pages only through
  `parquet_columns.h`. Its build stops when the switch is on, and that stays
  until the decoder reads DELTA_BINARY_PACKED.
- `tools/rebuild_partial.py` rebuilds PLAIN partials only. A DELTA partial is
  refused as torn, so recovery of a reset window needs DELTA page support
  first.
- A matched phone build with a newest-first entry in
  [mobile migration](../mobile-migration.md), and every board of that kind
  reflashed.
- BYTE_STREAM_SPLIT for FLOAT is not built. It would follow the same switch
  pattern as its own Kconfig option after the DELTA numbers are in.

## What moves to a cloud compactor

These are host or cloud jobs, outside the firmware. None of them is built yet.

1. Register device files as a raw tier, for example with iceberg-go `AddFiles`,
   which reads footers and assigns row lineage on v3. Older files without field
   ids are not carried over, because legacy data is wiped after the matched
   reflash (bridge decision 14).
2. Rewrite into Iceberg v3 data files of about 256 MB, sorted by Thing, H3 cell
   and event time, with dictionary plus LZ4_RAW or SNAPPY, the page index on and
   bloom filters on `h3_cell_id`, `device_id` and a new `thing_id` column. The
   Thing UUID lives only in the path and footer on the device.
3. Partition by month while the fleet is small, then move to day with partition
   evolution.
4. Keep any file the phone may read in LZ4_RAW or SNAPPY. A ZSTD tier is for host
   only analytics.
5. Keep compacted output under its own prefix, never mixed with board files
   (bridge decision 13). Keep the device originals as an immutable archive that
   Iceberg maintenance does not own, so a table expiry can never delete the only
   raw copy.
6. Run the reader benchmark from #25 Phase 4 against the compactor output. Compare
   request count, bytes, time to first row and total time for N device files
   against one compacted file, from the phone and from DuckDB.

The compacted numbers in the survey come from 16 files and 624 rows, so they show
a direction, not a target.

## Owed measurements

These do not reopen the decision. They are bench steps and go to the hands-on
issue #61 with the other bench work.

- A paired UNCOMPRESSED against LZ4_RAW bench on the current IDF builds of both
  boards, recording file bytes, writer time, sync time and peak heap. The LZ4_RAW
  default rests on the Arduino era CoreS3 bench, the host survey and the phone
  reader.
- One real file per rotation setting on the current firmware, since the survey
  predates the Thing baseline and the `opensensor` footer.
- The paired PLAIN and DELTA bench in DELTA integers above.

## Reopen when

- Met on 2026-10-18. The phone reader decodes DELTA_BINARY_PACKED and
  BYTE_STREAM_SPLIT on both iOS and Android (mobile PR #103, covered by
  `verifyIos`). The DELTA integer measurement on device is
  [#135](https://github.com/walkthru-earth/opensensor-space-firmware/issues/135).
- Not met yet. The DELTA measurement in #61 shows it fits the writer time,
  heap and image budget on both boards. The switch and the host proof landed
  in #135 step 2.
- iOS reads ZSTD. Then compare ZSTD against LZ4_RAW on device.
- On device numbers show a dictionary page fits next to TLS and BLE on both
  boards.
- An energy measurement on battery or solar shows LZ4_RAW costs more than the
  bytes it saves in upload and storage.
- Field data from deployed boards shows partial files or footer share dominate
  upload cost, which would reopen row group sizing and the footer size.
- The writer moves to several pages per chunk, which would make a page index
  useful.

## Open questions for the owner

1. Where does the compactor run, our own host, a cloud function or a managed
   table service, and who owns the catalog?
2. Does the public tier serve compacted files only, or also the raw device files?
3. Month or day partitions at launch, and at what fleet size does it switch?
4. Is a separate ZSTD tier for host analytics worth the extra storage?
5. Does the cloud compactor replace the phone's day compaction (bridge question
   38), or do both write derived files under separate prefixes?

Questions 1 to 4 come from #25. The survey's question on the footer diet is
answered above by decision 16, and the questions on field file sizes, LZ4 energy
and dictionary RAM became reopen criteria.
