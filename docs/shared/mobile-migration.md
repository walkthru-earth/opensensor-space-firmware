# Mobile app migration notes

Companion app builds and firmware wire contracts may both break. There is no
backward compatibility, each firmware change ships with a matched app build and
a reflash. Each section below records one change
the Android and iOS apps (opensensor-space-mobile) can observe, newest first,
with what changed, why, and what the app must do. The wire contract itself
lives in [ble-sync-protocol.md](ble-sync-protocol.md). Files already on a phone
or SD card are never rewritten by the firmware. After a matched reflash the
owner wipes them and starts again.

## Owner site keys and the deployment id (fix-148-owner-config, issue #148)

**Needs a matched reflash and an app change.** Pairs with mobile #266 and
#272. Waveshare firmware moves to `idf-waveshare-parquet-v2.19` and CoreS3 to
`idf-cores3-parquet-v6.25`. Columns, field ids, LIVE keys, decimals and the
schema and dictionary strings do not change. The `dict` and `desc` digests do
not move.

**What changed.**

- Five new `SET_CONFIG` keys, see the
  [key table](ble-sync-protocol.md#device-configuration-v2).
  `owner.title` (64 bytes), `owner.description` (160), `owner.provider` (64),
  `owner.url` (`https://` plus a host that starts with a letter or digit, 96
  bytes, no spaces, `"`, `\` or control characters) and
  `thing.deployment_id` (`[a-z0-9-]`, 32 bytes, a letter or digit at each end,
  never the literal `unknown` because that is the footer value while unset).
  Values are UTF-8, an empty value clears the key, and invalid UTF-8 or a
  control character rejects the whole request with `ERROR` 12 and a detail
  that names the key. There is no contact email key.
- `GET_CONFIG` accepts pages `0..6`. Page 5 is
  `{"owner":{"title":"","provider":"","url":""},"thing":{"deployment_id":""}}`
  and page 6 is `{"owner":{"description":""}}`. Values come back raw UTF-8,
  with JSON escaping only. The worst cases are 458 and 348 bytes. Page 7 and
  above answer malformed `ERROR` with detail `page`.
- The footer `thing.deployment_id` is the stored value, or `unknown` while it
  is unset. A change closes the open file, like a location change, so each
  file holds one deployment id. Title, description, provider and url never
  reach Parquet and never split a file.
- The owner keys live in a new NVS record. A damaged record reads as empty and
  prints `CONFIG OWNER invalid-stored-profile=true` once at boot. No serial
  line ever carries an owner value.
- A `SET_CONFIG` that mixes owner keys with other keys writes two records and
  is not atomic across a power cut. A failed save answers `ERROR` 12 with
  detail `nvs` and changes nothing live. If its rollback also fails, the new
  owner values can appear after the next reboot.
- `PARQUET BEGIN row_bytes` grows from 880 to 920 on both boards, because each
  queued sample now carries its deployment id.

**What the app must do.** Send the owner keys in their own `SET_CONFIG`,
never mixed with other keys, and read them back from pages 5 and 6. After an
`nvs` error, read pages 5 and 6 again after the next reboot. Check
`thing.deployment_id` against `[a-z0-9-]`, the letter or digit at each end,
the `unknown` ban and each byte cap before sending. Check that `owner.url` has
a letter or digit right after `https://`.
Expect a new file after a deployment id change. Read `thing.deployment_id`
from each file's footer, never from the current config, because older files
keep the value they were written with.

## The board says when it needs setup (fix-147-setup-bit, issue #147)

**Needs a matched reflash and an app change (mobile #272).** CoreS3 moves to
`idf-cores3-parquet-v6.24` and Waveshare to `idf-waveshare-parquet-v2.18`.
Columns, field ids, LIVE keys, decimals, status JSON, the schema and
dictionary strings and the `dict` and `desc` digests do not change. Rows still
log with null location until a cell is set.

**What changed.**

- Advert flags bit 6 `setup` is set while `thing.mobility` is `fixed` and
  `location.cell` is empty. It clears as soon as a `SET_CONFIG` sets a cell or
  `thing.mobility=moving`, and the flip starts the 60 s fast advertising
  window. A moving Thing never sets it. Nothing new is stored, the board
  derives it from the location settings.
- GET_CONFIG page 1 ends with `"thing":{"mobility":"fixed","setup":1}`.
  `setup` is 1 or 0 and matches the advert bit.
- Waveshare V2 page 1 adds `location.gnss_cell`, the newest receiver fix as an
  H3 res 8 cell, or `""` without a fix from the last 15 s or when
  `location.resolution` is below 8. It is never a coordinate. CoreS3 has no
  receiver and omits the key.
- The CoreS3 footer shows `Setup needed, open the OpenSensor app` in yellow
  while the bit is set. Page 1 stays under 480 bytes, 198 at worst.

**Why.** Workers install about 18 Things on site (owner decision
2026-11-08). Any phone that sees the bit can open the setup wizard without
knowing the board first, and the GNSS readback lets the wizard show the
board's own cell beside the map picker.

**What the app must do.** Decode advert bit 6 as `setup` and offer the setup
wizard for that board. Read page 1 `thing.setup` after connecting, and read it
again after the wizard's `SET_CONFIG` to confirm it is 0. Treat a missing
`location.gnss_cell` as "no receiver" and `""` as "no fresh fix". Show a
`gnss_cell` as a suggestion on the map only, never write it to `location.cell`
without the owner confirming the cell. See mobile #272.

## Moving Things write GNSS cells or nulls (yharby/gnss-moving-thing, issue #83)

**Needs a matched reflash, app change is small.** Waveshare firmware moves to
`idf-waveshare-parquet-v2.17` and CoreS3 to `idf-cores3-parquet-v6.23`,
because a moving CoreS3 now writes location nulls. Columns, field ids, LIVE
keys, decimals, CONFIG keys and the schema and dictionary strings do not
change. The `dict` and `desc` digests do not move.

**What changed.**

- `thing.mobility=moving` now changes what each row holds. A fixed Thing still
  writes the configured cell with `location_source` 1. A moving Waveshare V2
  writes the GNSS cell coarsened to `location.resolution` with
  `location_source` 2 while the fix is at most 15 s old, and four location
  nulls (`location_source` null too) otherwise. A moving Thing never writes `location.cell`. A
  moving CoreS3 has no receiver, so all its rows are null.
- A GNSS cell change does not split a file. One file from a moving Thing holds
  many cells. A change of `thing.mobility`, `location.cell`,
  `location.resolution` or `location.country` still splits.
- No raw latitude or longitude is logged, stored or sent. Only the H3 cell
  leaves the parser.
- New serial lines on Waveshare, `OS GNSS state=<unknown|no-reply|off|on|fix|no-fix>
  uart=2 tx=18 rx=17` on each state change, `OS ERROR operation=gnss-start
  reason=<n>` when the reader cannot start and `OS ERROR
  operation=gnss-power-on` when the modem supply cannot be switched on. None
  carries a position.

**What the app must do.** Read `location_source` per row, never per file, and
draw a moving Thing as a track of cells. Treat null location rows from a
moving Thing as no fix, not as an error, and never fill them from the
configured cell. Show the owner that `moving` without a receiver writes no
location. The phone cell path (`location.phone_cell`, source 3) is still
reserved.

## LAN OTA requests and a boot self test (yharby/ota-software, issue #40)

**No app update needed.** Sync, CONFIG, LIVE and the file list do not change.
The `dict` and `desc` digests do not move. The app may add an update screen.

**What changed.**

- Four new LAN only requests, `OTA_BEGIN` `0x12`, `OTA_WRITE` `0x13`,
  `OTA_END` `0x14` and `OTA_CANCEL` `0x15`, and a new frame `OTA_STATE`
  `0x50`. See [LAN OTA](ble-sync-protocol.md#lan-ota). Over BLE they answer
  `ERROR` 13 `detail="lan-only"`.
- A new error code 18 `image rejected`, with the details `header`, `chip`,
  `project`, `verify` and `digest`.
- The device never reboots after `OTA_END`. The app sends `REBOOT` when the
  owner agrees.
- After boot the logger prints one `SELFTEST` line on serial and marks a
  healthy app valid. Nothing new reaches BLE or LAN.

**What the app must do.** Nothing, unless it adds OTA. An app that adds OTA
sends `OTA_BEGIN` with the size and SHA-256 of the `.bin`, then chunks of at
most `max_chunk` bytes at offset `next`, then `OTA_END` with a 120 s timeout,
then `REBOOT`. On any error it starts again with `OTA_BEGIN`. It never pauses more than 30 s
between chunks, or the device drops the upload. It shows the
error detail to the owner and never retries a rejected image.

## BREAKING. Gauge validity tokens, pms_status 6, created_by and repo URLs (yharby/contract-bump, issues #28 #39 #111)

**Needs app update.** Every board needs a matched reflash with the app build
that bundles the new templates. Firmware moves to `idf-cores3-parquet-v6.22`
and `idf-waveshare-parquet-v2.16`. The Waveshare dictionary moves to
`waveshare-sim7670g-telemetry-v8`. Schemas, columns, field order, LIVE keys
and decimals do not change. Old archives are not read (owner decision 14).

**What changed.**

- Waveshare validity tokens (#28). `battery_mv` reads
  `battery_installed_vcell_read_ok`, `battery_percent` reads
  `battery_installed_soc_read_ok_range_0_100` and `battery_soc_raw_percent`
  reads `battery_installed_soc_read_ok_uncapped`. The old token
  `battery_installed_gauge_read_ok` claimed both words had to read, which the
  code never required. Only the field table moves, so the Waveshare `dict`
  moves. Its `shipped.lock` gains a `baseline` release that names this entry.
- Waveshare `pms_status` gains class 6 `sleeping` (#39). It is reserved for the
  PMS duty cycle. No image writes it yet. CoreS3 stays at 0 to 4.
- Parquet `created_by` is `opensensor-space-firmware version 0.3 (build
  <firmware>)` on both boards (#111). It was `m5stack-aq-parquet version 0.2`
  on CoreS3 and `aq-parquet version 0.1` on Waveshare. The partial rebuild
  tool writes `opensensor-space-firmware rebuild_partial version 1`.
- Pinned URLs use the renamed repository,
  `https://raw.githubusercontent.com/walkthru-earth/opensensor-space-firmware/<commit>/...`
  (#111). The template embeds the dictionary URL, so both `desc` digests move.
  The CoreS3 `dict` does not.

| Board | New `dict` | Old `dict` | New `desc` | Old `desc` |
| --- | --- | --- | --- | --- |
| CoreS3 | `3b4d8500a19f42c6743b0f7d725276c96ca14a0c249f613048603afbc3cfaff9` (unchanged) | same | `4dd6c4b1ef19187ed2c011ab86018e43be0f28edc3af63a0f6a27b70ff203ef3` | `1c9511bd6836876bf90bbdbe3224b00bd5d176e0fdd5293d5ee6dd73ef6ea45a` |
| Waveshare | `a8605aafb6f76b794e9e7fb32927be6d203afeeb14527206c0e49c1eb0436809` | `50a53393e940be182ded0b1119de6ac6d9521e7d28dfd244a24666ccf2a616c7` | `d4e4fa499e20c414b3b1fd61343982e5735687d67cfb8b4591efaa5ed5f96bd4` | `1180e7273bf1dd4cce94ba02b84af61bdddeeb11dc7ddcef61967aca7031c472` |

Published files, byte exact, SHA-256 equal to the new `desc`.

- `docs/shared/templates/m5stack-cores3/3b4d8500a19f42c6743b0f7d725276c96ca14a0c249f613048603afbc3cfaff9.json`
- `docs/shared/templates/waveshare-esp32-s3-sim7670g/a8605aafb6f76b794e9e7fb32927be6d203afeeb14527206c0e49c1eb0436809.json`

**Why.** Owner decisions of 2026-10-27. One PR moves `dict` and `desc` once
for all four changes.

**What the app must do.**

1. Copy both published files byte for byte into the bundled templates and pin
   each SHA-256 to the new `desc`. Drop the old Waveshare `dict` and both old
   `desc` entries.
2. Accept the new Waveshare `dict` in the LIVE layout table. The keys and
   decimals equal those of `waveshare-sim7670g-telemetry-v7`.
3. Add `pms_status` codes 5 `model-mismatch` and 6 `sleeping` to `PmsStatus`.
   Today `PmsStatus.fromCode` has 0 to 4, so a Waveshare board that reports 5
   shows as waiting. Treat 5 and 6 as not ready, PM values are null for both.
   Read the names from the template `classification:classes`.
4. If the app matches `created_by`, match the prefix
   `opensensor-space-firmware version `. Never parse it for identity, use the
   `opensensor` footer key.
5. Show the `dictionary_uri` and describedby links only. They now name
   `walkthru-earth/opensensor-space-firmware`. Trust the digests, never the
   URL.
6. `location_source` 3 `phone` (#83) is unchanged. The class was already in
   both templates. No image writes it yet, see #83.

## Settings no longer read per-key values (issue #111)

**No app update needed.** Wire, `GET_CONFIG` and `SET_CONFIG` do not change.

**What changed.** A board with no valid `cfg` settings record now does a
first boot. It gets a new random PIN and LAN token, Wi-Fi off and default
location and NTP, and seeds a fresh record. It no longer reads the per-key
values that firmware before the `cfg` record stored. No real board has them in
the `oscfg` namespace. Per-key writes left the firmware in 6727547, the
commit that added the record, and `oscfg` was first used in b37a76b two weeks
later. The log line for a damaged record is now
`CONFIG BLOB invalid=true fallback=defaults` instead of `fallback=legacy`.
Nothing in NVS is erased.

**Why.** No backward compatibility (owner decision 14). The fallback was dead
code on every board.

**What the app must do.** Nothing. If a damaged record ever shows up, the
board looks freshly reset and needs pairing and Wi-Fi setup again, as before.

## BREAKING. Station becomes Thing, native geometry column and geo footer key (feat/thing-ab, issue #83)

**Needs app update.** Every board needs a matched reflash with the app build
that reads the Thing footer, the `thing` INFO key and the new location
columns. Old archives are not read (owner decision 14), the owner wipes them
after the reflash.

**What changed.** The firmware follows stac-opensensor commit
`4408c4e52366bb3d154d22bbfffc88ff09aed6f1` (bridge decisions 31 to 35).

- Station is now Thing (decision 31). The footer group `station` is
  `thing`, with `id`, `device_id`, `board`, `deployment_id` and a new required
  `mobility`. BLE `info` and LAN HELLO name the id `thing` instead of
  `station`. The mDNS TXT record is `thing=<uuid>`. Hive paths start
  `thing=<uuid>/` instead of `station=<uuid>/`, for dated and unsynced files.
  NVS holds the id in `parquet/thing`, so the first boot of this image mints a
  new Thing id. The old `parquet/station` key stays in flash and is never read.
- `thing.mobility` is `fixed` (default) or `moving`. It is a new `SET_CONFIG`
  key, `GET_CONFIG` page 1 returns it as `{"thing":{"mobility":"fixed"}}` after
  the `location` object, and a change splits the file like a location change.
- `thing.board` is the board slug of the template folder,
  `m5stack-cores3` or `waveshare-esp32-s3-sim7670g`. The free text board name
  is the template title. `firmware.boot_id` is the 32 hex boot id that also
  appears in the file name.
- `h3_center_lat_e7` and `h3_center_lon_e7` are gone (decision 32). Their
  places hold `location_source` (INT32 annotated `INTEGER(8,false)`, so UINT8)
  and `geometry` (BYTE_ARRAY with the Parquet `GEOMETRY` logical type, no crs,
  so OGC:CRS84). `geometry` is a 2D little endian WKB Point of 21 bytes at
  the H3 cell centre, null exactly when `h3_cell_id` is null. Its column chunks
  carry `GeospatialStatistics` and no min or max.
- `location_source` codes are 0 none, 1 configured, 2 gnss, 3 phone, null when
  the row has no location. The template lists them as `classification:classes`.
  This firmware only writes 1, it has no GNSS or phone location path yet.
- `location.phone_cell` is a reserved `SET_CONFIG` key for source 3, the
  phone's own fix. The board will coarsen it to `location.resolution`, keep it
  in RAM only and fall back to `location.cell` when it is cleared or on
  reboot. The app may show the key now but must not send it until a firmware
  release says it is accepted. Until then the board rejects it like any
  unknown key, with `ERROR` 12 and `detail` `location.phone_cell`, and the
  whole request changes nothing, so never batch it with other keys.
- A second footer key `geo` holds compact GeoParquet 2.0 JSON,
  `{"version":"2.0.0","primary_column":"geometry","columns":{"geometry":{"encoding":"WKB","geometry_types":["Point"],"bbox":[xmin,ymin,xmax,ymax]}}}`.
  The bbox is left out when the file has no location. A file holds exactly the
  `opensensor` and `geo` keys.
- The default maximum H3 resolution moves from 5 to 8 (about 0.74 km2 on
  average). Locations already saved keep their stored resolution.
- The validity predicate of the four location columns is `location_present`.
- The Thing release is a fresh field table baseline on both boards (owner
  answer to bridge question 36, 2026-10-26). Every `field_id` is a plain
  1-based position. Removing the two H3 centre columns changes the meaning of
  exactly four ids. CoreS3 ids 80 and 81 were `h3_center_lat_e7` and
  `h3_center_lon_e7` and are now `location_source` and `geometry`. Waveshare
  ids 55 and 56 change the same way. Every other id keeps its column. The app
  must still not key on any id from an earlier dictionary, take ids from the
  new template only. From this baseline the
  rule is append only again.
- Template columns follow the spec spelling. `h3_cell_id` is `int64` with role
  `referenceInformation`, `h3_resolution` is `int32` with role
  `referenceInformation`, `location_source` is `uint8` with role
  `qualityInformation` and aggregate `mode`, `geometry` is type `geometry` with
  role `coordinate` and no unit. The data asset has
  `table:primary_geometry` `geometry`.
- Only dated `data_HHMM_` files are meant for publication. Unsynced files stay
  in the local archive.

| | CoreS3 | Waveshare V2 |
| --- | --- | --- |
| Firmware | `idf-cores3-parquet-v6.21` | `idf-waveshare-parquet-v2.15` |
| Schema | `cores3-telemetry-v6` | `waveshare-sim7670g-telemetry-v6` |
| Dictionary | `cores3-telemetry-v7` | `waveshare-sim7670g-telemetry-v7` |
| Columns | 81 | 62 |
| `dict` | `3b4d8500a19f42c6743b0f7d725276c96ca14a0c249f613048603afbc3cfaff9` | `50a53393e940be182ded0b1119de6ac6d9521e7d28dfd244a24666ccf2a616c7` |
| `desc` | `1c9511bd6836876bf90bbdbe3224b00bd5d176e0fdd5293d5ee6dd73ef6ea45a` | `1180e7273bf1dd4cce94ba02b84af61bdddeeb11dc7ddcef61967aca7031c472` |

LIVE keys and decimals are unchanged, neither new column has a LIVE key.

**Why.** The ecosystem names the monitored object a Thing, which may move.
A native GEOMETRY column lets DuckDB, GDAL and Iceberg v3 read the location
without a custom decoder, and the H3 cell stays the privacy unit.

**What the app must do.** Read `thing.id` instead of `station.id` and the
`thing` key from `info`, HELLO and mDNS TXT. Archive under `thing=<uuid>/`.
Read `thing.mobility` and offer `thing.mobility` in settings. Read the
location from `geometry` (WKB Point, x is longitude) and `location_source`,
and use the `geo` key for the file bbox. Send resolution 8 as the default.
Bundle the new templates by `desc` and resolve every column by name or by the
new `field_id` values only.

## BREAKING. One versioned opensensor footer key (fix/88-opensensor-footer, issue #88)

**Needs app update.** Every board needs a matched reflash with the app build
that reads the new footer. An app that reads the old flat keys finds none of
them in a new file.

**What changed.** Every telemetry Parquet file now has exactly one key value
entry, `opensensor`. Its value is compact UTF-8 JSON that follows
stac-opensensor `json-schema/footer.json` v0.1.0 (commit a56dfb0, pinned in
`tools/stac_opensensor/`). The members come in the schema's property order,
and every number is a JSON number, not text. Every old flat key is gone, with
no `opensensor:template_*` keys either. `file.rotation_interval_s` is at most
3600. The firmware identity moves to `idf-cores3-parquet-v6.20` and
`idf-waveshare-parquet-v2.14`. Both `dict` and both `desc` digests are
unchanged, and so are the columns, the LIVE keys and the wire.

**Why.** The owner's stac-opensensor footer schema replaces about 40 flat text
keys with one versioned document that a host validates in one step and that
links each file to its board template by URL and digest.

**What the app must do.** Read `opensensor`, parse it as JSON, check
`version` is `0.1.0`, and take values by path from the table below. Use
`template.href` and `template.sha256` to fetch and verify the board template,
which carries the column units and code meanings. Mobile #205 reads only this
key. It finds the template by `template.sha256` first, then by
`dictionary.sha256`, and moves from `station_id` and `purpose` to `station.id`
and `file.purpose`. This PR and mobile #205 merge as one pair. A file without
the key is an old archive, and the owner wipes those after the reflash.

| Old key | New JSON path |
| --- | --- |
| `schema_version` | `firmware.schema` |
| `device_id` | `station.device_id` |
| `station_id` | `station.id` |
| `boot_id` | `firmware.boot_id` |
| `firmware` | `firmware.version` |
| `dictionary_version` | `dictionary.version` |
| `dictionary_uri` | `dictionary.uri` |
| `dictionary_sha256` | `dictionary.sha256` (the `dict` digest) |
| `acquisition_config_id` | `acquisition.config_id` |
| `acquisition_config` | `acquisition.config` (JSON text, as before) |
| `deployment_id` | `station.deployment_id` |
| `calibration_id` | `acquisition.calibration_id` |
| `time_semantics` | `acquisition.time_semantics` |
| `rotation_interval_s` | `file.rotation_interval_s` (number) |
| `row_groups` | `file.row_groups` (number) |
| `row_group_rows_max` | `file.row_group_rows_max` (number) |
| `compression` | `file.compression` |
| `purpose` | `file.purpose` |
| `board` | `station.board` |
| `sample_interval_ms` | `acquisition.sample_interval_ms` (number) |
| none | `version` (`0.1.0`) |
| none | `template.href` (raw URL of the board template for this `dict` at a pinned commit, #89) |
| none | `template.sha256` (the `desc` digest) |

**Dropped, no slot in the spec.** Each key below is dropped with no alias and
no dual write. `footer.json` v0.1.0 has no member for it, and the reason
column says where a reader finds the information now.

| Old key | New JSON path | Reason |
| --- | --- | --- |
| `h3_cell_id` | dropped | Already in every row, column `h3_cell_id` |
| `h3_resolution` | dropped | Already in every row, column `h3_resolution` |
| `h3_center_units` | dropped | Rows carry `h3_center_lat_e7` and `h3_center_lon_e7`, units are in the board template |
| `h3_max_resolution` | dropped | Lost from the file, rows keep the actual resolution |
| `location_source` | dropped | Lost from the file, the location source is fixed by firmware |
| `country_iso3166_1_alpha2` | dropped | Lost from the file, no country slot in the spec |
| `country_source` | dropped | Lost from the file, no country slot in the spec |
| `reconciliation_anchor_mono_us` | dropped | Rows carry `clock_anchor_mono_us`. The old footer anchor also covered earlier rows in the same file. A host takes it from a later row with the same `boot_id`, except when the file was that boot's last |
| `reconciliation_anchor_utc_ns` | dropped | Rows carry `clock_anchor_utc_us`. The old footer anchor also covered earlier rows in the same file. A host takes it from a later row with the same `boot_id`, except when the file was that boot's last |
| `reconciliation_anchor_source` | dropped | Rows carry `clock_status`. The old footer anchor also covered earlier rows in the same file. A host takes it from a later row with the same `boot_id`, except when the file was that boot's last |
| `clock` | dropped | `clock_status` classes in the board template |
| `durability` | dropped | Lost, it was a fixed text note |
| `pms_status` | dropped | `pms_status` classes in the board template |
| `light_status` (CoreS3) | dropped | `light_status` classes in the board template |
| `proximity_status` (CoreS3) | dropped | `proximity_status` classes in the board template |
| `gauge_status` (Waveshare) | dropped | `gauge_status` classes in the board template |
| `battery_health_state` (Waveshare) | dropped | `battery_health_state` classes in the board template |
| `pm25_batch_candidate_status` (Waveshare) | dropped | `pm25_batch_candidate_status` classes in the board template |
| `battery_presence` (Waveshare) | dropped | Still in `acquisition.config` as `battery_presence` |
| `sensor_model` (Waveshare) | dropped | Lost as a key, `acquisition.config` keeps `pms_model` |
| `magnetic_units` (CoreS3) | dropped | Lost, it was a board text note |
| `unavailable` | dropped | Lost, it was a board text note |
| `gauge_status_flags` (Waveshare) | dropped | Lost, it was a board text note on the bit layout |
| `battery_minutes_to_empty` (Waveshare) | dropped | Lost, it was a board text note |
| `pm25_batch_candidate_formula` (Waveshare) | dropped | Lost, it was a board text note |
| `pm25_batch_candidate_validation` (Waveshare) | dropped | Lost, it was a board text note |
| `hardware_revision` (Waveshare) | dropped | Lost as a key, `station.board` names the revision |
| `storage_bus` (Waveshare) | dropped | Lost, it was a board text note |

**Gap, rebuilt partial files.** `tools/rebuild_partial.py` writes no
`opensensor` key. Its source is a `.partial` file cut before the footer, so
there is no `opensensor` footer to carry over. Every row holds `device_id`,
`boot_id_hi` and `boot_id_lo`, but the partial bytes do not hold
`station.id`, `acquisition.sample_interval_ms` or `acquisition.time_semantics`,
and `file.purpose` has no value for a rebuilt file. The tool keeps its host side
`rebuild_*` provenance keys, so mobile #205 sees a rebuilt file as an old
archive with no `opensensor` key. A source that still has its footer is copied
byte for byte and keeps its `opensensor` key. Closing this gap needs a spec
change, for example a rebuilt `file.purpose` value, and is raised with the
owner.

**Example footers.** The ids are placeholders. CoreS3 writes 1663 bytes and
Waveshare 2287 for a one hour LZ4 file with four row groups.

```json
{"version":"0.1.0","station":{"id":"00000000-0000-4000-8000-000000000000","device_id":"0123456789ab","board":"CoreS3 ESP32-S3 rev0.2","deployment_id":"unknown"},"firmware":{"version":"idf-cores3-parquet-v6.20","schema":"cores3-telemetry-v5","boot_id":"0123456789abcdef0123456789abcdef"},"dictionary":{"sha256":"4f80c26cde8d7ea78c2160225d98386009a8651d9a049b4bf68a75da96015e94","version":"cores3-telemetry-v6","uri":"https://raw.githubusercontent.com/walkthru-earth/aq-parquet/ada95144af0cf5191e95a35d0f80768fe58ac73b/firmware/esp-idf-cores3/bringup/telemetry_fields.inc"},"template":{"href":"https://raw.githubusercontent.com/walkthru-earth/aq-parquet/1f37edf350ae5efab0caa59ab4ceedd07d0028e5/docs/shared/templates/m5stack-cores3/4f80c26cde8d7ea78c2160225d98386009a8651d9a049b4bf68a75da96015e94.json","sha256":"bb0ab0a3eaafb05e45bbbf5216194ab808110df92e6ee277c8ef2af6f2f314b0"},"acquisition":{"sample_interval_ms":10000,"time_semantics":"event_time_utc_us estimates snapshot start; collection_completed_mono_us bounds sequential reads; pms_received_mono_us is last checksum-valid UART frame receipt, not sensor phenomenon time; result/ingestion time and clock uncertainty unknown; no retroactive UTC assignment","config_id":"cores3-acquisition-v3","config":"{\"sample_interval_ms\":10000,\"pms_warmup_us\":30000000,\"pms_stale_after_ms\":5000,\"pms_uart_rx\":18,\"pms_uart_tx\":17,\"pms_baud\":9600,\"m5unified\":\"0.2.26\",\"esp_idf\":\"6.1\",\"ltr553\":\"raw-v1\",\"snapshot\":\"latest-available-not-average\"}","calibration_id":"unknown"},"file":{"rotation_interval_s":3600,"row_groups":4,"row_group_rows_max":90,"compression":"LZ4_RAW","purpose":"telemetry"}}
```

```json
{"version":"0.1.0","station":{"id":"00000000-0000-4000-8000-000000000000","device_id":"0123456789ab","board":"Waveshare ESP32-S3-SIM7670G-4G V2.0 ESP32-S3 rev0.2","deployment_id":"unknown"},"firmware":{"version":"idf-waveshare-parquet-v2.14","schema":"waveshare-sim7670g-telemetry-v5","boot_id":"0123456789abcdef0123456789abcdef"},"dictionary":{"sha256":"6f567b38fecc165f544626168931b3dc3d674482da87ed78cb0bec5bf3dc6ceb","version":"waveshare-sim7670g-telemetry-v6","uri":"https://raw.githubusercontent.com/walkthru-earth/aq-parquet/ada95144af0cf5191e95a35d0f80768fe58ac73b/firmware/esp-idf-waveshare-sim7670g/logger/telemetry_fields.inc"},"template":{"href":"https://raw.githubusercontent.com/walkthru-earth/aq-parquet/1f37edf350ae5efab0caa59ab4ceedd07d0028e5/docs/shared/templates/waveshare-esp32-s3-sim7670g/6f567b38fecc165f544626168931b3dc3d674482da87ed78cb0bec5bf3dc6ceb.json","sha256":"7f2bbf5314ce8d6f4d0e5ee518991a9c3d43ab7b66d973e8a5921e811059872a"},"acquisition":{"sample_interval_ms":10000,"time_semantics":"event_time_utc_us estimates snapshot start; collection_completed_mono_us bounds sequential reads; pms_received_mono_us is last checksum-valid UART frame receipt, not sensor phenomenon time; result/ingestion time and clock uncertainty unknown; no RTC anchor and no retroactive UTC assignment","config_id":"waveshare-sim7670g-acquisition-v3","config":"{\"sample_interval_ms\":10000,\"pms_warmup_us\":30000000,\"pms_stale_after_ms\":5000,\"pms_uart_rx\":1,\"pms_uart_tx\":2,\"pms_baud\":9600,\"pms_model\":\"PMS5003T\",\"battery_presence\":\"explicit-owner-configuration\",\"battery_percent\":\"integer-percent-truncated-capped-at-100\",\"snapshot\":\"latest-available-not-average\",\"sensor_identity\":\"owner-provisioned-nvs-and-per-row\",\"batch_candidate\":\"off-by-default-not-cairo-validated\",\"battery_health\":{\"source\":\"max17048-vcell\",\"external_power_mv\":4240,\"external_power_hysteresis_mv\":20,\"low_enter_mv\":3500,\"critical_enter_mv\":3350,\"hysteresis_mv\":75,\"debounce_samples\":3,\"settling_minutes\":60,\"calibration\":\"uncalibrated-defaults\",\"critical_action\":\"finalize-open-file-once\"}}","calibration_id":"unknown"},"file":{"rotation_interval_s":3600,"row_groups":4,"row_group_rows_max":90,"compression":"LZ4_RAW","purpose":"telemetry"}}
```

**Query.** DuckDB reads one value per file like this.

```sql
SELECT file_name,
       decode(value)::JSON ->> '$.firmware.version' AS firmware,
       (decode(value)::JSON ->> '$.acquisition.sample_interval_ms')::INTEGER AS period_ms
FROM parquet_kv_metadata('station=*/**/*.parquet')
WHERE decode(key) = 'opensensor';
```

**Proof.** `telemetry-contract-test` and `waveshare-contract-test` build the
footer through `footer::build`, validate it against the pinned `footer.json`
with format checks, match its key order to the example, write a real file and
read back exactly one key. Six mutations must fail, a stray key, a number as
text, a wrong `desc`, a missing field, rotation over 3600 and an extra
member. `logger-footer-test` pins the exact bytes and the escaping.

## BREAKING. Dictionary and template URLs pinned to commits (fix/89-pin-uris, issue #89)

**Needs app update.** Every board needs a matched reflash with the app build
that bundles the new templates. The firmware identity moves to
`idf-cores3-parquet-v6.19` and `idf-waveshare-parquet-v2.13`. Both `dict`
digests are unchanged. Both `desc` digests move, because the template embeds
the dictionary URL.

**What changed.**

- The footer `dictionary_uri` (`kDictionaryUri`) and the template
  `describedby` href are now the same raw GitHub URL at a full 40 hex commit,
  no longer a blob URL on `main`. The bytes at the URL hash to `dict`.
- New `kTemplateHref` in `telemetry_contract.h`, the raw published template at
  a full commit whose bytes hash to `desc`. It is outside the template, so it
  never moves `desc`. The footer carries it once #88 lands, nothing on the
  wire carries it yet.
- `pixi run pinned-uris-test` fails on a `main`, branch or tag URL, a short
  sha, a commit that is not on `main` and a commit whose bytes miss the
  digest.

| Board | `dict` (unchanged) | New `desc` | Old `desc` |
| --- | --- | --- | --- |
| CoreS3 | `4f80c26cde8d7ea78c2160225d98386009a8651d9a049b4bf68a75da96015e94` | `bb0ab0a3eaafb05e45bbbf5216194ab808110df92e6ee277c8ef2af6f2f314b0` | `1b7d969abb670579cd1f34f0bd7e224f66b120ed178fbb2fcf024e14a46c3f08` |
| Waveshare | `6f567b38fecc165f544626168931b3dc3d674482da87ed78cb0bec5bf3dc6ceb` | `7f2bbf5314ce8d6f4d0e5ee518991a9c3d43ab7b66d973e8a5921e811059872a` | `e6b32af089b5c0b8389999a09b4a81ffe36d15f908a2002765025f78853b7d32` |

Published files, byte exact, SHA-256 equal to the new `desc`.

- `docs/shared/templates/m5stack-cores3/4f80c26cde8d7ea78c2160225d98386009a8651d9a049b4bf68a75da96015e94.json`
- `docs/shared/templates/waveshare-esp32-s3-sim7670g/6f567b38fecc165f544626168931b3dc3d674482da87ed78cb0bec5bf3dc6ceb.json`

The file paths keep their names because `dict` is unchanged. Only the bytes
and `desc` move, and `index.json` maps the new `desc` values.

| Board | `kDictionaryUri` | `kTemplateHref` |
| --- | --- | --- |
| CoreS3 | `https://raw.githubusercontent.com/walkthru-earth/aq-parquet/ada95144af0cf5191e95a35d0f80768fe58ac73b/firmware/esp-idf-cores3/bringup/telemetry_fields.inc` | `https://raw.githubusercontent.com/walkthru-earth/aq-parquet/1f37edf350ae5efab0caa59ab4ceedd07d0028e5/docs/shared/templates/m5stack-cores3/4f80c26cde8d7ea78c2160225d98386009a8651d9a049b4bf68a75da96015e94.json` |
| Waveshare | `https://raw.githubusercontent.com/walkthru-earth/aq-parquet/ada95144af0cf5191e95a35d0f80768fe58ac73b/firmware/esp-idf-waveshare-sim7670g/logger/telemetry_fields.inc` | `https://raw.githubusercontent.com/walkthru-earth/aq-parquet/1f37edf350ae5efab0caa59ab4ceedd07d0028e5/docs/shared/templates/waveshare-esp32-s3-sim7670g/6f567b38fecc165f544626168931b3dc3d674482da87ed78cb0bec5bf3dc6ceb.json` |

**Why.** A URL on `main` moves on, so the bytes behind it stop matching the
digest. A commit URL always serves the bytes the digest names.

**What the app must do.**

1. Copy both published files byte for byte into the bundled templates and pin
   each full SHA-256 to the new `desc`. Drop the old `desc` entries.
2. Resolve templates by `desc` first, then `dict`, as before. A board on
   v6.19 or v2.13 reports the new `desc` prefix in `info.desc`.
3. Show or follow `dictionary_uri` only as a link. Trust `dictionary_sha256`
   and `desc`, never the URL. Files written before this change carry a blob
   URL on `main`, which may have moved since.

## BREAKING. Board template over GET_DESCRIPTOR 0x11 and `info.desc` (fix/87-get-descriptor, issue #87)

**Needs app update.** Every board needs a matched reflash with the app build
that reads `info.desc` and knows opcode `0x11`. The firmware identity moves to
`idf-cores3-parquet-v6.17` and `idf-waveshare-parquet-v2.11`. Both `dict`
digests and both `desc` digests are unchanged, so files, LIVE profiles and
cached templates keep working.

**What changed.**

- `info` (BLE `info` characteristic and LAN `HELLO`) gains `"desc"`, the first
  16 hex characters of the board descriptor digest, right after `"dict"`. It is
  always present. The whole document stays under 400 bytes.
- New opcode `0x11` `GET_DESCRIPTOR` over BLE and LAN. The request is the
  opcode, then `offset` u32 and `length` u16, little-endian, 7 bytes in all.
- New frame `0x4F` `DESCRIPTOR_CHUNK`, `total` u32, `offset` u32, then up to
  `min(length, total − offset, payload_max − 9)` template bytes. BLE
  `payload_max` is the negotiated MTU minus 3, up to 512, so a chunk is at
  most 503 bytes over BLE and 1015 over LAN. The board only sends it as the
  reply to a `GET_DESCRIPTOR`, never unsolicited.
- New error code 17, no descriptor, for an image built without a template.
  Other failures reuse codes the app knows. The board checks in this order
  and answers the first match. `ERROR` 1 `detail="descriptor"` for a request
  that is not 7 bytes, then `ERROR` 17, then `ERROR` 1 `detail="length"` for
  length 0, then `ERROR` 6 `detail="offset"` for an offset at or past
  `total`, then `ERROR` 7 `detail="mtu"` when the link budget cannot hold a
  byte.

**Why.** The app needs the board template (the `OpenSensorTemplate` JSON) to describe
a station without a network. The board already carries it in flash, and now
serves it.

**What the app must do.**

1. Read `info.desc`. If the app already holds a template whose SHA-256 starts
   with that prefix, skip the download.
2. Otherwise download by request, one chunk per request. Send offset 0, take
   `total` from the reply, then ask for the next missing offset until all
   `total` bytes are in. The board keeps no state between requests, so a lost
   or late reply costs one more request for the same offset, and any missing
   range can be asked for again later. Drop a chunk whose offset the app did
   not ask for. If `total` changes, drop the bytes and start over from offset
   0 once, and fail the download if it changes again.
3. Hash the exact bytes, without a trailing NUL and without reformatting. The
   full `desc` is the SHA-256 of these bytes. The template cannot carry its own
   hash, so check the hex digest twice. First its first 16 characters must
   equal `info.desc`. Then the full 64 hex must equal the template SHA-256 the
   app trusts, from the Parquet footer or the `file:checksum` of the template
   link on the station Collection. Discard the bytes on any mismatch.
4. Treat `ERROR` 17 as a board with no template and fall back to a bundled
   copy, such as the published file in [`docs/shared/templates`](templates/README.md)
   whose SHA-256 is the board `desc` (#86). Treat `ERROR` 7 `detail="mtu"` as a link to retry after a larger MTU.

The host tool does the same with
`pixi run ble-sync descriptor --out <file>`, and with `--expect <64 hex>` for
the full check.

## Board templates published byte exact (fix/86-publish-templates, issue #86)

**App update needed, no reflash.** Nothing on the board, BLE or LAN changes.

**What changed.** The generated board template of each board is now published
in [`docs/shared/templates`](templates/README.md), exactly the `kTemplateJson`
bytes the board embeds, minified with no final newline. The SHA-256 of each
file is the board `desc`. `index.json` maps each `dict` to the board, schema
name, template `href` and `desc` under `templates`, and each `desc` to the
board, `dict` and `href` under `by_desc`. Only the current `dict` per board is
published, older digests are dropped. The dict digests, `desc`, columns, LIVE
keys and firmware identities are unchanged.

| Board | Published file | `desc` |
| --- | --- | --- |
| CoreS3 | `templates/m5stack-cores3/4f80c26cde8d7ea78c2160225d98386009a8651d9a049b4bf68a75da96015e94.json` | `1b7d969abb670579cd1f34f0bd7e224f66b120ed178fbb2fcf024e14a46c3f08` |
| Waveshare | `templates/waveshare-esp32-s3-sim7670g/6f567b38fecc165f544626168931b3dc3d674482da87ed78cb0bec5bf3dc6ceb.json` | `e6b32af089b5c0b8389999a09b4a81ffe36d15f908a2002765025f78853b7d32` |

**Why.** The app bundles hand written template bytes today, whose hashes are
not `desc`. With the generated bytes published, the app and the firmware hold
one template per board, and the app can match it by `desc`.

**What the app must do.** Replace both bundled templates with these files,
copied verbatim (no reformat, no added newline). Resolve a board template by
`desc` first through `by_desc`, then by `dict` through `templates`. Read LIVE
keys through `firmware.live_keys` of the template, which matches the board
LIVE key order.

## BREAKING. AQ wire, NVS and internal names move to OpenSensor (fix/93-renames, issue #93)

**Needs app update.** Every board needs a matched reflash with the app build
that knows the new names. An app on the old names finds no board on LAN and
no service on BLE.

**What changed.** Every name the app, the host tools or a stored blob depends
on moves from its AQ form to an OpenSensor form, with no backward
compatibility. Port 47390, the mDNS TXT keys, the GATT attribute order and
payloads, the `parquet` NVS namespace and every key inside a namespace are
unchanged. Schema names, property ids, URNs, columns, footer keys, LIVE keys
and both `info.dict` digests are unchanged too, so files and LIVE profiles
keep working. The firmware identity moves to `idf-cores3-parquet-v6.16` and
`idf-waveshare-parquet-v2.10`. Both `desc` digests are unchanged.

| What | Old | New |
| --- | --- | --- |
| mDNS service type | `_aqsync._tcp` | `_ossync._tcp` |
| LAN handshake magic | `AQS1` | `OSS1` |
| GATT base UUID | `c0a5e9f0-XXXX-4b1a-9c3e-2d7f8a6b4e01` | `255f595e-XXXX-47ed-8e0d-59994199b680` |
| GATT service (0001) | `c0a5e9f0-0001-4b1a-9c3e-2d7f8a6b4e01` | `255f595e-0001-47ed-8e0d-59994199b680` |
| GATT `info` (0002) | `c0a5e9f0-0002-4b1a-9c3e-2d7f8a6b4e01` | `255f595e-0002-47ed-8e0d-59994199b680` |
| GATT `status` (0003) | `c0a5e9f0-0003-4b1a-9c3e-2d7f8a6b4e01` | `255f595e-0003-47ed-8e0d-59994199b680` |
| GATT `live` (0004) | `c0a5e9f0-0004-4b1a-9c3e-2d7f8a6b4e01` | `255f595e-0004-47ed-8e0d-59994199b680` |
| GATT `control` (0005) | `c0a5e9f0-0005-4b1a-9c3e-2d7f8a6b4e01` | `255f595e-0005-47ed-8e0d-59994199b680` |
| GATT `response` (0006) | `c0a5e9f0-0006-4b1a-9c3e-2d7f8a6b4e01` | `255f595e-0006-47ed-8e0d-59994199b680` |
| Advert service data UUID | `c0a5e9f0-0001-…` | `255f595e-0001-…` |
| GATT service name | `AQ Sync` | `OpenSensor Sync` |
| NVS config namespace | `aqcfg` | `oscfg` |
| NVS coredump namespace | `aqdump` | `osdump` |
| Serial and LOG_TAIL tags | `AQ BOARD`, `AQ CONFIG`, `AQ DIAG`, `AQ ERROR`, `AQ GAUGE`, `AQ MEMORY`, `AQ OWNER_PIN`, `AQ PMS`, `AQ READY`, `AQ TF`, `AQ WIFI_PROFILE` | `OS BOARD`, `OS CONFIG`, `OS DIAG`, `OS ERROR`, `OS GAUGE`, `OS MEMORY`, `OS OWNER_PIN`, `OS PMS`, `OS READY`, `OS TF`, `OS WIFI_PROFILE` |
| DIAG task names | `aq-lan`, `aq-board`, `aq-wdt`, `aq-diagnostic` | `os-lan`, `os-board`, `os-wdt`, `os-diagnostic` |
| DIAG BLE error key | `aq_ble_error` | `os_ble_error` |
| Kconfig symbols | `CONFIG_AQ_LOGGER_WORKER_WDT*`, `CONFIG_AQ_LOGGER_BENCH_STALL`, `CONFIG_AQ_LOG_PERSIST_*`, `CONFIG_AQ_PM_DFS_*` | `CONFIG_OS_` with the same suffix, no rename shim, an old saved value is dropped and the default applies |
| Release image names | `aq_cores3-bringup-<tag>.bin`, `aq_waveshare-logger-<tag>.bin` | `opensensor_cores3-bringup-<tag>.bin`, `opensensor_waveshare-logger-<tag>.bin` |
| Firmware identity | `idf-cores3-parquet-v6.15`, `idf-waveshare-parquet-v2.9` | `idf-cores3-parquet-v6.16`, `idf-waveshare-parquet-v2.10` |

`PARQUET ...` serial lines keep their names.

**The `oscfg` namespace starts empty.** Nothing is copied from `aqcfg`, so
after the reflash the board loses every stored setting, the pairing mode and
PIN, the display flag, Wi-Fi on, the SSID and password, the `lan.on` switch
(back to its default of on), the LAN sync token, the sensor identity
(`sensor.vendor`, `sensor.model`, `sensor.serial`, `sensor.batch_candidate`),
the location and the NTP settings. The pairing mode, PIN and display flag go
back to the first boot default. Until the sensor identity is set again,
Waveshare files carry no sensor identity and `pm25_batch_candidate_status`
stays 0, disabled. Provision all of them again and pair again, as on a new
board. The station UUID and the saved codec live in `parquet` and stay.

**Pair again cleanly.** Bonds survive the reflash and the phone may cache the
old GATT table, so forget the board in the phone's Bluetooth settings and send
`ble.clear_bonds=1` before pairing again.

**The old namespaces stay in flash.** The firmware never reads or erases
`aqcfg` and `aqdump`, so they still hold the old Wi-Fi password and LAN token.
Only a full flash erase removes them, and that also erases the station UUID.

**Why.** The product is OpenSensor. The owner dropped backward compatibility on
2026-10-26, so the wire and stored names move now instead of carrying two
names.

**What the app must do.** Browse `_ossync._tcp`, send `OSS1` in the LAN
handshake, use the new GATT UUIDs for the service, every characteristic and the
advert service data filter, match the `OS ` tags when it parses LOG_TAIL lines,
and run first time provisioning again after the reflash.

## Board template and desc digest in the firmware (fix/85-template-generator, issue #85)

**No app update needed yet.** A matched reflash brings the template, nothing
on BLE or LAN changes.

**What changed.** Each board now embeds one stac-opensensor v0.1.0 board
template, a minified JSON built on the host from `telemetry_fields.inc`,
`telemetry_semantics.inc`, the new `telemetry_classes.inc` (code and boolean
column classes, such as `pms_status` 0 `missing` with `nodata`) and the board
title and keywords. `desc` is the SHA-256 of exactly the embedded bytes. The
`parquet schema` console command now ends its BEGIN line with `desc=<sha256>
desc_bytes=<n>`. The dict digests, the Parquet files, LIVE keys and every BLE
reply other than the firmware identity are unchanged. The firmware identity
moves to `idf-cores3-parquet-v6.15` and `idf-waveshare-parquet-v2.9`, the
first builds that carry `desc`.

- CoreS3 `desc` `1b7d969abb670579cd1f34f0bd7e224f66b120ed178fbb2fcf024e14a46c3f08`,
  29,244 bytes, dict `4f80c26c...`.
- Waveshare `desc` `e6b32af089b5c0b8389999a09b4a81ffe36d15f908a2002765025f78853b7d32`,
  23,284 bytes, dict `6f567b38...`.

**Why.** The STAC contract keys each board build by `desc`, so a reader can
fetch one document that says what every column means, with its unit, role,
classes, gates and ranges.

**What the app must do.** Nothing today. When #86 puts `desc` on the wire,
copy it verbatim and use it as the key for the template. Never hash a
re-serialized copy, the digest covers the exact bytes.

## Per field LIVE precision and new dictionary digests (fix/91-live-precision, issue #91)

**Needs app update.** Bench boards need a matched reflash with the app build
that knows the new digests.

**What changed.** Each `FIELD` row in `telemetry_fields.inc` now ends with
`decimals`, the digits a float prints after the point in LIVE, chosen from the
sensor resolution. Integer fields carry 0. LIVE floats used to print with one
decimal everywhere. Now CoreS3 prints `t` with two decimals (SHT20 steps 0.01
degC, still always null there) and `imu_t` and `rh` with one. Waveshare keeps
one decimal for `t` and `rh` (PMS5003T steps 0.1). Columns outside LIVE carry
decimals too, for example CoreS3 accelerometer `g` with three and gyro `deg/s`
with two, and MAX17048 state of charge on Waveshare with two. The value is
rounded, so `1.00` and `1.0` both mean one. LIVE keys, key order, the null
rule, the all-null overflow and every other key are unchanged.

The new row argument moves the `info.dict` digest on both boards, and the
dictionary string moves with it, to `cores3-telemetry-v6` and
`waveshare-sim7670g-telemetry-v6`. The schema strings stay at `-v5` because the
columns are unchanged.

- CoreS3 `4f80c26cde8d7ea78c2160225d98386009a8651d9a049b4bf68a75da96015e94`,
  firmware `idf-cores3-parquet-v6.14`.
- Waveshare `6f567b38fecc165f544626168931b3dc3d674482da87ed78cb0bec5bf3dc6ceb`,
  firmware `idf-waveshare-parquet-v2.8`.

The widest real LIVE object is 333 bytes on CoreS3 and 270 bytes on Waveshare,
and the all-null object is 265 and 211 bytes, all under 480.

**Why.** The STAC contract (bridge decision 7) needs each field's precision in
the dictionary, so the digest says how a value prints. One decimal hid the
resolution of the finer sensors.

**What the app must do.** Map the two new digests to the CoreS3 and Waveshare
LIVE profiles. Parse LIVE values as numbers, never by text width, and keep the
app's own display rounding. Drop the previous digests `e5060fb7…` and
`67688d0f…` with the reflash. A board that reports an unknown or retired digest
still syncs its files but shows no LIVE readings, there is no key fallback
(opensensor-space-mobile #184).

## LIVE sends every declared key, null for no reading (fix/91-live-overflow, issue #91)

**Needs app update.** Bench boards need a matched reflash with the app build
that reads this rule.

**What changed.** Every LIVE key a board declares in its dictionary is now in
every LIVE object, over BLE and LAN. A key with no usable reading (invalid,
unavailable, NaN or Inf, out of range for the wire unit, or text of 32
characters or more) is sent as JSON `null` instead of being left out. An
absent key now means only that the board has no such field. A row too large
for 480 bytes no longer leaves the previous snapshot cached. The board sends
the object with every declared key `null` for that sample, as both the LIVE
notification and the cached value, and logs
`PARQUET LIVE overflow bytes=<n> max=480` at most once an hour. The widest
real object is 332 bytes on CoreS3 and 270 bytes on Waveshare, so overflow is
not expected on these boards. LIVE keys, key order, value formats, the
`info.dict` digests and every other key are unchanged. Per field LIVE
precision follows in a separate #91 change with new digests.

**Why.** The STAC contract (bridge decision 5) needs the declared key set to
be visible in every sample, so a phone can tell a missing reading from a
field the board does not have. Before, an overflowing row was skipped, so
BLE and LAN kept serving the older sample as if it were current.

**What the app must do.** Treat `null` as no reading, never as zero. Treat a
declared key that is missing as not on this board. Parse by key name and
ignore keys you do not know. An object where every key is `null` is a sample
with no readings, for example after an overflow, so clear the live values
instead of keeping the last ones.

## BLE name prefix OS- and board neutral footer values (fix/90-board-neutral-config, issue #90)

**What changed.** CoreS3 and Waveshare now advertise `OS-xxxx` (OpenSensor)
instead of `AQ-xxxx`, and the mDNS host is `os-xxxx.local` instead of
`aq-xxxx.local`. The prefix is a board constant passed through
`ble::Identity`, so a future board may use another one. The service UUIDs,
`_aqsync._tcp`, `AQS1`, opcodes and every `info`, status and live key are
unchanged. The footer keys `sample_interval_ms`, `row_group_rows_max`,
`deployment_id` and `calibration_id` keep their names, units and decimal text
format, and their default values (`10000`, `90`, `unknown`). They now come
from the logger config, and `sample_interval_ms` is the live sampling period
of the board that wrote the file.

**Why.** The shared logger must not carry one product name, so a new board
can ship with its own cadence, ids and prefix.

**What the app must do.** Stop filtering the scan with `AqGatt.NAME_PREFIX =
"AQ-"`. Scan by the GATT service UUID, or accept `OS-` (and later prefixes).
A board with an unknown prefix is otherwise invisible. Read
`sample_interval_ms` from each file footer as the truth (mobile #186 and #190)
rather than assuming 10 s. Existing bench boards must be reflashed together
with a matching app build and then re-paired, since the bonded name changes.

## Board neutral property ids (fix/84-telemetry-semantics, issue #84)

**What changed.** The `property=` value of each `PARQUET FIELD` line from the
`parquet schema` console command, and the `property` of each field in the
host dictionary JSON, is now a board neutral id from the board's
`telemetry_semantics.inc`. It was `urn:walkthru-earth:<board>:property:<column>`.
It is now a NERC P07 term, a NERC CF standard_name URI, or
`urn:walkthru-earth:property:<quantity>`, and a column both boards write has
the same id on both. The dict digest, the Parquet files, LIVE and every BLE
reply are unchanged.

**What the app must do.** Nothing today, since the app does not read these
ids. Any later code that joins on a property id should use the new ids, which
match the stac-opensensor template.

## PARQUET BEGIN first in each boot (fix-35-begin-order, issue #35)

**What changed.** The storage worker now waits until the logger has printed
`PARQUET BEGIN`, so in `LOG_TAIL` each boot's `PARQUET BEGIN` comes before its
worker lines, such as `PARQUET RECOVERY`, `PARQUET QUARANTINE`, `PARQUET SCAN`
and the mount error. Before, a fast card could print those first. The text of
every existing line is unchanged. One new line, `PARQUET ERROR
operation=worker-start-wait waited_ms=5000 order=unordered`, appears only if
the worker waited 5 s without the signal and started anyway. It raises the
error count, and worker lines of that boot may then come before BEGIN.

**Why.** Tools that split a log into boots at each `PARQUET BEGIN` could file a
new boot's recovery lines under the boot before it.

**What the app must do.** Nothing. An app that groups `LOG_TAIL` lines by boot
may now start a boot at its `PARQUET BEGIN`, while `begin()` lines such as
`PARQUET CODEC` and the watchdog reset error still come just before it. Lines
from older firmware may still show the old order. It may show
`worker-start-wait` as a storage error.

## Quarantine walk error lines (fix/iterative-dir-walk, issue #37)

**What changed.** The startup partial quarantine can print three new lines in
`LOG_TAIL`. `PARQUET ERROR operation=quarantine-depth dir=<relative path>
max_depth=6` names a directory too deep to enter. `PARQUET ERROR
operation=quarantine-entry name=<relative path> errno=<n>` names one entry
that could not be inspected or opened, which the walk skips before going on.
`PARQUET ERROR operation=quarantine-walk errno=<n>` reports a directory read
error that ended the walk early. All three raise the existing error count.
Before, those cases were skipped with no line, and one bad entry stopped the
rest of the walk.

**Why.** The walk is now iterative so its stack is fixed, and a skipped tree
must be visible.

**What the app must do.** Nothing. It may show these as storage errors.

## LIVE key stability rule (docs only, opensensor-space-mobile #153)

**What changed.** [ble-sync-protocol.md](ble-sync-protocol.md#live-key-stability)
now states that a LIVE key never changes meaning once released, and that a new
meaning gets a new key. Firmware releases that change the LIVE layout say
**needs app update** in their release notes and in their entry here. No
firmware, key, digest or identity string changes.

**Why.** A digest that moves only because of a comment or whitespace edit used
to stop live readings on every app that did not know it yet.

**What the app must do.** Keep mapping known digests to their reviewed
profiles. `imu_t` stays board temperature and `t` stays ambient air.

**History.** This entry first asked the app to read the LIVE table keys by
name for an unknown digest. That fallback never shipped. Since #91 an unknown
or retired digest syncs files but shows no LIVE (opensensor-space-mobile #184).

## Longer DIAG tasks and DIAG heap lines (fix-37-stack-budgets, issue #37)

**What changed.** The hourly `DIAG tasks` line in `LOG_TAIL` gains
`esp_timer=<bytes>` and `sys_evt=<bytes>` after `uptime_s`, and `DIAG heap`
gains `psram_largest=<bytes>` after `psram_min`. The board also prints both
lines when the owner sends `parquet mem` on the USB console. Existing fields
keep their names, order and meaning.

**Why.** Bench runs size task stacks and check heap fragmentation from these
lines, next to the static budgets in `tools/stack_budget.json`.

**What the app must do.** Nothing. The app does not parse these lines. A
reader that splits `key=value` pairs should ignore keys it does not know.

## Incremental LIST_SINCE with a saved cursor (fix-38-list-since, issue #38)

**What changed.** A new control opcode `LIST_SINCE` (`0x10`) lists only the
files of this station that are new since a 32-byte cursor. It answers `FILE`
frames, then a 48-byte `LIST_SINCE_END` (`0x4E`) whose first 15 bytes match
`LIST_END`, plus a `flags` byte and the next cursor. STATUS JSON gains
`"ls":<u32>`, the FAT volume serial of the mounted card, 0 when unknown. To
keep STATUS under 480 bytes, `utc_ms` is clamped to 13 digits and `clk_age_s`
and `miss` to the u32 range, which no valid clock reaches. Plain `LIST`,
`LIST_END` and every other frame are unchanged. The layouts, goldens and
errors are in
[ble-sync-protocol.md](ble-sync-protocol.md#list_since).

**Why.** Plain `LIST` reads every directory entry on the card each sync, which
grows with the archive. On a 5161-file test tree the incremental walk read 107
entries against 6688 for the full walk.

**What the app must do.**

- Store the cursor per station, keyed by station id plus the cursor's
  `mount_epoch`, as the 32 opaque bytes from `LIST_SINCE_END`. Never build or
  edit one. Send the all-zero cursor when none is stored, or when STATUS `ls`
  differs from the stored epoch.
- Save the new cursor only after a valid `LIST_SINCE_END`. An `ERROR`, a lost
  link or `ABORT` keeps the previous cursor, and the files already received
  are still worth downloading.
- On `ERROR` 11 with op `0x10`, fall back to plain `LIST` for that board and
  stop sending `LIST_SINCE` until the firmware version changes.
- Merge results into the local archive and dedupe by relative name plus CRC
  from `OPENED`. A `full` or `unsynced_full` listing repeats files the phone
  already holds, which is expected.
- Run a periodic audit, a plain `LIST` or `LIST_SINCE` with the all-zero
  cursor, for example once a day or every tenth sync. It catches a file
  written below the floor after a clock moved back across a reboot, and the
  unsynced files of a boot whose NVS ring write and `ringless=` marker
  folder both failed. A boot with only a failed ring write leaves the marker,
  and every later listing repeats its files in full until a later append in
  that boot removes it or the archive is erased, so dedupe
  them as above. Other stations,
  `legacy-parquet/` and `benchmarks/` also appear only in a full listing.
- Read `ls` as optional, absent on older firmware.

## One BLE HCI fail line per refused interval run (fix/52-followup-nits, after #52)

**What changed.** The board retries a connection interval request that is
refused while the central runs a procedure of its own. The link setup and the
idle timer retry every second, and a refused request from a `control` write
is retried by the next write. Each retry used to add `BLE HCI fail cmd=conn op=0x2013 rc=15 timeout=0` to serial and
`LOG_TAIL`. Now only the first refusal of a run logs it. A different `rc`, a
success, a new connection or a host reset starts a new run. The
`BLE CONN request=` line in `LOG_TAIL` still records every attempt with its
`rc`.

**Why.** Serial showed the same failure every second for as long as the
central's procedure ran, which buried the lines around it (PR #52 review).

**What the app must do.** Nothing. The app does not parse these lines.

## LZ4_RAW by default and a saved codec choice (feat/25-persist-codec, issue #25)

**What changed.** `parquet codec none` and `parquet codec lz4` now save the
choice in NVS (u8 key `codec` in the existing `parquet` namespace), so it
survives a reboot. A board with nothing saved, which is every board updating
from an earlier image, now writes LZ4_RAW files instead of UNCOMPRESSED.
What the phone can see.

- New files carry LZ4_RAW column chunks and footer `compression=LZ4_RAW`
  unless the owner chose `codec none`. Columns, field ids, types, the schema
  string, the dictionary digest and file names are unchanged.
- STATUS JSON `codec` reads `"LZ4_RAW"` right after boot instead of
  `"UNCOMPRESSED"`. The key and its two values are unchanged.
- LOG_TAIL gains a boot line `PARQUET CODEC codec=<name> source=nvs`, or
  `source=default reason=missing|open|read|corrupt` when nothing usable is
  saved. `PARQUET BEGIN codec=` now names the boot codec instead of always
  `UNCOMPRESSED`. The codec command reply is
  `PARQUET CONFIG codec=<name> persistent=true`, or `persistent=false` after
  `PARQUET ERROR operation=codec-store reason=open|write|commit`.
- No firmware identity string moves in this change, the next release image
  carries it.

**Why.** LZ4_RAW halved complete files on the CoreS3 bench (28,537 to
14,080 B) and finalized faster, and the host survey shows about 59 % fewer
data bytes on a 90 row file. Before this change every reboot fell back to
UNCOMPRESSED, so the saving was lost after the first reset.

**What the app must do.** Nothing required. `:core:parquet` already decodes
LZ4_RAW on Android and iOS, and files already stored keep their own codec. The
app does not parse the LOG_TAIL lines. An app that shows the STATUS `codec`
value should expect `LZ4_RAW` on most boards.

## COREDUMP line in the LOG ring (fix/49-coredump-report, part of issue #49)

**What changed.** The first boot after a new core dump is saved to flash
writes one additive line to the LOG ring, which `LOG_TAIL` returns.

```
COREDUMP present=1 valid=1 bytes=24992 sha=0708090a0b0c0d0e task=parquet-sd pc=0x4037a1b2 exc_cause=1 exc_vaddr=0x00000000 elf=3f2a1b0c prior=none saved=1
```

`valid` is `1` when the stored checksum matches, `0` when the dump is
corrupt and `unknown` when the flash could not be read. `bytes` is the image
size, `sha` the first 8 bytes of its stored SHA-256 in hex, `task` the
crashed task, `pc`, `exc_cause` and `exc_vaddr` the exception registers, and
`elf` a prefix of the crashed image's ELF SHA-256. Each of `task`, `pc`,
`exc_cause`, `exc_vaddr` and `elf` is `-` when the summary is not available.
`prior` is `none`, `other` (an older dump was reported before), `corrupt` or
`unknown` (the saved record could not be read). `saved` is `1` once the
device has recorded the dump as reported. When the dump cannot be identified
the line is `COREDUMP present=unknown valid=0 error=read-failed` or
`COREDUMP present=1 valid=0 error=invalid-size`. Nothing is saved for such a
dump, so that line repeats on every boot until the partition changes. Later
boots stay quiet for an identified dump unless the record could not be read or
saved. The dump is never
erased. No frame layout, opcode, error code, LIVE key or column changed.

**Why.** ESP-IDF printed the saved dump on the console only, so a crash in the
field left no trace in what the app collects.

**What the app must do.** Nothing. The line is plain LOG text. An app that
shows device logs may flag a `COREDUMP` line, and parse it as space separated
`key=value` pairs that may grow new keys. The values hold no secrets.

## Bonded reconnect serves the first LIST (fix/51-ble-auth-state)

**What changed.** On some bonded reconnects the board never saw the encryption
event, so its cached "authenticated" flag stayed false while the link was in
fact encrypted. The first `LIST` after such a reconnect was refused with
`ERROR` 7 and that `ERROR` was dropped too, so the app saw nothing. The board
now reads NimBLE's live security state, so that `LIST` is served. Code 7 is
never sent for an authentication reason any more. An unpaired phone still
sees what it saw before. NimBLE refuses its unencrypted Control write with an
ATT Insufficient Encryption, Encryption Key Size or Authentication error, which
starts pairing. It can still subscribe to `response`, because the CCCD has
no security flag, but receives nothing there except a possible `ERROR` 14.
`ERROR` 14 with `detail="auth"` over BLE is only a guarded fallback, for a
Control write that passes ATT but fails the firmware check. In this
configuration both apply the same rule, so an app should not expect to see it.
Serial and `LOG_TAIL` gain a line `BLE BONDED authenticated=true bonds=N
via=access`, `via=connect` or `via=subscribe` when the board learns the link
is secure without the encryption event. A later encryption event on that
link logs no second `BLE BONDED` line. A bonded central that encrypts
before the board processes the connection (macOS does) now keeps the
`response` subscription NimBLE restores from the bond. Before, that
subscription was lost, so every frame of the first `LIST` was refused and
its closing `ERROR` 7 never arrived. The board logs such a restore as
`BLE SUBSCRIBE early=N response=1`. A response frame refused because
`response` is not subscribed logs `BLE REFUSE reason=unsubscribed`, and one
refused because the link is not secure logs `BLE REFUSE reason=insecure`,
each once per session. No frame layout changed.
The board also stopped rebooting on some bonded reconnects. It used to
request the LE data length while handling the connection, and when that
request got no answer while the phone started encryption, the BLE host reset
and the board crashed. The data length request now goes once the link is
secure, before the 2M PHY request, and is skipped when the phone already set
251 octets. Serial and `LOG_TAIL` gain `BLE HCI fail cmd= op= rc= timeout=`
when a link request fails (`timeout=1` means the BLE host resets), and
`BLE LINK wait phy after=mtu`, `after=conn` or `after=phy`,
`BLE LINK dle=N by central` and `BLE LINK phy=2m by central` while the link
sequence runs. The app does not parse any of these lines.

**Why.** Issue #51, a reconnect looked like a hung sync.

**What the app must do.** Keep treating an ATT Insufficient Authentication,
Encryption or Encryption Key Size error on a Control write as "pair again".
That is the normal unpaired case. Should `ERROR` 14 ever arrive on BLE, treat it the same way,
like a bad LAN token. Never retry either as busy. Handle 7 as
before, by its `detail`. A plain busy 7 can be retried later, while
`detail="mtu"` needs a larger MTU and `detail="count"` will not succeed by
repeating the same request. Nothing for the new link lines. An app that shows
device logs may flag `BLE HCI fail`.

## ABORT stops a LIST within one step (fix/33-abort-list-walk)

**What changed.** The device walks the card for `LIST` in steps of at most 32
directory entries and checks for ABORT before each step and each `FILE`. An
ABORT now stops a `LIST` within one step, even on a large card. Before, the
walk only stopped at the next `FILE`, and the whole walk ran first. The reply
is unchanged, `FILE` frames already sent, then `ERROR` 16, then `ABORTED`
naming `LIST`. Other links and LAN peers are served between steps. A file
finalized during the walk may be missing from that listing, and `new_files`
then stays set in the advert. `LIST_END.partials` is approximate when a
`.partial` file is finalized during the walk. Only one `LIST` runs at a time,
and a `LIST` from another link or LAN peer now gets `ERROR` 7 `detail="list"`
at once instead of waiting for the running one. A full card no longer
remounts during a `LIST`, so a long walk is not cut off every 60 s. A remount
that still ends a `LIST`, for example after `FLUSH` or a config change, now
answers `ERROR` 7 `detail="remount"` instead of `ERROR` 8, because the card
comes back and the app should retry. An erase or a directory read failure
still answers `ERROR` 8. When the device needs its storage worker for
something more urgent, such as a critical battery close, the walk ends early
with `ERROR` 7 `detail="preempted"` instead of after the whole walk, and a
`LIST` sent while that close is due gets the same reply at once. No frame
layout, opcode or error code changed.

**Why.** On a large card the abort drain could outrun the app's bound of 4096
frames, 4 MiB or 30 s and close the link with `DrainExceeded`. A long walk
also held the storage worker away from samples and the watchdog heartbeat.

**What the app must do.** Nothing. `LinkSession` already accepts any number of
`FILE` frames, then `ERROR` 16 and `ABORTED` in either order, and only checks
that the `FILE` count matches `LIST_END.count`. The drain now ends sooner.
A preempted `LIST` is `ERROR` 7, which `DeviceSession` already maps to
`DeviceBusyException`, and `CollectionController` retries it after its
backoff like any busy reply. The same holds for the `detail="list"` busy
reply, which an app syncing over BLE and LAN at once can see on its second
link, and for `detail="remount"`. Before, a `LIST` cut by a remount ended the
collection as a device failure with no retry. The `DeviceBusyException` KDoc
should list the LIST details `preempted`, `list` and `remount`. On a critical
battery the device makes up to 8 close attempts with about 91 s of backoff
between them, and each retry can preempt a `LIST` again, so the app can spend its 5 attempt
retry budget and end with link busy. That is accepted on a dying battery and
needs no app change.

## CoreS3 LIVE keys covered by the dictionary digest (CoreS3 v6.13, issue #31)

- CoreS3 LIVE keys now live in `telemetry_fields.inc` as the last `FIELD`
  argument, the same as Waveshare, so the dictionary SHA-256 covers them.
- CoreS3 writes dictionary `cores3-telemetry-v5`, firmware
  `idf-cores3-parquet-v6.13`. Schema stays `cores3-telemetry-v5` (row value 5)
  with the same 81 columns, order, types and field ids. Configuration stays
  `cores3-acquisition-v3`.
- The new digest is
  `e5060fb7fedbcabf665b1c8a159a77f5fe9283ac34bd2d89f1bc98b0f027d38b`. Its LIVE
  keys, in order, are `seq`, `mono`, `utc`, `pms`, `c1`, `c25`, `c10`, `pm1`,
  `pm25`, `pm10`, `n03`, `n05`, `n1`, `n25`, `n5`, `n10`, `imu_t`, `als`,
  `vbus`, `bat`, `pct`, `chg`, `t`, `rh`. These are the same keys as the
  previous build. `t` and `rh` are mapped but always null on CoreS3.
- The legacy digest
  `22291d3aab63365404394f30c1549fb18bde0699df9b2de863de70bb3f4edf65` covers
  two released LIVE key sets that both report `idf-cores3-parquet-v6.12`. The
  build at e166978 sends the IMU die temperature as `t`. The build at d2a840c
  sends it as `imu_t`. Nothing in `info` tells them apart.
- The app must map `e5060fb7…` to the existing `LiveProfiles.CORES3` profile
  from opensensor-space-mobile #87, unchanged. That profile leaves out `t` and
  `rh` on purpose because they are always null on CoreS3, so do not copy the
  key list above into it. It reads `imu_t` as board temperature, never as air
  temperature. Tracked in opensensor-space-mobile #90.
- Until the app knows `e5060fb7…`, `LiveProfiles.resolve` returns null for it
  and the app shows "Live readings need newer firmware". Synced files still
  work.
- Release order. Ship the app release that maps `e5060fb7…` first, then flash
  v6.13 to field boards. Bench boards may run v6.13 earlier.
- Suggested mobile follow up for `22291d3a…` only. Read `imu_t`, and when it is
  missing read `t` as board temperature. Both carry the IMU die temperature on
  CoreS3, because ambient temperature is never valid there.
- Recorded key sets for every digest since CoreS3 `f35d7398…` (v6.9 to v6.11)
  and Waveshare `0a024b85…` (v2.6) are in `tools/fixtures/live_keys/`,
  including the first Waveshare v2.7 build `d0d01294…`. `f35d7398…` sends the
  IMU die temperature as `t`, like the e166978 build, so it matches
  `LiveProfiles.CORES3_LEGACY` in #87. The contract tests fail when a board's LIVE keys
  change without a new digest, or when a new digest has no fixture.
- The digest is the SHA-256 of the raw `telemetry_fields.inc` bytes, so a
  comment, whitespace or line ending edit also moves it and needs an app
  release. History, the unknown digest fallback planned in
  opensensor-space-mobile #153 never shipped, an unknown digest shows no LIVE
  (#184).
- Phone side code may break freely. Firmware keeps every released wire contract
  readable, and any new LIVE key set gets a new dictionary digest.

## Battery health, cell voltage without SOC and the emergency close (Waveshare v2.7, PR #24, issues #28, #29 and #30)

This one entry describes the final v2.7 contract. CoreS3 is unchanged.

- Schema and dictionary. Waveshare writes `waveshare-sim7670g-telemetry-v5`
  (row value 5), firmware `idf-waveshare-parquet-v2.7`, configuration
  `waveshare-sim7670g-acquisition-v3`, 62 columns. The v5 dictionary digest is
  `67688d0fd2e86053c1c66b961a1446ad3c225ee71823e0fadb771b645c7183ff`.
- Six columns are appended, field ids 57 to 62.
  `battery_soc_raw_percent` (FLOAT, uncapped gauge SOC),
  `battery_charge_rate_percent_per_h` (FLOAT, MAX17048 CRATE, negative while
  discharging), `gauge_status_flags` (INT32, bit0 RI, bit1 VH, bit2 VL, bit3 VR,
  bit4 HD, bit5 SC, bit6 EnVr), `gauge_hibernating` (INT32, 0 or 1),
  `battery_health_state` (INT32 code) and `battery_minutes_to_empty` (INT32,
  null unless the SOC is trusted and discharging within 7 days).
- `battery_health_state` and the LIVE key `bstate` share the codes 0 unknown,
  1 external power likely, 2 ok, 3 low, 4 critical, 5 settling after a gauge
  reset. External power is inferred from cell voltage only, and the
  thresholds are uncalibrated. `bstate` is absent when no battery is
  configured. At boot a cell resting on the critical threshold still leaves
  unknown after three reads, as critical when at least half of them read
  critical, otherwise as low.
- Cell voltage without SOC. A failed or implausible SOC no longer hides a good
  cell voltage. Rows can have `battery_mv` set while `battery_percent`,
  `battery_soc_raw_percent` and `battery_minutes_to_empty` are null, with
  `gauge_status=1`, and the reverse is possible. LIVE follows the row, so
  `bat` can be present without `pct` and the reverse. `bstate` can reach 4
  critical from voltage alone. The `gauge_status` footer metadata text now
  reads `...2=valid; gauge_status stays 1 when only VCELL is valid,
  battery_mv is still written`. The codes 0, 1 and 2 keep their meaning, and
  2 still needs both words. This text is file metadata, not part of the
  digest.
- LIVE keys live in `telemetry_fields.inc` as the last `FIELD` argument, so
  the digest covers them. The keys and their order are `seq`, `mono`, `utc`,
  `pms`, `c1`, `c25`, `c10`, `pm1`, `pm25`, `pm10`, `n03`, `n05`, `n1`, `n25`,
  `t`, `rh`, `bat`, `pct`, `bstate`. Any change to a board's
  `telemetry_fields.inc`, including a live key, changes that digest, so it
  needs a matching app release before boards are reflashed.
- LOG_TAIL power lines. `POWER STATE state=<name> mv=<n|null>` on each state
  change, with ` action=finalize` once per critical entry. Boot adds `AQ GAUGE
  status=...`, after a gauge reset `AQ GAUGE ri_clear=<result>`, and `AQ GAUGE
  read` carries `vcell_ok`, `soc_ok`, `vcell_plausible` and `soc_plausible`.
- LOG_TAIL emergency close lines. The close runs on the storage worker with
  retries. `files=<f>` counts telemetry files finalized during the attempt,
  including one a storage probe closed before the close itself wrote.
  - `PARQUET FLUSH reason=power-critical result=closed attempt=<n> files=<f>`
  - `PARQUET FLUSH reason=power-critical result=<busy-retry|failed>
    storage=<state> attempt=<n> files=<f> retry_ms=<ms>`
  - `PARQUET FLUSH reason=power-critical result=nothing-open attempt=<n>
    files=<f>`, only after a retry. A first attempt with nothing open prints
    no line.
  - `PARQUET ERROR operation=power-close reason=<outcome> attempts=<n>
    files=<f> closed=0` when every retry failed.
- Transfers during a close. A BLE or LAN `LIST` or `READ` running when a close
  is due ends early with the existing busy error (`ERROR busy` for LIST,
  `READ_END` with the busy status for READ), also while it waits for a slow
  peer. A serial `parquet get` ends with `PARQUET ERROR operation=get
  reason=power-critical offset=<n>`, where `offset` is the next byte not sent.
  The app retries as it already does for busy. A close in storage Retry can
  remount the card, so an open READ handle may then answer bad handle, as in
  the #41 entry below.
- One discharge closes once. `bstate` can go 4 critical, 3 low and back to 4
  critical without a second close. Only 2 ok or 1 external power re-arms it,
  and not while 5 settling.
- No STATUS, GET_DIAG, GET_CONFIG, frame layout or advertising change.
- The app must accept schema 5 and dictionary v5 for Waveshare files, keep
  reading v4 files already stored, must not assume `battery_percent` or `pct`
  is present whenever `battery_mv` or `bat` is, and may show `bstate` as a
  battery badge.

## READ after a storage remount can answer bad handle (fix/34-remount-archive-handles)

**What changed.** When the device remounts the SD card while a phone holds an
open handle, the next `READ` on that handle runs the finalized check again. The
same size and CRC-32 keep the handle. A deleted, edited or swapped file now
answers `ERROR` 5 (bad handle) for that `READ` and every later `READ` or
`CLOSE` on the handle. A card that has not mounted again yet, or an I/O error
during the check, answers `READ_END` status 4 and keeps the handle, so a later
`READ` can still succeed. Before this change the device reopened the path and
served the new bytes under the old handle. No frame layout or opcode changed.

**Why.** A card can be pulled, edited on a PC and put back, or swapped, while
the phone keeps its handle. The old reopen mixed bytes from two files under
one size and CRC, so the phone only caught it at its own CRC check.

**What the app must do.** Nothing. `SyncEngine` already turns a READ
`ERROR` 5, and any non-zero `READ_END` status, into `FileOutcome.DeviceError`
for that file without reopening or looping, logs a refused `CLOSE` and moves
on to the next file. The next run
lists again and resumes the partial only if `OPENED` size and CRC still match.

## Smaller resident writer workspace (feat/multi-chip-portability-and-watchdog)

- `PARQUET BEGIN psram_workspace_bytes=` drops by about 51 KB because the
  codec-test benchmark file is no longer resident. Waveshare V2 at schema v4
  measured 162520 bytes on 2026-10-16. Value change only.
- LOG_TAIL `PARQUET ERROR operation=codec-test reason=no-psram` is replaced by
  `PARQUET ERROR operation=allocate mem=psram bytes=N purpose=codec-test`.
- A failed workspace allocation at boot now logs
  `PARQUET ERROR operation=allocate mem=psram bytes=N purpose=writer-state
  logging=false`. Other boot allocation failures keep the old line.
- The app does not parse these lines, so no app change is required.

## Storage worker watchdog (feat/multi-chip-portability-and-watchdog)

- LOG_TAIL `PARQUET ERROR operation=worker-stall` gains a trailing
  `watchdog=armed|warn|off` field. Additive.
- New LOG_TAIL lines `PARQUET WATCHDOG mode=... timeout_s=... consecutive=...`
  at boot and `PARQUET ERROR operation=worker-watchdog-reset consecutive=N`
  after a stall reset. Additive.
- A stalled storage worker now resets the device after about 60 s instead of
  staying wedged. The app sees a normal BLE/LAN disconnect and reconnect.
- New LOG_TAIL line `PARQUET WATCHDOG mode=armed consecutive=N
  reason=recovered` when a stalled worker moves again before the reset. The
  device keeps running and a later stall can trip again. Additive.
- `PARQUET WATCHDOG ... supervised=0` now comes with `mode=warn`, since a
  worker without its supervisor timer is never left armed.
- A serial `parquet get` and its CRC pass count as worker progress, so a
  large USB export no longer logs `worker-stall` or trips the watchdog.
- No STATUS, LIVE, GET_DIAG, GET_CONFIG or advertising field changes. The app
  does not parse these log lines today, so no app change is required.

## CoreS3 acquisition configuration v3 (refactor/board-neutral-logger-and-sensors)

- The CoreS3 file metadata `configuration_id` changes from
  `cores3-acquisition-v2` to `cores3-acquisition-v3`, and the `configuration`
  JSON now records `"m5unified":"0.2.26"`, the version the firmware is built
  with. It said 0.2.25 since the dependency moved in #19.
- Schema, dictionary, columns and firmware identity are unchanged.
- The app does not need to change unless it pins `configuration_id`. If it
  does, accept `cores3-acquisition-v3`.

## LIVE keys come from each board's field table

**What changed.** The `live` notification and the LAN `LIVE` push no longer
use one hardcoded list of 22 keys. Each board declares a `live_key` next to the
fields it wants on LIVE, and the shared logger emits exactly those keys, in
the board's field-table order, for values that are valid in the queued row.

- `t` now always means ambient air temperature. CoreS3 has no resolved ambient sensor,
  so `t` stays absent there.
- CoreS3 sends its IMU die temperature as the new key `imu_t`. Before this
  change it was sent as `t` whenever ambient temperature was null.
- Key order follows the board dictionary. For example `c1`, `c25`, `c10` now
  come before `pm1`, `pm25`, `pm10` on both boards.
- Non-finite float values are omitted like any other null.

**Why.** The shared logger carried board knowledge (which fields exist and the
`t` fallback rule) that belongs in the board. A key with two meanings also
made the LIVE temperature tile depend on the board type.

**What the app must do.**

- Treat `t` as ambient air temperature only. Show `imu_t` as board or die
  temperature, never as air temperature.
- Parse LIVE as an unordered JSON object and look keys up by name.
- Ignore keys it does not know, and treat a missing key as null.
- Use `info.schema` and the dictionary to decide which tiles a board can
  ever show, instead of a fixed key list.

## UTC columns in microseconds and Parquet field ids (CoreS3 v6.12, Waveshare v2.6)

**What changed.**

- `event_time_utc_ns` is now `event_time_utc_us` and `clock_anchor_utc_ns` is
  now `clock_anchor_utc_us`. Values are INT64 microseconds since the Unix
  epoch, annotated `TIMESTAMP(MICROS, isAdjustedToUTC=true)`. The dictionary
  unit is `us_since_unix_epoch` (was `ns_since_unix_epoch`).
- Every leaf column now carries a Parquet `field_id`. The id is the column's
  1-based position in the board's `telemetry_fields.inc`, and dictionaries are
  append only, so an id never moves to another column.
- CoreS3 writes schema `cores3-telemetry-v5` (row value 5), dictionary
  `cores3-telemetry-v4`, firmware `idf-cores3-parquet-v6.12`. Waveshare writes
  schema and dictionary `waveshare-sim7670g-telemetry-v4` (row value 4),
  firmware `idf-waveshare-parquet-v2.6`. Both dictionary SHA-256 values change.
- Unchanged. The `utc` key in the LIVE document is still nanoseconds. The
  footer key `reconciliation_anchor_utc_ns` is still nanoseconds. Column count,
  column order and all other names and types are unchanged.

**Why.** The phone or host registers device files in Apache Iceberg v3 tables.
`TIMESTAMP(MICROS, UTC)` is Iceberg `timestamptz` in every format version, so
the files go into a v3 table (or an older v2 table) without rewriting. The
device clock is microsecond, so NANOS would add no precision, would end in
2262, and would map only to the v3 type `timestamptz_ns`, which Spark, Athena
and Redshift cannot read. Iceberg identifies columns by field id, not by name,
so every leaf now carries one.

**What the app must do.**

- Read the UTC instant from `event_time_utc_us` when present and fall back to
  `event_time_utc_ns` for older files. Convert by unit, never by name alone.
  Check the Parquet logical type (`MICROS` or `NANOS`).
- Accept schema versions 5 (CoreS3) and 4 (Waveshare) and the new dictionary
  digests. Do not reject a file only because its schema string is newer.
- Create Iceberg tables as format v3. When building a manifest or schema, use
  each leaf's `field_id` as the Iceberg field id. Leave row lineage fields to
  the committing library, the device does not write them.
- Older files have no ids and store UTC as plain INT64 or NANOS. A name mapping
  cannot change a column's type, so keep them out of the Iceberg table or
  rewrite them on a host.
- Commit with a library that supports v3 `add_files` style registration, for
  example iceberg-go 0.7.0 or later, or iceberg-rust 0.9.0 or later with data
  file entries built from the footer metrics. PyIceberg 0.12 cannot write v3
  tables yet.
- When reconciling unsynchronized files from footer anchors, compute
  microseconds as `reconciliation_anchor_utc_ns / 1000 + (monotonic_us -
  reconciliation_anchor_mono_us)`.
- Keep treating LIVE `utc` as nanoseconds.
