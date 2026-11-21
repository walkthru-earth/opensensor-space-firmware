# 0004 Internal flash archive when the card is missing

| | |
| --- | --- |
| Status | Accepted 2026-11-21 by the owner, being built |
| Date | 2026-11-21 |
| Issue | Owner request of 2026-11-21, tracked on the pull request that builds it |
| Code | `firmware/common/logger/src/os_logger.cpp`, `firmware/common/logger/src/storage_governor.h`, each board's `partitions.csv` |
| Related | [Telemetry pipeline](../telemetry-pipeline.md), [BLE sync protocol](../ble-sync-protocol.md), [0001](0001-standalone-thing-stac.md), #144 (cloud upload) |

## Context

A board without a usable microSD card keeps one RAM batch of at most 90 rows
(`kMaxBatchRows` in `os_logger_cadence.h`), about 15 minutes at the 10 s
cadence. After that the worker drops rows, and a reset loses the batch. A
pulled card, a dead card or a board shipped without one therefore loses data
even when the phone or laptop is right there on Wi-Fi or BLE.

Every 16 MB board already carries an unused data partition, label `spiffs`,
at 0xc20000 with 0x3e0000 bytes (3.875 MiB). The CrowPanel carries the same
label at 0x7a0000 with 0x60000 bytes (384 KiB). No tool writes or erases it
today.

The owner asked on 2026-11-21 that data is never lost for want of a card.
Logging goes on into internal memory, the phone and laptop sync it as usual,
and the files move to the card once a card is back.

## Decision

1. **The flash volume.** The `spiffs` partition becomes a FAT volume over
   ESP-IDF wear levelling, mounted at `/flash` next to the card at `/sd`.
   The partition table, label, subtype, offsets and sizes stay as they are.
   ESP-IDF 6.1 finds a labelled FAT partition by label with any subtype
   (`components/fatfs/vfs/vfs_fat_spiflash.c`), so no partition or tooling
   changes are needed and LAN OTA images keep the same layout.
2. **Format once.** A mount that fails with `ESP_FAIL` is retried once
   without a format, so a transient fault never spends the format. When the
   retry fails too, the partition is formatted and that is recorded in NVS (new namespace `osflash`, key `formatted`). After that
   flag is set, a failed mount is an error that is logged and leaves the
   volume off. The firmware never formats it again by itself and never
   touches the card, NVS or any other partition. A factory image may hold
   old bytes in this partition, and the board backup taken before each flash
   keeps them.
3. **Board opt in.** The shared logger gets the second root through its
   `Config` and a Kconfig switch. Each board mounts the volume itself through
   a shared helper, as it mounts the card. There are no board-name
   conditionals in `firmware/common`.
4. **Where a new file goes.** The worker picks the root when it creates a
   file. The card while storage is Ready, otherwise the flash volume while it
   is mounted and has room, otherwise no file (rows stay in the RAM batch as
   today). A file keeps its root until it is finalized. A storage probe that
   finds the card back finalizes an open flash file, so the rows waiting in
   RAM write a card file before the card counts as Ready. Paths below each root
   are the same Hive layout, `thing=<id>/year=/month=/day=/` or
   `thing=<id>/unsynced/boot=<boot>/`, with the same file names.
5. **Finalize is unchanged.** Flash files use the same `.partial`, fsync,
   readback, CRC32 and SHA-256, no-replace rename and STAC sidecar as card
   files. A reset during a flash write leaves a `.partial` that the existing
   provenance quarantine moves aside on the flash volume.
6. **Flash full.** When the flash volume falls below a reserve of one full
   file, the worker stops creating files there, keeps the RAM batch and then
   drops rows as it does without a card. It logs the condition and resumes
   when room returns. It never deletes a flash file to make room.
7. **Sync reads both roots.** LIST walks the card root, the legacy root and
   then the flash root. A flash file is listed under the same relative name
   it will have on the card, so the phone, which keys a file by Thing and
   relative name, sees one file whether it sits on flash or card. A name on
   both roots is listed once. OPEN and READ look on the card first, then on
   flash. LIST and OPEN answer ERROR 8 only when neither root is usable.
   LIST_SINCE never leaves out a file the peer may not have seen. When flash
   files exist or a copy has moved files onto the card, it falls back to the
   full listing.
8. **Copy back.** When the card is Ready and the flash volume holds files,
   the worker copies them one at a time in small chunks, between its other
   jobs, never during a LIST job or an emergency close. Each copy goes to a
   temporary name on the card, is fsynced, read back and checked against the
   CRC32 and SHA-256 of the flash file, then renamed into place only if the
   final name is absent. Parquet goes before its sidecar, and quarantined
   files go to the card's quarantine with the same relative path.
9. **The one allowed delete.** After a verified copy the worker deletes the
   flash file. This is the only automatic delete of owner data, and it
   happens only when an identical copy is on the card. If the card already
   holds a file of that name with different bytes, both stay, the worker logs
   a conflict and never overwrites either. A stale temporary copy on the card
   holds no unique rows, because its source is still on flash, so the worker
   removes it and starts that copy again.
10. **Cloud stays out.** Upload from the board is #144 and is not part of
    this record.

## Rule changes

The owner approved these for this feature on 2026-11-21.

- "The card is the origin" becomes "the card, or the internal flash volume
  while no card is usable, is the origin". The phone or laptop archive is
  still the first copy.
- The worker may delete a flash file after an identical, verified copy is on
  the card. Every other rule against deleting, repairing or rewriting owner
  data stays.

## Limits

- Capacity is small. A 15 minute window of the CoreS3 schema measured
  49,839 bytes ([0002](0002-small-parquet-files.md)), so the 16 MB boards hold
  roughly 75 windows, about 19 hours. The CrowPanel `spiffs` partition is
  0x60000 bytes (384 KiB). After the 128 KiB `fallback_reserve_bytes` and the
  FAT and wear levelling overhead it holds about 3 to 4 windows, about an
  hour. Wear levelling and FAT also take a little of the 16 MB boards.
- Each flash erase or write stalls the instruction cache for a few
  milliseconds. The 10 s monotonic sampling and the watchdog rules stay. A
  bench test must show no sample gaps.
- Flash wear is spread by wear levelling. At one window per 15 minutes the
  writes per day are far below the rated cycles, but no endurance test has
  run.
- Power-cut safety of FAT over wear levelling on these boards is not proven
  until a dated bench record shows it.

## What would reopen it

- A board whose free partition is too small to hold one window.
- A bench run that shows sample gaps, lost files or a failed copy.
- A need to show the flash state on the phone. STATUS has about 3 bytes left
  in its 480 byte budget, so a new key needs its own change.
