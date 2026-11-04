# OpenSensor Thing Profile

| Item | Value |
| --- | --- |
| Title | OpenSensor Thing Profile |
| Profile schema | <https://walkthru-earth.github.io/stac-opensensor/v0.1.0/profile.json> |
| Template schema | <https://walkthru-earth.github.io/stac-opensensor/v0.1.0/template.json> |
| Footer schema | <https://walkthru-earth.github.io/stac-opensensor/v0.1.0/footer.json> |
| Version | 0.1.0 |

The [extension](https://github.com/walkthru-earth/stac-opensensor/blob/83787768cc8451109d48a03d4ef5493ffcd403dd/README.md) defines fields. This profile fixes how a whole catalog of Things uses them,
so any client can read any Thing without guessing.
A Thing is the OGC SensorThings term for a device that carries sensors. It is fixed in one place or moving.
The profile says which extensions each document must declare, which fields each column must carry and which vocabularies to use.

A document follows the profile when it lists the profile schema in `stac_extensions`.
STAC validators then check the profile like any other extension.

- [Full example](#full-example)
- [Profile schema](https://github.com/walkthru-earth/stac-opensensor/blob/83787768cc8451109d48a03d4ef5493ffcd403dd/json-schema/profile.json)
- [Template schema](https://github.com/walkthru-earth/stac-opensensor/blob/83787768cc8451109d48a03d4ef5493ffcd403dd/json-schema/template.json)
- [Footer schema](https://github.com/walkthru-earth/stac-opensensor/blob/83787768cc8451109d48a03d4ef5493ffcd403dd/json-schema/footer.json)
- [Decision register](https://github.com/walkthru-earth/stac-opensensor/blob/83787768cc8451109d48a03d4ef5493ffcd403dd/docs/decisions.md)

## Versioning

The extension schema, the profile schema, the template schema and the footer share one version, now 0.1.0.
The template carries it as `template_version` and the footer as `version`.
While the version is 0.x, a minor bump is a breaking change.
Readers compare the major and minor parts and refuse a pair they do not know. A patch bump never breaks a reader.
Once a version is published, its schema files are never edited in place. A fix ships as a new version.
The schemas use the `$id` host `walkthru-earth.github.io`, which answers with a 301 redirect to `walkthru.earth`, the organization's own domain.
The `$id` stays as it is. Validators follow the redirect and fetch the same bytes.

## Layers

One Thing is described by three layers. Each layer has one owner.

| Layer | Owner | Where | Holds |
| --- | --- | --- | --- |
| Board template | Firmware build | Firmware repo, `docs/shared/templates/<board>/<dict>.json` at a pinned commit | Columns and their meaning, sample interval, instruments, LIVE keys, protocol floor |
| Thing Collection | The board, see [Writers](#writers) | `thing=<uuid>/collection.json` | Template columns, mobility, location extent, partitions, license and links |
| App profile | Mobile app | Outside this repository | Card order, hero card, display step and column labels in each language |

The board template is plain JSON, not STAC. A firmware build generates it, and its SHA-256 is the descriptor digest.
A device can serve it over BLE or LAN. A Thing Collection is the template's columns with facts about the Thing added.
The app profile is the app's own choice and never goes into published STAC.

### Template hosting

A template is found by an immutable URL, commit pinned or content addressed, whose bytes hash to the digest that names it.
The canonical publisher is the firmware repo pinned to a full 40 character commit, at `docs/shared/templates/<board>/<dict>.json`.
A catalog MAY mirror a template at `templates/<board>/<template sha256>.json`.
Readers MUST check the SHA-256 of the bytes they fetch against the footer `template.sha256` or the link `file:checksum`.
No schema can tell that a URL is immutable, so that check is a reader MUST, and a URL on a moving branch fails it once the branch moves.
Template bytes are minified, carry no final newline and are never reformatted, so the digest stays stable.
The `templates/` folder of this repository holds example copies only. It is not a template host.

## Catalog layout

```text
catalog.json                                          root Catalog, one child link per Thing
thing=<uuid>/collection.json                          Thing Collection, columns in item_assets
thing=<uuid>/items.parquet                            optional item mirror, every file Item as one row
thing=<uuid>/year=YYYY/month=MM/day=DD/<file>.parquet one uploaded file, at most 1 hour
thing=<uuid>/year=YYYY/month=MM/day=DD/<file>.json    its Item, same name
templates/<board>/<template sha256>.json              optional template mirror
```

`<file>` is the board's own name, `data_HHMM_<boot>_<first>-<last>-<attempt>`.
`HHMM` is the UTC start of the rotation window that holds the first row, and `<boot>` is the 32 hex boot id from the footer.
The window start is floor(first row seconds ÷ `rotation_interval_s`) × `rotation_interval_s`, as the firmware logger computes it.
A board that boots at 13:07 with a 900 s window writes `data_1300_…` whose first row is 13:07.
`<board>` is the board slug, such as `m5stack-cores3`.
`<dict>` is the 64 hex digest of the firmware dictionary. The footer holds it as `dictionary.sha256` and the template as `firmware.dict`.
A template is keyed by `<dict>` because one schema can ship with several dictionaries.

Only dated `data_HHMM_` files are published. Unsynced, benchmark and other board paths stay on the device.
A board closes a file at the end of every rotation window and uploads it at once.
The window is 600, 900, 1800 or 3600 s, 900 by default. Each divides 3600, so a window never crosses an hour or a day,
and the date of the window is the date of its first row.
So the newest data is at most one window old.
Two files may share a window after a reboot or a retried upload. Readers order rows by time and drop repeats of the same Thing and time.
Files written before the board knew UTC time get no Item until they are reconciled.
Day or longer roll-ups are derived products under a separate prefix, never inside `thing=<uuid>/`.

## Partitions

A Thing's files are a Hive partitioned table. The Collection says so with the
[Partition extension](https://github.com/portolan-sdi/stac-partition-extension) v1.0.0, which is incubating.
A reader can then query every file of a Thing in one call without walking Items.

| Field | Value |
| --- | --- |
| `partition:scheme` | `hive` |
| `partition:strategy` | `temporal` |
| `partition:keys` | `year`, `month` and `day`, in that order, each `int32`, the UTC date of the rotation window, with the description `UTC <key> of the file window, zero padded on disk` |
| `partition:glob` | `s3://walkthru-earth/opensensor-space/thing=<uuid>/year=*/month=*/day=*/*.parquet` |

Partition values are written zero padded, `year=2026/month=09/day=05`, so paths sort by date.
They are read as `int32`. A reader that infers types sees `09` as text, so it MUST cast the keys.
The glob uses the S3 scheme because a glob needs listing, which plain HTTPS does not give.
The bucket and prefix come from the owner's upload target. In the example the S3 endpoint is `https://data.source.coop`.
`partition:file_count` is left out, because new files keep landing.
`thing` is not a key of one Thing, since it is fixed for the whole Collection.
A query across Things globs `thing=*/year=*/month=*/day=*/*.parquet` from the catalog root.

### Reading

Files of one Thing can come from several board templates after a firmware update, so their columns can differ.
Readers union by column name and never by position.
In DuckDB that means `union_by_name = true`, plus `hive_types` so the padded keys are integers.

```sql
SELECT * FROM read_parquet(
  's3://walkthru-earth/opensensor-space/thing=3f2b9c1e-6a7d-4e58-9b1c-2d4f6a8e0c13/year=*/month=*/day=*/*.parquet',
  hive_partitioning = true,
  hive_types = {'year': 'INTEGER', 'month': 'INTEGER', 'day': 'INTEGER'},
  union_by_name = true)
WHERE year = 2026 AND month = 10 AND day = 24;
```

Without `hive_types`, DuckDB reads `month=09` as the text `09` and `month = 9` matches nothing.

A Thing writes 24 to 96 Items a day. A catalog host MAY add a
[stac-geoparquet](https://radiantearth.github.io/stac-geoparquet-spec/latest/) item mirror at
`thing=<uuid>/items.parquet`, so a client reads all Items in one request. It is a Collection asset with type
`application/vnd.apache.parquet` and role `collection-mirror`. The Item JSON stays the source of truth,
and the mirror MUST hold exactly the published Items, one row each, or not exist.

## Writers

Each document has one writer. Firmware [DR 0001](https://github.com/walkthru-earth/opensensor-space-firmware/pull/137) records the same split.

| Document | Writer | When |
| --- | --- | --- |
| File Item | The board | At file close, as the sidecar `<file>.json`, only for dated `data_HHMM_` files |
| Thing Collection | The board | From its upload ledger and owner config, after each upload |
| Root Catalog | The catalog host's publish job | When a Thing joins or leaves the catalog |
| `items.parquet` | The catalog host | Optional, rebuilt from the Item JSON |

The board writes `collection.json` and every `<file>.json` and never reads the bucket.
The catalog host writes only `catalog.json` and `thing=<uuid>/items.parquet`. It never edits a board document.

The phone app and an indexer transport and validate these documents.
They MAY rebuild a File Item or a Thing Collection from the files, the footers, the templates and the owner config.
A rebuilt document MUST equal the board's under [Canonical form](#canonical-form), and a tool never publishes one that differs.
The phone writes a File Item only when it is the Thing, that is when the phone itself logs and uploads the file.
When the phone uploads a board's file after a BLE or LAN sync, the file and its Item are published,
but the Collection does not list it in its extent until the board learns of that upload. Telling the board is future work.

The File Item travels with its Parquet file, so the data `href` is relative, `./<file>.parquet`.
Its links are relative too. `root` is `../../../../catalog.json`, and `parent` and `collection` are `../../../collection.json`.
A standalone owner bucket with no catalog host MAY use the Thing Collection as its root, and `root` links then point at it.

The board fills the Thing Collection from four sources.

| Source | Fields |
| --- | --- |
| Template | `item_assets.data` columns, `table:primary_datetime`, `table:primary_geometry`, `keywords`, `instruments`, `opensensor:sample_interval_s`, the dictionary link |
| Footer of the newest file | `id` from `thing.id`, `opensensor:mobility`, the template link `href` and `file:checksum` |
| Upload ledger | `extent`, the union of uploaded Item boxes and the earliest uploaded `start_datetime` |
| Owner config | `title`, `description`, `providers`, contacts, the bucket and prefix in `partition:glob`, column `title` and `opensensor:scale`, `opensensor:scales` |

The owner sets the owner config at setup, and the board stores it. Everything else is fixed by this profile.
A board never shrinks `extent`. It keeps the ledger summary, the first start time and the union box, in NVS,
so an SD card swap or a lost ledger file does not reset it.

### Canonical form

Two writers that follow these rules produce equal documents.
Equal means the same JSON value. Key order and white space do not matter, array order does.
Numbers compare as IEEE 754 doubles, so any printing that reads back to the same double is fine, such as C `%.17g`.
Geometry coordinates, `bbox` and the Collection `extent.spatial` compare within 1e-9 degrees.
[scripts/canonical.mjs](https://github.com/walkthru-earth/stac-opensensor/blob/83787768cc8451109d48a03d4ef5493ffcd403dd/scripts/canonical.mjs) is the reference builder, and `npm run check-profile` rebuilds every example from its rows.
Every rule fits an ESP32-S3. Each needs one pass over the rows, sums in doubles and H3 C.

| Topic | Rule |
| --- | --- |
| Datetimes | RFC 3339 in UTC with `Z`. Whole seconds have no fraction, otherwise the microseconds with trailing zeros removed, such as `13:00:00.4123Z` |
| Item times | `start_datetime` and `end_datetime` are the smallest and largest `event_time_utc_us`, `datetime` is `null` |
| Item `opensensor:sample_interval_s` | The footer `sample_interval_ms` ÷ 1000 |
| Statistics columns | Every column whose role is `physicalMeasurement` or `modelResult` and whose type is numeric, int8 to uint64, float or double, in template order, and only columns the file has |
| Statistics entry | `name`, `type` and `statistics`, nothing else. `table:columns` is left out when no column qualifies |
| Counted rows | The value is not null, is finite after a float32 value is widened exactly to a double, and passes `opensensor:valid_when`. A gate on a missing or null column fails. `opensensor:settle_s` does not remove rows |
| `minimum` and `maximum` | The widened values, unrounded |
| `mean` | The sum of counted values in row order ÷ their count, rounded to 2 decimals |
| `valid_percent` | 100 × counted rows ÷ `table:row_count`, rounded to 2 decimals. A column with no counted row has only `valid_percent` `0` |
| Rounding | Half away from zero, sign(v) × floor(\|v\| × 100 + 0.5) ÷ 100 in doubles |
| Item `stac_extensions` | Table, File, Grid for a fixed Thing only, Anonymized Location, the OpenSensor extension, this profile, in that order |
| Item `links` | `root`, `parent`, `collection`, in that order, each with type `application/json` |
| Item data asset | `href`, `type` `application/vnd.apache.parquet`, `roles` `["data"]`, `file:size`, `file:checksum`, `table:row_count` and the statistics |
| Item properties | `datetime`, `start_datetime`, `end_datetime`, `opensensor:mobility`, `opensensor:sample_interval_s`, `grid:code` for a fixed Thing, `anon:size`, `anon:warning` |
| Geometry | The steps in [File Item](#file-item). H3 C 4.5.0 is the reference, the version the firmware vendors and h3-js 4.5.0 wraps |
| Collection `stac_extensions` | Table, CF, Classification, File, Partition, Contacts when the owner gives a contact, the OpenSensor extension, this profile, in that order |
| Collection `links` | `root`, `parent`, the dictionary `describedby`, the template `describedby` titled `Board template`, `license`, in that order |
| Collection `extent` | One `bbox` from the ledger and one interval from the ledger start to `null` |
| Collection columns | The template columns in template order, each with only the owner's `title` and `opensensor:scale` added. `opensensor:scales` is set only when a column names a scale |

A file gets no Item when its name is not a dated `data_HHMM_` name, its footer has no Thing id or mobility, it has no rows,
a row has no UTC time, no row is located, a cell is not resolution 8, or a fixed Thing file holds more than one cell.
A null or zero `h3_cell_id` is not located.

## Thing Collection

| Rule | Level |
| --- | --- |
| `stac_extensions` lists Table v1.3.0, the OpenSensor extension and this profile | MUST |
| `license`, `keywords` and `instruments` are set | MUST |
| `opensensor:sample_interval_s` is set | MUST |
| `opensensor:mobility` is set, the same value as `thing.mobility` in the footer | MUST |
| `stac_extensions` lists the Partition extension v1.0.0 and the fields in [Partitions](#partitions) are set | MUST |
| `providers` holds the Thing's owner as `producer` and the data host as `host` | MUST |
| The owner's point of contact uses the [Contacts](https://github.com/stac-extensions/contacts) extension | MAY |
| `item_assets.data` is a Parquet asset with role `data`, `table:primary_datetime` and `table:columns` | MUST |
| `item_assets.data` sets `table:primary_geometry` to `geometry` when the files have that column | SHOULD |
| One `describedby` link of type `text/plain` points at the firmware dictionary with its `file:checksum` | MUST |
| A `describedby` link of type `application/json` points at the board template with its `file:checksum` | SHOULD |
| Columns equal the template's columns in template order, plus only `title` and `opensensor:scale` from the owner config | MUST |
| `opensensor:scales` holds every scale a column points at | MUST |
| `root`, `parent` and `child` links are relative | MUST |
| No `item` links. Items are found by the partition glob, by listing or through `items.parquet` | SHOULD |
| `extent.spatial` covers every published Item `bbox` | MUST |
| `extent.temporal` starts at the first published `start_datetime` and ends `null` while the Thing publishes | MUST |
| `extent` never shrinks | MUST |

## Columns

Every column has `name`, `type` and exactly one value in `roles`. The role decides what else is required.

| Role | Meaning | Required beyond name, type and roles |
| --- | --- | --- |
| `physicalMeasurement` | Environmental reading | `unit`, `opensensor:property`, `opensensor:procedure`, `opensensor:field_id`, `opensensor:aggregate`, `opensensor:valid_when` |
| `auxiliaryInformation` | Board health such as battery or board temperature | `opensensor:procedure`, `opensensor:field_id`, `opensensor:aggregate` |
| `qualityInformation` | Status or code column | `opensensor:procedure`, `opensensor:field_id`, `opensensor:aggregate` |
| `modelResult` | Derived column | `unit`, `opensensor:derived`, `opensensor:aggregate` |
| `referenceInformation` | Identity or bookkeeping | Nothing more |
| `coordinate` | Time or position | Nothing more |

## File Item

One Item describes one uploaded file. Its `id` is the file name without `.parquet`.
A file with no located row gets no Item. It is still uploaded with its footer, adds nothing to the Collection extent
and is found only by listing the partition.

| Rule | Level |
| --- | --- |
| `stac_extensions` lists Table, Anonymized Location v1.0.0, the OpenSensor extension and this profile | MUST |
| `stac_extensions` lists Grid v1.1.0 for a fixed Thing and leaves it out for a moving Thing | MUST |
| `collection` names the Thing | MUST |
| `opensensor:mobility` is set, the same value as in the Collection | MUST |
| A fixed Thing's geometry is its H3 cell polygon, and `grid:code` is `H3-` plus the 15 character cell | MUST |
| A moving Thing's geometry is the union of the H3 cells its rows visited, a Polygon or MultiPolygon, with no `grid:code` | MUST |
| Every cell is resolution 8 | MUST |
| Geometry is never a device position, a cell centre or a track | MUST |
| `bbox` is the bounding box of the geometry | MUST |
| `anon:size` is `0.01` and `anon:warning` is the text below | MUST |
| `start_datetime` and `end_datetime` are the first and last row times, `datetime` is `null` | MUST |
| `assets.data` is the Parquet file with role `data` and `table:row_count` | MUST |
| `assets.data` `href` is relative, `./<file>.parquet` | MUST |
| `root`, `parent` and `collection` links are relative | MUST |
| `assets.data` carries `file:size` and a SHA-256 multihash `file:checksum` of the uploaded bytes, with File v2.1.0 listed | MUST |
| `opensensor:sample_interval_s` is the footer `sample_interval_ms` ÷ 1000 | MUST |
| Measurement columns carry core `statistics` for the file, as in [Canonical form](#canonical-form) | MUST |

Geometry is built the same way by every writer, so a regenerated Item is equal to the board's.

1. Take the distinct non-null `h3_cell_id` values of the file as 15 character lowercase H3 strings.
2. Sort them in ascending order.
3. Call H3 `cellsToMultiPolygon` on that list with GeoJSON order, longitude first and closed rings.
4. One polygon becomes a GeoJSON Polygon, more become a MultiPolygon. Coordinates are kept as H3 returns them.

A fixed Thing follows the same steps with its one cell. `bbox` is the minimum and maximum of every coordinate.

`anon:size` is twice the mean H3 edge length at resolution 8 in degrees at 111.32 km a degree.
That is 2 × 0.5314 km ÷ 111.32 km = 0.00955, rounded up to `0.01`. It is the same for fixed and moving Things.

| Mobility | `anon:warning` |
| --- | --- |
| `fixed` | `The geometry is the H3 cell chosen by the owner, not the device position.` |
| `moving` | `The geometry is the H3 cells the device visited, not its track.` |

`statistics` holds `minimum`, `maximum` and `mean` of the counted rows, and `valid_percent`, the counted rows over all rows.
A row counts when its value is finite and passes `opensensor:valid_when`. [Canonical form](#canonical-form) has the exact rules.

## Location

The rows hold the location. The footer `opensensor` key holds none.

| Column | Type | Meaning |
| --- | --- | --- |
| `h3_cell_id` | int64, nullable | H3 cell of the row |
| `h3_resolution` | int32, nullable | Resolution of that cell, 8 in published data |
| `location_source` | uint8, nullable | Where the cell came from, see below |
| `geometry` | Parquet GEOMETRY, nullable | WKB Point at the cell centre, longitude and latitude in OGC:CRS84, no `crs` member |

A row with no location has null in all four columns. Never a 0 and never a partial set.

| `location_source` | Meaning |
| --- | --- |
| 0 | Reserved and never written. Kept in `classification:classes` as `nodata` |
| 1 | Configured by the owner, the usual case for a fixed Thing |
| 2 | The device's own GNSS fix |
| 3 | A fix sent by the phone app |

Every published Thing uses resolution 8, fixed or moving. A cell is about 0.74 km² with an edge of about 530 m.
`h3_resolution` is the resolution of the row's cell, so it is 8 in every published row. A publisher never coarsens.

In `table:columns`, `h3_cell_id` is type `int64` and `h3_resolution` type `int32`, both role `referenceInformation`.
`location_source` is type `uint8`, role `qualityInformation`, aggregate `mode`.
Its codes go in `classification:classes` like every other code column.
`geometry` is type `geometry`, role `coordinate`, with no `unit`, and the data asset sets `table:primary_geometry` to `geometry`.

A fixed Thing writes its configured cell in every row with `location_source` 1.
When the owner moves a fixed Thing to a new cell, the firmware starts a new file and mobility stays `fixed`.
The Collection extent then covers every published cell.
Rows of the old cell stay in the old files unless the owner deletes those files.

A moving Thing writes the cell of each fix, so one file can hold many cells.
Its Item `bbox` is the box of the cell union, and the Collection extent is the union of the Item boxes.

The geometry is always the cell centre, never the raw fix, so the resolution sets how much a row reveals.
Parquet geospatial statistics on `geometry` give each row group a bounding box, and readers take the file box from them.
A file with a `geometry` column MUST also carry the standard GeoParquet 2.0.0 `geo` key, which belongs to GeoParquet, not to this profile.
It holds `version` `"2.0.0"` and `primary_column` `geometry`.
For that column it holds `encoding` `WKB`, `geometry_types` `["Point"]` and the file `bbox`, with no `crs`.
That `bbox` is over the cell centres of the located rows. A file with no located row has no `bbox`.
Without the `geo` key geopandas refuses the file. GDAL, DuckDB and pyarrow with geoarrow read the native type either way.
Points are 2D. GDAL reports a Point Z layer as an unknown type, so a height goes in its own column.
Moving Features trajectories are out of scope for now.

## Conventions

These choices hold for every Thing.

| Topic | Choice |
| --- | --- |
| Units | UCUM codes in core `unit`, for example `ug/m3`, `Cel`, `%`, `[g]` and `{particles}/dL` |
| Firmware unit labels | Kept in the template as `firmware.unit_labels` when they differ from the UCUM code |
| Roles | ACDD `coverage_content_type` values, one per column |
| Aggregation | CF `cell_methods` words, `mean` for readings, `point` for levels such as battery, `mode` for codes |
| Code meanings | `classification:classes` on the column, `nodata` on the missing value |
| CF names | `cf:standard_name` only where the CF definition matches exactly, never on CF=1 particle columns |
| Property URIs | Board neutral. A NERC P07 term first, then `http://vocab.nerc.ac.uk/standard_name/<cf name>/` for an exact CF match, else `urn:walkthru-earth:property:<quantity>`. Board and sensor go in `opensensor:procedure`, never in the property |
| Column ids | `opensensor:field_id` is the 1-based FIELD order, append only. The Thing release is a fresh baseline, every board renumbered its ids once there, and the append only rule holds from that baseline on |
| Time | `event_time_utc_us`, int64 microseconds since the Unix epoch, as `table:primary_datetime` |
| Location | H3 cell per row, resolution 8 for fixed and moving Things alike, see [Location](#location) |
| Health scales | WHO 2021 guideline and interim targets, labels in English and Arabic, `color_hint` per step |
| Checksums | SHA-256 multihash in `file:checksum`, the `1220` prefix then 64 hex characters |
| License | CC BY 4.0 |
| Firmware facts | LIVE keys, protocol floor and unit labels stay in the template under `firmware` |
| App choices | Card order, hero card and display step stay in the app profile |

## OGC SensorThings mapping

The profile reuses the SensorThings words so a SensorThings client maps it without guessing.

| SensorThings entity | OpenSensor |
| --- | --- |
| Thing | One Thing Collection, `thing=<uuid>/`, footer `thing.id` |
| Datastream | One measurement column of the Thing, its `table:columns` entry with `opensensor:field_id` and `unit` |
| ObservedProperty | `opensensor:property` of the column |
| Sensor | `opensensor:procedure` of the column, listed in `instruments` |
| Observation | One value in one row. `phenomenonTime` is `event_time_utc_us`, `result` is the value |
| Location | The H3 cell. For a fixed Thing the Item `grid:code`, for a moving Thing the row `h3_cell_id` |
| HistoricalLocation | The rows over time. Each row carries the cell it was taken in |
| FeatureOfInterest | The air in the H3 cell of the row |

## Parquet footer

Every data file carries one key of ours, `opensensor`, in its Parquet key-value metadata, modelled on the GeoParquet `geo` key.
The standard `geo` key sits beside it, see [Location](#location).
The value is compact UTF-8 JSON that validates against [json-schema/footer.json](https://github.com/walkthru-earth/stac-opensensor/blob/83787768cc8451109d48a03d4ef5493ffcd403dd/json-schema/footer.json).
No other key of ours is part of the contract.
A file stands alone, so a writer can build its Thing Collection and Item from the footer, the rows and the template it names.

| Field | Type | Description |
| --- | --- | --- |
| version | string | **REQUIRED**. The shared version, `0.1.` then the patch, such as `"0.1.0"`, see [Versioning](#versioning) |
| thing | object | **REQUIRED**. `id` (the Thing UUID), `device_id`, `board` (the slug used in template paths), `mobility` (`fixed` or `moving`), and optional `deployment_id` |
| firmware | object | **REQUIRED**. `version`, `schema` and `boot_id` (32 hex characters, the same token as in the file name) |
| dictionary | object | **REQUIRED**. `sha256` equals `firmware.dict` in the template, plus optional `version` and `uri` |
| template | object | **REQUIRED**. `href`, an immutable URL, and `sha256`, the SHA-256 of the exact template bytes, equal to the template link `file:checksum` without `1220` |
| acquisition | object | **REQUIRED**. `sample_interval_ms` and `time_semantics`, plus optional `config_id`, `config` and `calibration_id` |
| file | object | **REQUIRED**. `rotation_interval_s`, one of 600, 900, 1800 or 3600, `row_groups`, `row_group_rows_max`, `compression` and `purpose` |

Three version strings sit in a footer and each means something different.

| Field | Changes when |
| --- | --- |
| `firmware.version` | Any firmware release, such as `idf-cores3-parquet-v6.23` |
| `firmware.schema` | The column layout changes. It equals the template `id`, such as `cores3-telemetry-v6` |
| `dictionary.version` | The dictionary text changes. Only a label, the digest `dictionary.sha256` identifies it |

`purpose` is `telemetry` for a normal file. `codec-comparison-duplicate-rows` marks a benchmark copy that repeats rows and is never published.
`time_semantics` is free text from the firmware. It says which instant `event_time_utc_us` estimates. Readers show it and never parse it.
`compression` is the upper case Parquet codec, `LZ4_RAW` or `UNCOMPRESSED` on today's boards.
`acquisition.config` is the acquisition settings as compact JSON text. `deployment_id` and `calibration_id` are `unknown` until the owner sets them.
Every footer object stays open. Readers ignore members they do not know.

Numbers are JSON numbers. The footer `sample_interval_ms` is the truth for that file, and the template value is only the default.
The Item takes its interval from the footer and the Collection from the template, so an owner who changes the interval sees both.
`npm run check-profile` warns when they differ, and never fails.
Code meanings, such as clock quality codes, belong in the template, not in the footer.
DuckDB reads it with `SELECT value::JSON FROM parquet_kv_metadata('file.parquet') WHERE key = 'opensensor'`.
Examples [footers/m5stack-cores3.json](footers/m5stack-cores3.json) and
[footers/waveshare-esp32-s3-sim7670g.json](https://github.com/walkthru-earth/stac-opensensor/blob/83787768cc8451109d48a03d4ef5493ffcd403dd/footers/waveshare-esp32-s3-sim7670g.json).
Firmware and app docs cite this profile by its full 40 character commit.

## Board template

| Field | Type | Description |
| --- | --- | --- |
| type | `"OpenSensorTemplate"` | **REQUIRED** |
| template_version | string | **REQUIRED**. `0.1.` then the patch, such as `"0.1.0"` |
| id | string | **REQUIRED**. Firmware schema name, such as `cores3-telemetry-v6` |
| title | string | Board and sensor names |
| keywords | \[string] | Search terms, copied to the Collection |
| instruments | \[string] | **REQUIRED**. Procedures, copied to the Collection |
| opensensor:sample_interval_s | number | **REQUIRED**. Seconds between samples |
| item_assets | object | **REQUIRED**. `data` with columns as in a Collection, without `title` or `opensensor:scale` |
| links | \[Link] | **REQUIRED**. `describedby` to the firmware dictionary with `file:checksum` |
| firmware | object | **REQUIRED**. See below |

| Firmware field | Type | Description |
| --- | --- | --- |
| schema | string | **REQUIRED**. Firmware schema name |
| proto_min | integer | **REQUIRED**. Lowest sync protocol the board speaks |
| dict | string | **REQUIRED**. SHA-256 of `telemetry_fields.inc`, as the device sends it |
| live_keys | Map\<string, string> | **REQUIRED**. Column name to LIVE JSON key |
| unit_labels | Map\<string, string> | Column name to firmware unit label, where it differs from `unit` |

## Full example

The examples describe two real boards with the templates their firmware generates, copied byte for byte.
One is a fixed M5Stack CoreS3 in Accra, the other a moving Waveshare ESP32-S3 SIM7670G in Cairo.

| File | Layer |
| --- | --- |
| [templates/m5stack-cores3/3b4d8500….json](https://github.com/walkthru-earth/stac-opensensor/blob/83787768cc8451109d48a03d4ef5493ffcd403dd/templates/m5stack-cores3/3b4d8500a19f42c6743b0f7d725276c96ca14a0c249f613048603afbc3cfaff9.json) | CoreS3 template, `cores3-telemetry-v6`, 81 columns |
| [templates/waveshare-esp32-s3-sim7670g/a8605aaf….json](https://github.com/walkthru-earth/stac-opensensor/blob/83787768cc8451109d48a03d4ef5493ffcd403dd/templates/waveshare-esp32-s3-sim7670g/a8605aafb6f76b794e9e7fb32927be6d203afeeb14527206c0e49c1eb0436809.json) | Waveshare template, `waveshare-sim7670g-telemetry-v6`, 62 columns |
| [examples/catalog.json](https://github.com/walkthru-earth/stac-opensensor/blob/83787768cc8451109d48a03d4ef5493ffcd403dd/examples/catalog.json) | Root Catalog |
| [examples/thing=3f2b9c1e…/collection.json](https://github.com/walkthru-earth/stac-opensensor/blob/83787768cc8451109d48a03d4ef5493ffcd403dd/examples/thing=3f2b9c1e-6a7d-4e58-9b1c-2d4f6a8e0c13/collection.json) | Fixed Thing Collection |
| [examples/thing=3f2b9c1e…/…/data_1300_….json](https://github.com/walkthru-earth/stac-opensensor/blob/83787768cc8451109d48a03d4ef5493ffcd403dd/examples/thing=3f2b9c1e-6a7d-4e58-9b1c-2d4f6a8e0c13/year=2026/month=10/day=24/data_1300_0f3a9c2e5b7d41e8a6c3f0b2d9e4a713_2881-2970-0.json) | Fixed File Item, one 900 s window |
| [examples/thing=7c1e4b9a…/collection.json](https://github.com/walkthru-earth/stac-opensensor/blob/83787768cc8451109d48a03d4ef5493ffcd403dd/examples/thing=7c1e4b9a-3d52-4f86-a0b7-e9d2c5f81a46/collection.json) | Moving Thing Collection |
| [examples/thing=7c1e4b9a…/…/data_0815_….json](https://github.com/walkthru-earth/stac-opensensor/blob/83787768cc8451109d48a03d4ef5493ffcd403dd/examples/thing=7c1e4b9a-3d52-4f86-a0b7-e9d2c5f81a46/year=2026/month=10/day=24/data_0815_5e9d0c7b31a84f62b8d47a10c3e6f295_412-487-0.json) | Moving File Item, three cells, first row 08:17:20 in the 08:15 window |
| [footers/m5stack-cores3.json](footers/m5stack-cores3.json) | CoreS3 footer of the fixed Item's file |
| [footers/waveshare-esp32-s3-sim7670g.json](https://github.com/walkthru-earth/stac-opensensor/blob/83787768cc8451109d48a03d4ef5493ffcd403dd/footers/waveshare-esp32-s3-sim7670g.json) | Waveshare footer of the moving Item's file |
| [footers/m5stack-cores3.geo.json](https://github.com/walkthru-earth/stac-opensensor/blob/83787768cc8451109d48a03d4ef5493ffcd403dd/footers/m5stack-cores3.geo.json) and [footers/waveshare-esp32-s3-sim7670g.geo.json](https://github.com/walkthru-earth/stac-opensensor/blob/83787768cc8451109d48a03d4ef5493ffcd403dd/footers/waveshare-esp32-s3-sim7670g.geo.json) | GeoParquet `geo` keys of the same files |
| [fixtures/](https://github.com/walkthru-earth/stac-opensensor/tree/83787768cc8451109d48a03d4ef5493ffcd403dd/fixtures) | The rows of each example file and each Thing's owner config, the input of the rebuild |

`npm run check-profile` validates all of them.
It rebuilds each Item and Collection with [scripts/canonical.mjs](https://github.com/walkthru-earth/stac-opensensor/blob/83787768cc8451109d48a03d4ef5493ffcd403dd/scripts/canonical.mjs) and compares them with the examples.
It checks the file name, HHMM and partition against the rotation window, the cells and the location columns of the rows,
the Catalog child links, each footer and `geo` key against its template, Collection and rows,
and that broken documents fail, such as a resolution 7 cell, an absolute `href`, a closed temporal interval or a footer of another template.
