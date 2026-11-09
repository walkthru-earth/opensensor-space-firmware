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
