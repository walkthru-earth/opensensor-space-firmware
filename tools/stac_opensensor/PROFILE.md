# OpenSensor Thing Profile

| Item | Value |
| --- | --- |
| Title | OpenSensor Thing Profile |
| Profile schema | <https://walkthru-earth.github.io/stac-opensensor/v0.1.0/profile.json> |
| Template schema | <https://walkthru-earth.github.io/stac-opensensor/v0.1.0/template.json> |
| Version | 0.1.0 |

The [extension](https://github.com/walkthru-earth/stac-opensensor/blob/4408c4e52366bb3d154d22bbfffc88ff09aed6f1/README.md) defines fields. This profile fixes how a whole catalog of Things uses them,
so any client can read any Thing without guessing.
A Thing is the OGC SensorThings term for a device that carries sensors. It is fixed in one place or moving.
It says which extensions each document must declare, which fields each column must carry and which vocabularies to use.

A document follows the profile when it lists the profile schema in `stac_extensions`.
STAC validators then check the profile like any other extension.

- [Full example](#full-example)
- [Profile schema](https://github.com/walkthru-earth/stac-opensensor/blob/4408c4e52366bb3d154d22bbfffc88ff09aed6f1/json-schema/profile.json)
- [Template schema](https://github.com/walkthru-earth/stac-opensensor/blob/4408c4e52366bb3d154d22bbfffc88ff09aed6f1/json-schema/template.json)

## Layers

One Thing is described by three layers. Each layer has one owner.

| Layer | Owner | File | Holds |
| --- | --- | --- | --- |
| Board template | Firmware build | `templates/<board>/<dict>.json` | Columns and their meaning, sample interval, instruments, LIVE keys, protocol floor |
| Thing Collection | Catalog publisher | `thing=<uuid>/collection.json` | Template columns plus titles, scales, mobility, location extent, license and links |
| App profile | Mobile app | Outside this repository | Card order, hero card, display step and column labels in each language |

The board template is plain JSON, not STAC. A firmware build generates it, its SHA-256 is the descriptor digest,
and a device can serve it over BLE or LAN. A Thing Collection is the template's columns with facts about the Thing added.
The app profile is the app's own choice and never goes into published STAC.

## Catalog layout

```text
catalog.json                                          STAC Catalog, one child link per Thing
thing=<uuid>/collection.json                          Thing Collection, columns in item_assets
thing=<uuid>/year=YYYY/month=MM/day=DD/<file>.parquet one uploaded file, at most 1 hour
thing=<uuid>/year=YYYY/month=MM/day=DD/<file>.json    its Item, same name
templates/<board>/<dict>.json                         board template, cached forever by digest
```

`<file>` is the board's own name, `data_HHMM_<boot>_<first>-<last>-<attempt>`, where `HHMM` is the start of the UTC window.
`<board>` is the board slug, such as `m5stack-cores3`.
`<dict>` is the 64 hex `firmware.dict`, because one schema can ship with several dictionaries.
Only dated `data_HHMM_` files are published. Unsynced, benchmark and other board paths stay on the device.
A board closes a file at the end of its rotation window and uploads it at once, so the newest data is at most 1 hour old.
Two files may share a window after a reboot or a retried upload. Readers order rows by time and drop repeats of the same Thing and time.
Files written before the board knew UTC time get no Item until they are reconciled.
Day or longer roll-ups are derived products under a separate prefix, never inside `thing=<uuid>/`.

## Thing Collection

| Rule | Level |
| --- | --- |
| `stac_extensions` lists Table v1.3.0, the OpenSensor extension and this profile | MUST |
| `license`, `keywords` and `instruments` are set | MUST |
| `opensensor:sample_interval_s` is set | MUST |
| `opensensor:mobility` is set, the same value as `thing.mobility` in the footer | MUST |
| `item_assets.data` is a Parquet asset with role `data`, `table:primary_datetime` and `table:columns` | MUST |
| `item_assets.data` sets `table:primary_geometry` to `geometry` when the files have that column | SHOULD |
| A `describedby` link points at the firmware dictionary with its `file:checksum` | MUST |
| A second `describedby` link points at the board template with its `file:checksum` | SHOULD |
| Columns equal the template's columns, plus only `title` and `opensensor:scale` | SHOULD |
| `opensensor:scales` holds every scale a column points at | MUST |

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

| Rule | Level |
| --- | --- |
| `stac_extensions` lists Table, Grid v1.1.0, Anonymized Location v1.0.0, the OpenSensor extension and this profile | MUST |
| `collection` names the Thing | MUST |
| `opensensor:mobility` is set, the same value as in the Collection | MUST |
| A fixed Thing's geometry is its H3 cell polygon, and `grid:code` is `H3-` plus the 15 character cell | MUST |
| A moving Thing's geometry is the union of the H3 cells its rows visited, a Polygon or MultiPolygon | MUST |
| Geometry is never a device position, a cell centre or a track | MUST |
| `anon:size` and `anon:warning` are set | MUST |
| `start_datetime` and `end_datetime` are the first and last row times, `datetime` is `null` | MUST |
| `assets.data` is the Parquet file with `table:row_count` | MUST |
| Measurement columns carry core `statistics` for the file | SHOULD |

## Location

The rows hold the location. The footer `opensensor` key holds none.

| Column | Type | Meaning |
| --- | --- | --- |
| `h3_cell_id` | int64, nullable | H3 cell of the row, null when there is no location |
| `h3_resolution` | int32, nullable | Resolution of that cell, 8 by default, never finer than the owner's configured maximum |
| `location_source` | uint8, nullable | Where the cell came from, see below |
| `geometry` | Parquet GEOMETRY, nullable | WKB Point at the cell centre, longitude and latitude in OGC:CRS84, no `crs` member |

| `location_source` | Meaning |
| --- | --- |
| 0 | None |
| 1 | Configured by the owner, the usual case for a fixed Thing |
| 2 | The device's own GNSS fix |
| 3 | A fix sent by the phone app |

In `table:columns`, `h3_cell_id` is type `int64` and `h3_resolution` type `int32`, both role `referenceInformation`.
`location_source` is type `uint8`, role `qualityInformation`, aggregate `mode`.
Its codes go in `classification:classes` like every other code column.
`geometry` is type `geometry`, role `coordinate`, with no `unit`, and the data asset sets `table:primary_geometry` to `geometry`.
A fixed Thing writes its configured cell in every row with `location_source` 1.
A moving Thing writes the cell of each fix, so one file can hold many cells.
The geometry is always the cell centre, never the raw fix, so the resolution sets how much a row reveals.
Parquet geospatial statistics on `geometry` give each row group a bounding box, and readers take the file box from them.
A file with a `geometry` column MUST also carry the standard GeoParquet 2.0.0 `geo` key, which belongs to GeoParquet, not to this profile.
It holds `version` `"2.0.0"` and `primary_column` `geometry`.
For that column it holds `encoding` `WKB`, `geometry_types` `["Point"]` and the file `bbox`, with no `crs`.
Without it geopandas refuses the file. GDAL, DuckDB and pyarrow with geoarrow read the native type either way.
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
| Column ids | `opensensor:field_id` is the 1-based FIELD order, append only |
| Time | `event_time_utc_us`, int64 microseconds since the Unix epoch, as `table:primary_datetime` |
| Location | H3 cell per row, resolution 8 for fixed and moving Things alike, see [Location](#location) |
| Health scales | WHO 2021 guideline and interim targets, labels in English and Arabic, `color_hint` per step |
| Checksums | SHA-256 multihash in `file:checksum`, the `1220` prefix then 64 hex characters |
| License | CC BY 4.0 |
| Firmware facts | LIVE keys, protocol floor and unit labels stay in the template under `firmware` |
| App choices | Card order, hero card and display step stay in the app profile |

## Parquet footer

Every data file carries one key of ours, `opensensor`, in its Parquet key-value metadata, modelled on the GeoParquet `geo` key.
The standard `geo` key sits beside it, see [Location](#location).
The value is compact UTF-8 JSON that validates against [json-schema/footer.json](https://github.com/walkthru-earth/stac-opensensor/blob/4408c4e52366bb3d154d22bbfffc88ff09aed6f1/json-schema/footer.json).
No other key of ours is part of the contract.
A file stands alone, so an indexer can build its Thing Collection and Item from the footer, the rows and the template it names.

| Field | Type | Description |
| --- | --- | --- |
| version | `"0.1.0"` | **REQUIRED**. Readers refuse an unknown major version |
| thing | object | **REQUIRED**. `id` (the Thing UUID), `device_id`, `board` (the slug used in template paths), `mobility` (`fixed` or `moving`), and optional `deployment_id` |
| firmware | object | **REQUIRED**. `version`, `schema` and `boot_id` (32 hex characters, the same token as in the file name) |
| dictionary | object | **REQUIRED**. `sha256` equals `firmware.dict` in the template, plus optional `version` and `uri` |
| template | object | **REQUIRED**. `href` and `sha256`, the SHA-256 of the exact template bytes, equal to the template link `file:checksum` without `1220` |
| acquisition | object | **REQUIRED**. `sample_interval_ms` and `time_semantics`, plus optional `config_id`, `config` and `calibration_id` |
| file | object | **REQUIRED**. `rotation_interval_s` at most 3600, `row_groups`, `row_group_rows_max`, `compression` and `purpose` |

Numbers are JSON numbers. The footer `sample_interval_ms` is the truth for that file, and the template value is only the default.
Code meanings, such as clock quality codes, belong in the template, not in the footer.
DuckDB reads it with `SELECT value::JSON FROM parquet_kv_metadata('file.parquet') WHERE key = 'opensensor'`.
Example [footers/cores3-telemetry-v5.json](footers/cores3-telemetry-v5.json).
Firmware and app docs cite this profile by its full 40 character commit.

## Board template

| Field | Type | Description |
| --- | --- | --- |
| type | `"OpenSensorTemplate"` | **REQUIRED** |
| template_version | `"0.1.0"` | **REQUIRED** |
| id | string | **REQUIRED**. Firmware schema name, such as `cores3-telemetry-v5` |
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

The examples describe a real M5Stack CoreS3 build, firmware schema `cores3-telemetry-v5`.
The template lists 11 of its 81 columns, one or more of each kind. A generated template lists them all.

| File | Layer |
| --- | --- |
| [templates/m5stack-cores3/e5060fb7….json](https://github.com/walkthru-earth/stac-opensensor/blob/4408c4e52366bb3d154d22bbfffc88ff09aed6f1/templates/m5stack-cores3/e5060fb7fedbcabf665b1c8a159a77f5fe9283ac34bd2d89f1bc98b0f027d38b.json) | Board template, `dict` `e5060fb7…` |
| [examples/catalog.json](https://github.com/walkthru-earth/stac-opensensor/blob/4408c4e52366bb3d154d22bbfffc88ff09aed6f1/examples/catalog.json) | Root Catalog |
| [examples/collection.json](https://github.com/walkthru-earth/stac-opensensor/blob/4408c4e52366bb3d154d22bbfffc88ff09aed6f1/examples/collection.json) | Thing Collection made from the template |
| [examples/item.json](https://github.com/walkthru-earth/stac-opensensor/blob/4408c4e52366bb3d154d22bbfffc88ff09aed6f1/examples/item.json) | File Item, one hour |

`npm run check-profile` validates all of them and checks that the Collection's columns and checksums match the template.
