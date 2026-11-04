# 0001 Who writes STAC for a standalone Wi-Fi Thing

| | |
| --- | --- |
| Status | Accepted 2026-11-04 as owner decision OD-2026-11-04-D. The board writes STAC. This supersedes the option C proposal below |
| Date | 2026-10-27, amended 2026-11-04 |
| Issue | [#109](https://github.com/walkthru-earth/opensensor-space-firmware/issues/109) |
| Related | [#81](https://github.com/walkthru-earth/opensensor-space-firmware/issues/81) uploader, [#83](https://github.com/walkthru-earth/opensensor-space-firmware/issues/83) STAC epic, mobile #181 and #190, walkthru-earth/stac-opensensor#1 |
| Spec | stac-opensensor `PROFILE.md` Writers section at [`83787768cc8451109d48a03d4ef5493ffcd403dd`](https://github.com/walkthru-earth/stac-opensensor/blob/83787768cc8451109d48a03d4ef5493ffcd403dd/PROFILE.md#writers). The 2026-10-27 analysis read `PROFILE.md` at `815fc45cce64d477ea4268f42ace79aed8877a98` (bridge decisions 33 and 40) |
| Bridge | decisions 12, 13, 16, 33 and 40 in `.handoff/ecosystem-bridge.md` |
| Register | OD-2026-11-04-A to OD-2026-11-04-D in the [stac-opensensor decision register](https://github.com/walkthru-earth/stac-opensensor/blob/83787768cc8451109d48a03d4ef5493ffcd403dd/docs/decisions.md) |

## Decision

The owner decided on 2026-11-04 that the board firmware writes STAC
(OD-2026-11-04-D). This replaces the option C proposal. This is not option D
in the options table below. It is closest to option B, with the Collection
built from the upload ledger instead of a bucket read. The analysis from
2026-10-27 stays below as history.

| Piece | Writer | How |
| --- | --- | --- |
| File Item | Board | Written at file close as a sidecar `<file>.json` next to `<file>.parquet` on SD. Only for dated `data_HHMM_` files that pass the PROFILE no-Item rules. The data asset href is relative, `./<file>.parquet` |
| Thing Collection | Board | Written after each upload from the board's own upload ledger and the owner config. The board is the only writer under `thing=<uuid>/` and never reads the bucket. Title, description, providers, contacts, the bucket and prefix of `partition:glob`, and column `title` and `opensensor:scale` come from the owner config set at setup |
| Root Catalog | Catalog host | Only the catalog host's publish job writes it, when a Thing joins or leaves |
| `items.parquet` | Catalog host, optional | Rebuilt from the Item JSON |
| Regenerated STAC | Phone and indexer, optional | They may rebuild the File Item and the Thing Collection, and the result must equal the board's output. The Root Catalog has no board output to match |

The Item is meant to travel with its Parquet. No transport can carry it today.
Sidecar transport over the board uploader and over BLE and LAN is future work,
tracked in a separate issue. Undated files stay on the card with no Item, as
before.

The board stays the only Collection writer. A file the phone publishes after a
BLE or LAN sync enters the Collection only when the board learns of that
upload. How the board learns is future work, tracked in the same separate issue
as sidecar transport over BLE and LAN. It could be an upload receipt the phone
sends back to the board over BLE or LAN, or the board reading its own prefix.
Until then such an Item is published but not listed in the Collection. Readers
find it by listing the `thing=<uuid>/` prefix.

Three related owner decisions came the same day. v0.1.0 of the spec is re-cut
and keeps its schema URLs (OD-2026-11-04-A). Every published Thing, fixed or
moving, uses H3 resolution 8 (OD-2026-11-04-B). A template href may be any
immutable URL whose bytes hash to `template.sha256`, with this repo pinned to a
full commit as the canonical host (OD-2026-11-04-C).

The on device writer is not built yet. It is a separate firmware issue that
measures flash, internal RAM and PSRAM on both boards before it lands. It must
follow the PROFILE [Writers](https://github.com/walkthru-earth/stac-opensensor/blob/83787768cc8451109d48a03d4ef5493ffcd403dd/PROFILE.md#writers) and
[Canonical form](https://github.com/walkthru-earth/stac-opensensor/blob/83787768cc8451109d48a03d4ef5493ffcd403dd/PROFILE.md#canonical-form) sections at the pinned commit, so a
regenerated document equals the board's.

### Facts that still constrain the implementation

These come from the analysis below and from the code, and they still hold.

- Board keys are upload only and every put sends `If-None-Match: *`. The board
  never lists or reads the bucket. So the Collection must come from the SD
  upload ledger, not from a read, modify and write. A Collection changes with
  every file, so the implementation must also say how a new Collection replaces
  the old one under keys that refuse overwrites.
- The uploader (`finalized_dated()` in
  `firmware/common/upload/src/os_upload_policy.cpp`) and the BLE and LAN name
  rule ([`ble-sync-protocol.md`](../ble-sync-protocol.md), names end in
  `.parquet`) accept only Parquet today. Carrying the sidecar needs an uploader
  change and a wire contract change that mobile agrees to. Both are future work
  in a separate issue.
- `location.resolution` in config still accepts 0 to 15, with 8 as the default
  ([`ble-sync-protocol.md`](../ble-sync-protocol.md),
  [`time-and-location.md`](../time-and-location.md)). Under OD-2026-11-04-B
  published files and Items carry resolution 8. The PROFILE gives a file no
  Item when a cell is not resolution 8. The uploader has no such check, so it
  would still send that Parquet. Narrowing the configurable range is a code
  follow-up with its own issue.
- Field boards upload to a private intake product, and a publish Action copies
  each file to the public product (#81 stage 4). The Action must copy the board's
  Item and Collection unchanged. Relative hrefs keep them valid after the copy.
- The logger starts a new file when the configured cell, resolution, country
  or mobility changes (`same_location` in
  `firmware/common/runtime/src/device_config.cpp`). So a fixed Thing file holds
  one cell. A moving Thing file may hold the cells of several fixes, and its
  Item geometry is their union.
- The PROFILE builds Item geometry with H3 `cellsToMultiPolygon` from H3 C
  4.5.0, the version vendored here. It is in the vendored sources but is not
  linked today. `firmware/common/location/CMakeLists.txt` relies on section GC
  to drop it. An Item writer pulls it in, and its flash and RAM cost is not
  measured.
- The PROFILE asks the board to keep the Collection extent summary, the first
  start time and the union box, in NVS, so an SD swap or a lost ledger does not
  reset it. The board has no such NVS state yet. This is future work for the
  File Item sidecar issue.
- The PROFILE says a board uploads each file as soon as its window closes. No
  board wires the uploader yet.
- The uploader waits for trusted UTC, so only dated files leave the board.

### Open questions answered

- Question 1. No. The board writes STAC, not the publish Action.
- Question 3. The phone may keep writing STAC, but its output must equal the
  board's output. The same holds for any indexer.
- Question 4. Owner only Collection fields come from setup config.
- Questions 2, 5, 6 and 7 move to the implementation issue. Question 2 now asks
  where a regenerating indexer lives, if one is built.

## History, 2026-10-27 analysis

Everything below is the original proposal. Its recommendation of option C was
not taken.

### Context (history)

A Wi-Fi Thing can run with no phone. It logs to SD and, once #81 lands, uploads
each closed file of at most one hour by itself (bridge decision 16). Bridge
decision 12 says only Parquet leaves the board and a footer reading indexer
builds STAC, with the phone writing STAC for its own uploads until that indexer
exists. No indexer exists yet and a standalone Thing has no phone, so its files
would land on source.coop with no STAC at all.

The spec names four pieces.

| Piece | Path | Changes when |
| --- | --- | --- |
| File Item | `thing=<uuid>/year=YYYY/month=MM/day=DD/<file>.json` | once per uploaded file, never after |
| Thing Collection | `thing=<uuid>/collection.json` | every file (extent, summaries), a mobility or location change, a template change |
| Root Catalog | `catalog.json` | a new Thing appears |
| Item mirror, optional | `thing=<uuid>/items.parquet` | every file, exactly in sync with the Item JSON or absent |

PROFILE already places the Thing Collection with a "catalog publisher" and says a
file stands alone, so an indexer can build its Collection and Item from the
footer, the rows and the template it names. PROFILE also publishes only dated
`data_HHMM_` files, and files written before the board knew UTC get no Item until
they are reconciled.

### Facts that constrain the choice (history)

These come from the #81 plan and its comments, the firmware and the spec.

- Board credentials are upload only (owner, 2026-10-19 on #81). No read, no
  delete. On every target the board sends `If-None-Match: *` and keeps its
  upload ledger on SD, so it never lists or reads the bucket. A Collection
  needs a read, modify and write on every file, which an upload only key cannot do.
- source.coop grants are per product, `ReadOnly` or `FullAccess`, with no prefix
  scoping (docs.source.coop, read 2026-10-19). Until an upload only role exists,
  field boards write to a private intake product and a GitHub Action signed in
  through OIDC validates each file and copies it to the public product (#81
  stage 4). That Action is already planned to update the catalog.
- The uploader skips uploads until UTC is trusted, because SigV4 rejects a skewed
  clock (#81 stage 3). Undated files stay on the SD card, so a standalone node
  has no file that needs reconciling before it can get an Item.
- The firmware already starts a new file on any change of location cell,
  resolution, country or mobility (`same_location` in
  `firmware/common/runtime/src/device_config.cpp`). One file never spans two
  fixed cells, so a file level Item geometry is always well defined.
- Every finalized file carries the `opensensor` footer key and the `geo` key
  (`docs/shared/table-and-observation-model.md`), so the Item and Collection
  inputs are all inside the file plus the template it names.
- The vendored H3 v4.5.0 sources include `cellToBoundary`, but
  `firmware/common/location/CMakeLists.txt` relies on section GC to drop unused
  polygon code, and no board calls it today. A board Item writer would pull it in.
  Its flash and RAM cost is not measured.

### Options (history)

| Option | Board writes | Indexer writes | Main cost or risk |
| --- | --- | --- | --- |
| A | File Item | Collection, Catalog, mirror | JSON writer, H3 boundary, SHA-256 and a frozen profile version in the image. The Item lands in intake, not public, so the Action still has to copy it |
| B | File Item and its Collection | Catalog | Needs read access and a conditional read, modify and write per upload. Conflicts with upload only keys. Two writers can lose updates without `If-Match` |
| C | Nothing, Parquet only | Item, Collection, Catalog, mirror | Needs a job that runs and an owner for it. Matches decision 12 |
| D | File Item as a cache | Rewrites Items and Collections, its output is the reference | Pays the on device cost of A and still needs the job of C |

### Recommendation (history, proposal, superseded)

Option C, with the #81 stage 4 publish Action as the indexer. This is a proposal
pending owner sign off on #109.

1. The board writes Parquet only. Each uploaded file is self describing, as
   decision 16 already requires.
2. The publish Action reads each new intake file, validates schema, `dict`,
   `desc` and Thing, copies it to `thing=<uuid>/...` in the public product and
   then writes, in this order, the File Item beside the file, the Thing
   Collection, and the root Catalog link for a new Thing. The Item mirror is
   written only if it can be kept exactly in sync.
3. Writes run Parquet first and Item second, so an Item never points at a missing
   file. The Item id is the file name, so a retry of the same file overwrites the
   same Item and never adds one. A re-upload with a new `<attempt>` is a new file
   name and gets its own Item over the same rows, so consumers dedupe by Thing
   plus time (decision 16).
4. The Action is the only writer of Collections and the Catalog. That removes the
   multi writer race from option B without needing conditional writes on
   source.coop.
5. The phone keeps writing STAC for the files it uploads until the indexer runs,
   as decision 12 says. After that the indexer's output is the reference for
   every Thing, phone synced or standalone.
6. The indexer builds every file derived value from the Parquet footer, the rows
   and the template. It must be able to rebuild every Item and the file derived
   parts of each Collection from the files alone, so any STAC a board or phone
   wrote is at most a cache. Collection fields the files do not carry (provider
   names, owner contact, license text, titles) need a recorded source, which is
   open question 4. Until that is decided the rebuild covers file derived
   metadata only.

Why not A or D now. They add an unmeasured image and RAM cost, pull H3 polygon
code and a profile version into every image, need a reflash for every spec
change, and the Item would still land in intake and need the Action to publish
it. The board gains nothing a footer reading job cannot produce from the same
bytes.

Why not B. It needs bucket read access and conditional writes that upload only
keys do not have, and it reopens the race between many boards on one
Collection.

### Who owns what (history)

| Part | Owns | Does not own |
| --- | --- | --- |
| Firmware logger | Self describing Parquet with the `opensensor` and `geo` footer keys, dated file names, a new file on any location change, immutable finalized files, the board template at build time | Any STAC JSON at runtime |
| Uploader on the board (#81) | Upload of dated finalized files only, oldest first, one at a time, the SD upload ledger, conditional puts where the store has them | Reading the bucket, Collections, the Catalog, any STAC write |
| Publish Action, the indexer | Validation, the copy from intake to public, File Item, Thing Collection, root Catalog, optional Item mirror, `file:size`, `file:checksum`, `statistics`, `providers` and the `partition:*` fields from decision 40 | Changing or deleting board files |
| Mobile app | Upload settings over BLE (mobile #181), STAC for its own uploads until the indexer runs (mobile #190), app profile labels | Writing STAC for a standalone Thing |
| stac-opensensor | The profile, `footer.json`, the example layout, `check-profile.mjs` | Running any job |

### Consequences (history)

- No firmware or contract change. The firmware already emits everything the
  indexer needs.
- The #109 acceptance item for measured on device Item writer cost is not done,
  because no board writer is proposed. It returns only if this record is reopened.
- A standalone Thing gets STAC only once the publish Action exists. Until then its
  public files are plain Parquet with full footers, readable by any client.
- A board on the generic `s3` target uploads to its owner's bucket with no Action,
  so it gets no STAC unless that owner runs the indexer.

### Reopen when (history)

- A deployment needs STAC with no host job at all, for example the generic `s3`
  target with no indexer the owner can run.
- source.coop offers a per prefix, upload only role that lets boards write the
  public product directly with no intake copy.
- A measured Item writer on CoreS3 and Waveshare V2 fits the image, DIRAM and
  PSRAM budget, and a host generator and the firmware produce byte equal Items
  tested in `pixi run gate`.

### Open questions for the owner (history, answered above)

1. Accept option C, with the #81 publish Action as the indexer for both phone
   synced and standalone Things?
2. Where does the indexer code live? Options are stac-opensensor scripts, a host
   tool in this repo, or a new repository, which needs owner approval.
3. Once the indexer runs, does the phone stop writing STAC, or must its output
   match the indexer byte for byte?
4. Fields the board cannot know (provider names, owner contact, license text,
   titles) belong in the Collection. Do they come from the app over BLE, from the
   template, or from indexer configuration only?
5. Is a generic `s3` target with no STAC acceptable, or should the indexer also
   ship as a command the bucket owner can run?
6. Should the uploader record a SHA-256 of each file in its SD ledger, using the
   ESP32-S3 hardware SHA while streaming, so the Action can cross check
   `file:checksum`? That is an uploader feature, not a STAC writer.
7. Bridge question 41 (a file with no located row) and question 37 item 4 (Item
   links `../../../collection.json`) still apply to whoever writes Items.
