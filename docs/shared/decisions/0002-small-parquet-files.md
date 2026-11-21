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
- No ZSTD in any file the phone may read. The phone reads ZSTD on Android only.
- No dictionary encoding until there are on device RAM numbers for it next to the
  TLS and BLE buffers.
- No footer diet that moves static entries behind a URI. Decision 16 requires
  every uploaded file to describe itself. The template link and digest in the
  `opensensor` key already point at the long column metadata.
- No compaction, rewrite or relabel of finalized files on the board.
- No Iceberg manifests, manifest lists or catalog commits on the board.
- No rotation window longer than one hour.

## What moves to a cloud compactor

These are host or cloud jobs, outside the firmware. A host prototype of items 2
to 6 is `tools/compact_archive.py` (#136), measured in the next section. Item 1
and the Iceberg table are not built yet.

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

## Measured answers (#136)

Measured on 2026-11-21 on one Apple silicon laptop with the pinned Pixi DuckDB
2.0.0 and the DuckLake extension build 417a9117f5. Inputs were written by the
board Parquet writer itself, through `tools/compactor_fixture.cpp`, so every
source file has the device codec, footer, field ids and row group shape. No
board took part and nothing here is a bench claim.

- The gate fixture in `pixi run compact-test` has 3 Things on 2 boards across a
  month boundary, 2,295 rows, plus an unsynced file and a legacy file without
  field ids. It checks the refusals and the verify step.
- The fleet corpus has 18 Things for 7 days of 900 s windows, 9 CoreS3 Things
  with an H3 cell and 9 CrowPanel Things without location. That is 12,096
  files, 1,088,640 rows and 210,257,553 bytes.
- One real Thing file from a CoreS3 artifact (71 rows, 20,535 bytes) compacted
  to 19,526 bytes with the content check passing. The source hash did not
  change. A legacy SD export from before the Thing baseline was refused, since
  it has no dated Thing path.

### Plain Hive compaction

`pixi run compact compact` reads the dated tree with `read_parquet`, explicit
`hive_types` and `union_by_name`, and writes one file per board and month (or
day) with `COPY`. Output is LZ4_RAW with V2 data pages, row groups of 122,880
rows, sorted by Thing, H3 cell and event time. Every device column keeps its
field id and logical type, so the UTC columns stay `TIMESTAMP(MICROS, UTC)`. A
new `thing_id` UUID column takes host field id 1001. DuckDB adds dictionary
encoding, DELTA_BINARY_PACKED on integers and bloom filters on the dictionary
columns. It writes no page index. Before a file is published the tool checks
the row count, a multiset hash of every row, an exact `EXCEPT ALL` both ways,
the field ids, the logical types, the codec, and that no source changed during
the run. A failed check publishes nothing.

| Fleet corpus | Files | Bytes | Wall time | Peak RSS |
| --- | ---: | ---: | ---: | ---: |
| Device files | 12,096 | 210,257,553 | | |
| Month partitions | 2 | 46,312,826 | 74 s | 3.6 GB |
| Day partitions | 14 | 50,105,342 | 69 s | 1.25 GB |

The month files are 31,769,781 bytes for CoreS3 and 14,543,045 bytes for the
CrowPanel, 4.5 times smaller than the sources together. Day files are 2.1 to
5.1 MB. Two runs gave byte identical output. Of the 74 s, 26.5 s went to
reading 12,096 footers and hashing the sources, and the verify steps took
longer than the copy itself.

### Reader benchmark (DR item 6, DuckDB only)

Median of 5 warm runs on the local disk. Opens, reads and bytes are DuckDB file
system counters. Every reader returned the same answer hash.

| Query | Device files | Month files | Day files |
| --- | ---: | ---: | ---: |
| Fleet summary per Thing | 4,632 ms, 36,288 opens, 176.8 MB | 8.7 ms, 6 opens, 2.3 MB | 11.3 ms, 42 opens, 2.9 MB |
| One Thing, one day | 1,625 ms, 13,440 opens, 169.5 MB | 5.5 ms, 6 opens, 0.37 MB | 10.6 ms, 42 opens, 0.47 MB |
| One H3 cell | 4,400 ms, 36,288 opens, 169.2 MB | 3.9 ms, 6 opens, 0.12 MB | 5.8 ms, 42 opens, 0.23 MB |

A reader that knows the Thing and day reads one folder of 96 files in 28.8 ms
(193 opens, 1.6 MB). `union_by_name` opens every footer even when the Hive
filters prune the paths, which is why a fleet query over the device tree opens
every file.

### DuckLake

`pixi run compact ducklake` copies the sources into a scratch catalog,
registers them with `ducklake_add_data_files`, merges them and then runs the
usual maintenance (`expire_snapshots`, `cleanup_old_files`).

| Fleet corpus, per board | CrowPanel | CoreS3 |
| --- | ---: | ---: |
| Register 6,048 files | 30.4 s | 57.2 s |
| Merge to 1 file | 3.4 s, 14,557,193 B | 4.5 s, 31,806,817 B |
| Registered copies left after cleanup | 0 | 0 |
| Sorted copy with `thing_id` | 3.3 s | 6.2 s |

- Maintenance deleted every registered file. Pointed at the archive, DuckLake
  would delete the only raw copy, which item 5 forbids. Only a copy can be
  registered.
- The merged file kept the device field ids. A table that DuckLake writes
  itself, like the sorted copy, gets field ids that DuckLake assigns, so the
  device ids from the dictionary are lost.
- The registered files carry no Thing column. The Thing lives only in the path
  and footer, so a registered raw table cannot filter by Thing.
- The catalog for 12,096 files was 15.2 MB.

### Iceberg

Not run. Writing an Iceberg table needs a catalog, a REST service or a cloud
table service, and the owner has not picked one (question 1). The DuckDB
Iceberg extension and iceberg-go `AddFiles` both need that catalog. The same
hazard applies as with DuckLake, so item 5 stands. Iceberg maintenance must
own a copy, never the device originals.

### Answers the measurements support

1. Where the compactor runs. A plain DuckDB `COPY` into its own Hive prefix
   works on our own host with no catalog and no service, keeps field ids and
   UTC types, is deterministic and verifies itself. A month of an 18 Thing
   fleet fits on one machine. DuckLake is not fit to own the raw tier, because
   its cleanup deletes registered files and its own writes renumber field ids.
   Still open is who owns a catalog, if any, and whether the job later moves to
   a cloud function. The Hive output can be registered in Iceberg later without
   a rewrite.
2. Public tier. Serve both. The device files stay the immutable archive and the
   only copy the board signs off on. The compacted prefix is what readers
   should query, since it answers a fleet query about 500 times faster with
   about 6,000 times fewer file opens. Whether the raw tier is public or kept
   private is still the owner's call.
3. Month or day. Month at launch. A week of the 18 Thing fleet gives 14.5 to
   31.8 MB per board, so a 31 day month projects to about 64 MB (CrowPanel) and
   140 MB (CoreS3) for 9 Things each, under the 256 MB target of item 2. That is
   about 15.6 MB per CoreS3 Thing per month and 7.2 MB per CrowPanel Thing. A
   board-month reaches 256 MB at about 16 CoreS3 Things or 36 CrowPanel Things,
   which is the point to switch that board to day partitions.
5. Phone day compaction. Keep both, under separate prefixes. The compactor reads
   only dated device files and refuses its own output prefix inside the input
   tree, so a phone derived file can never be read back as a source. It needs no
   phone output to work.

Question 4 (a ZSTD tier) was not measured and stays open, as does the cost on a
cloud object store, a phone reader against the compacted files, and the
compactor's memory at a fleet larger than this corpus.

## Owed measurements

These do not reopen the decision. They are bench steps and go to the hands-on
issue #61 with the other bench work.

- A paired UNCOMPRESSED against LZ4_RAW bench on the current IDF builds of both
  boards, recording file bytes, writer time, sync time and peak heap. The LZ4_RAW
  default rests on the Arduino era CoreS3 bench, the host survey and the phone
  reader.
- One real file per rotation setting on the current firmware, since the survey
  predates the Thing baseline and the `opensensor` footer.

## Reopen when

- Met on 2026-10-18. The phone reader decodes DELTA_BINARY_PACKED and
  BYTE_STREAM_SPLIT on both iOS and Android (mobile PR #103, covered by
  `verifyIos`). The DELTA integer measurement on device is
  [#135](https://github.com/walkthru-earth/opensensor-space-firmware/issues/135).
- Not met yet. The DELTA measurement in #135 shows it fits the writer time,
  heap and image budget on both boards.
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
