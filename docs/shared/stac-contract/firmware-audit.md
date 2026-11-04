# STAC firmware audit

> Dated history, kept as written on 2026-10-25. File names such as `aq_logger.cpp` and `station_identity` were renamed later. Read [the decisions](../decisions/README.md) and the current code for today's layout.

Read-only audit of branch `yharby/stac-firmware-audit` at commit `2b49114` (2026-10-25). No firmware code was changed and no board was flashed. All paths are relative to the repository root.

The goal is a sensor agnostic mobile app (and later ecosystem) driven by a STAC JSON contract. Released firmware wire contracts (BLE frames, opcodes, UUIDs, JSON keys, Hive paths) stay backward compatible.

## Corrections after review

The findings below are kept as written on 2026-10-25. Later checks against the code at `2b49114` found these errors. Where a line below disagrees with this list, this list wins. Each item cites the matching entry in [firmware-audit-review.md](firmware-audit-review.md).

1. Per-column metadata does leave the device. The `parquet schema` serial command prints procedure, unit, validity and property for every field (`firmware/common/logger/src/aq_logger.cpp:2522-2534`). Synced Parquet gives the app column names, physical types, field ids and every footer entry. The real gap is units, labels and meanings for LIVE over BLE and LAN before any file sync. Review C2 and C15.
2. The footer reserves 64 entries, it does not use them. Real use is 37 on CoreS3 (22 fixed, 7 location, 3 anchor, 5 board) and 45 on Waveshare (13 board). Review C3.
3. Raising `kFooterMetadata` (`aq_logger.cpp:98`) alone does not add slots. `Writer::finish` rejects `metadata_count > 64` as a literal (`firmware/common/src/parquet_writer.cpp:593`). Pass new keys inside the board metadata budget or lower `kMaxConfigMetadata`, or change both limits together. Review C4 and P6.
4. An unknown dictionary digest falls back to a generic LIVE vocabulary in the app. Only a new LIVE key, a retyped key or a new reviewed temperature source needs an app release. Review C14.
5. `field_id` is a stable join key only for schema v5 files. Commit `e166978` added field ids and changed the unit of position 9 in the same change, so older files carry no field ids and must be mapped by schema name and column name. Review C10.
6. The fixture binary takes the argument `dictionary` (`tools/telemetry_contract_fixture.cpp:59`). `--dictionary-out` is an option of `tools/test_telemetry_contract.py:119`. Review C11.
7. A new code value is a LIVE layout change under the LIVE key stability rule (`docs/shared/ble-sync-protocol.md:210-214`). Code tables therefore belong on the `dict` side, not only in `desc`. Review E5.
8. A shared property URI would merge columns that differ by board. `t`, `imu_t` and `pct` mean different things on CoreS3 and Waveshare. Keep per board property URIs and add a separate comparability link. Review E6.
9. Streaming a 25 to 40 KB descriptor as `CONFIG_CHUNK` style notifications has no recovery for lost frames. Use a request per chunk read with offset and length, so the app can ask again for a lost range. Review P1 and C12.
10. CF `units` are UDUNITS-2 and `Cel` is a UCUM code, so one `unit` field cannot hold both. Pick one system per field. Review P3.
11. LIVE prints every finite float with `%.1f` (`firmware/common/logger/src/aq_logger_live.cpp:89`). A precision other than 1 can only be a display hint, it cannot describe the wire value. Review E3.

The released extension stac-opensensor v0.1.0 settles items 5, 8, 9 and 10 in its own way. See [README.md](README.md) for the name map.

## Summary

- Each board has one X-macro table, `telemetry_fields.inc`, that is the single source of column truth. It already carries name, physical type, procedure, unit, validity rule, property URI and LIVE key.
- That table is compiled into `.rodata` and copied into the Parquet schema, but only the column name, type and `field_id` reach the Parquet schema. The rest exists only in the source file and in the host fixture output. (Corrected, see item 1 above.)
- The phone receives none of the per-column metadata. It sees a dictionary SHA-256 in `info` and short LIVE keys, then maps them with hand-written per-digest profiles in the app. (Corrected, see item 1 above.)
- Units are project labels, not UCUM. There are no CF names, no labels in any language, no valid ranges, no display scales, no aggregation hints.
- The Parquet footer key-value array is full (64 of 64 entries in the worst case) and each value is capped at 4096 bytes, so a full STAC document cannot ride in the footer today. (Corrected, see items 2 and 3 above.)
- The cheapest robust path is a build-time STAC generator fed by the existing host fixture, a new sibling semantics table with its own digest, a read-only BLE and LAN opcode that streams the descriptor from flash, and a compact href plus SHA-256 in the Parquet footer.
- The `esp-idf-n16r8` board does not exist in this worktree. See Q1.

## Q1. Per-board schema locations and per-column metadata

### Where the schemas live

| Board | Field table | Contract header | Digest header | Board KV metadata |
|---|---|---|---|---|
| M5Stack CoreS3 | `firmware/esp-idf-cores3/bringup/telemetry_fields.inc` (81 FIELDs, lines 9 to 91) | `firmware/esp-idf-cores3/bringup/telemetry_contract.h` | `firmware/esp-idf-cores3/bringup/telemetry_dictionary_digest.h:8-9` | `firmware/esp-idf-cores3/bringup/telemetry_logger.cpp:141-149` |
| Waveshare ESP32-S3-SIM7670G | `firmware/esp-idf-waveshare-sim7670g/logger/telemetry_fields.inc` (62 FIELDs, lines 13 to 74) | `firmware/esp-idf-waveshare-sim7670g/logger/telemetry_contract.h` | `firmware/esp-idf-waveshare-sim7670g/logger/telemetry_dictionary_digest.h:6-7` | `firmware/esp-idf-waveshare-sim7670g/logger/main.cpp:116-148` |
| n16r8 | none | none | none | none |
| common | `firmware/common/logger/src/aq_logger.h:13-23` (`struct Field`) | n/a | n/a | n/a |

The n16r8 board was never tracked in git and has no PRs or issues. The main checkout at `firmware/esp-idf-n16r8` holds only untracked, empty leftovers (`artifacts/2026-10-15-diagnostic`, `artifacts/raw-diagnostic`, `artifacts/wiring`, `build/diagnostic`, `components/aq_n16r8_diagnostic`, `diagnostic/`). PR #68 (LoRa solar station board options) mentions it. There is no schema to audit.

`firmware/common/sensors` holds drivers only (`pms_frame.h:9` `enum class Model {Pmsa003, Pms5003t}`, `aq_ltr553.h`, `aq_max17048.h`). Its AGENTS.md says drivers return raw words plus validity and hold no board policy, so it defines no columns.

### The FIELD macro

`FIELD(name, physical_type, procedure, unit, validity, live_key)` is expanded in each contract header.

- CoreS3 builds `struct Definition {name, type, procedure, unit, validity, property_uri, live_key}` at `telemetry_contract.h:46-54`, fills `kFields` at lines 61-72 and asserts 81 fields at line 73.
- Waveshare reuses `aqlogger::Field` directly (`telemetry_contract.h:62`), fills `kFields` at lines 69-80 and asserts 62 at line 81.
- The property URI is built by the macro as `urn:walkthru-earth:cores3:property:<name>` (CoreS3 `.inc` header comment) or `urn:walkthru-earth:waveshare-sim7670g:property:<name>` (Waveshare `.inc:7`). Resolved in #99, the property now comes from each board's `telemetry_semantics.inc` as a board neutral id.
- `field_id` is the 1-based position (`prepare_columns`, CoreS3 `telemetry_contract.h:88-96`, common `aq_logger.cpp:368-382`). Append-only per Iceberg v3 rules.
- A unit of `us_since_unix_epoch` switches the logical type to `TimestampMicrosUtc` (CoreS3 `telemetry_contract.h:81-86`).

### Per-column metadata matrix

| Attribute | In source table | In Parquet schema | In Parquet KV | Sent to phone |
|---|---|---|---|---|
| name | yes | yes | no | no (only LIVE key) |
| physical type | yes | yes | no | no |
| logical type (UTC micros) | derived from unit | yes | no | no |
| field_id | derived from order | yes (`parquet_writer.cpp:355-365`) | no | no |
| procedure | yes | no | no | no |
| unit | yes (project label) | no | no | no |
| validity rule | yes | no (only nullability) | no | no |
| property URI | yes (URN) | no | no | no |
| LIVE key | yes (optional) | no | no | yes, as JSON key |
| min and max per row group | runtime | yes (statistics, `parquet_writer.cpp:233`, `:397`) | no | no |
| status code meanings | no | no | yes, free text per board | no |
| sensor make and model | no | no | Waveshare only (`main.cpp:116-148`, `sensor_model`) | Waveshare config page 0 |
| label, description | no | no | no | no |
| valid range, display scale | no | no | no | no |
| aggregation method | no | no | no | no |
| calibration | no | no | `calibration_id` always "unknown" | no |
| sample interval | no | no | `sample_interval_ms` hardcoded "10000" | no |

The Parquet `SchemaElement` writer (`firmware/common/src/parquet_writer.cpp:355-365`) writes type, repetition, name, `field_id` and the timestamp logical type only. No description or unit leaves the device inside the schema.

### Units in use

CoreS3 and Waveshare use the same project label set. Examples are `ug/m3`, `count/0.1L`, `degC`, `%`, `mV`, `g`, `deg/s`, `raw_count`, `us_since_unix_epoch`, `code`, `bitmask`, `boolean` and `1e-7_degree_latitude`. `docs/shared/table-and-observation-model.md:183` states they are project labels, not UCUM.

### Notable rows

- CoreS3 `t` and `rh` (`telemetry_fields.inc:79-80`) are always null. Validity is `unavailable` and procedure is `sht20_unresolved` because of the SHT20 address collision.
- CoreS3 LIVE `t` therefore never appears. The app falls back to `imu_t` (board temperature, `.inc:48`).
- Waveshare `t` and `rh` (`.inc:34-35`) come from the PMS5003T and are valid.
- H3 location columns are CoreS3 `.inc:88-91` and Waveshare `.inc:63-66`.
- Waveshare battery v5 columns are `.inc:69-74`, including `bstate`.

## Q2. Dictionary, LIVE keys, digest and phone-side JSON

### Dictionary identity

| Constant | CoreS3 | Waveshare |
|---|---|---|
| Schema version | `kSchemaVersion = 5` (`telemetry_contract.h:18`) | 5 (`:19`) |
| Schema name | `cores3-telemetry-v5` (`:19`) | `waveshare-sim7670g-telemetry-v5` (`:20`) |
| Firmware | `idf-cores3-parquet-v6.13` (`:21`) | `idf-waveshare-parquet-v2.7` (`:22`) |
| Dictionary URI | GitHub blob on `main` (`:22-24`) | GitHub blob on `main` (`:24-26`) |
| Acquisition config id | `cores3-acquisition-v3` (`:26`) | `waveshare-sim7670g-acquisition-v3` (`:27`) |
| Dictionary SHA-256 | `e5060fb7…027d38b` (`telemetry_dictionary_digest.h:8-9`) | `67688d0f…5c7183ff` (`telemetry_dictionary_digest.h:6-7`) |

The digest is the SHA-256 of the raw bytes of `telemetry_fields.inc`, comments included. Any edit to that file, even whitespace, changes the digest and breaks the app's per-digest profile lookup. The dictionary URI points at mutable `main`, so it does not pin the bytes that produced the digest.

### Exact JSON shapes the phone receives

`info` (GATT read and LAN `HELLO`, `firmware/common/connectivity/src/ble_sync.cpp:1059-1066`, buffer `char info_text[400]` at line 56, documented at `docs/shared/ble-sync-protocol.md:131-143`).

```json
{"proto":3,"fw":"idf-cores3-parquet-v6.13","schema":"cores3-telemetry-v5","cols":81,
 "dict":"<64 hex>","station":"<uuid>","dev":"<12 hex>","boot":"<32 hex>","max_read":16384}
```

A Waveshare `info` is about 300 bytes, leaving about 100 bytes in the 400-byte buffer.

`status` is documented at `ble-sync-protocol.md:145-177`.

`live` is built by `firmware/common/logger/src/aq_logger_live.cpp:38-109` and documented at `ble-sync-protocol.md:179-214`. It is flat, capped at 480 bytes, and omits invalid values. Keys follow `[a-z0-9_]{1,15}` (`aq_logger.h:13-23`, validated at `aq_logger_live.cpp:11-36`). UTC is scaled from microseconds to nanoseconds (lines 66-75). Floats are printed with `%.1f` (line 89), so precision is fixed at one decimal for every float column.

```json
{"seq":1234,"mono":123456789,"utc":1761400000000000000,"pms":4,
 "c1":3,"c25":5,"c10":6,"pm1":3,"pm25":5,"pm10":6,
 "n03":510,"n05":150,"n1":30,"n25":2,"n5":0,"n10":0,
 "imu_t":38.2,"als":120,"vbus":5012,"bat":4110,"pct":92,"chg":1}
```

LIVE keys per board.

| Key | CoreS3 line | Waveshare line |
|---|---|---|
| `seq`, `mono`, `utc`, `pms` | 13, 14, 17, 19 | 13, 14, 17, 19 |
| `c1`, `c25`, `c10`, `pm1`, `pm25`, `pm10` | 21-26 | 21-26 |
| `n03`, `n05`, `n1`, `n25` | 27-30 | 27-30 |
| `n5`, `n10` | 31-32 | none |
| `imu_t`, `als` | 48, 49 | none |
| `t`, `rh` | 79, 80 (always null) | 34, 35 |
| `vbus`, `bat`, `pct`, `chg` | 54, 55, 56, 57 | `bat` 42, `pct` 43 |
| `bstate` | none | 73 |

The key sets are frozen per digest in `tools/fixtures/live_keys/cores3.json` and `tools/fixtures/live_keys/waveshare.json`.

Config pages (`ble-sync-protocol.md:289-337`). Page 0 is sensor `{vendor, model, serial, batch_candidate}`. Page 1 is location `{cell, resolution, cell_resolution, lat, lon, country}`. Waveshare fills page 0 from `pms5003t_identity.h` (vendor `Plantower` line 12, model `PMS5003T` line 13).

### How the app interprets them

`opensensor-space-mobile/core/protocol/.../LiveProfile.kt` keys a hand-written `LiveProfile(temperature, humidity, keys, temperatureKey, reviewed)` by `info.dict` (CoreS3 v5 digest at line 52). `TemperatureSource` is `BOARD("imu_t")` or `AMBIENT("t")`. An unknown digest falls back to a generic vocabulary. Every new board or column needs an app release today. (Corrected, see item 4 above.)

## Q3. Parquet key-value metadata and Hive layout

### Footer KV entries

Superseded by #88. Files now carry one `opensensor` key with versioned JSON, see `mobile-migration.md`. The text below describes the old layout.

Limits are in `firmware/common/logger/src/aq_logger.cpp:93-102`. There are 22 fixed entries, 7 location entries, 3 anchor entries and up to 32 board entries, against `kFooterMetadata = 64`. The worst case uses every slot. (Corrected, see items 2 and 3 above.) `parquet_writer.cpp:598-599` caps keys at 255 bytes and values at 4096 bytes. `created_by` is capped at 128 bytes (line 595).

Fixed entries (`aq_logger.cpp:851-878`) in order.

| Key | Value source |
|---|---|
| `schema_version` | schema name, for example `cores3-telemetry-v5` |
| `device_id` | 12 hex from MAC |
| `station_id` | NVS UUIDv4 (`aq_logger.cpp:384-420`) |
| `boot_id` | 32 hex per boot |
| `firmware` | `kFirmware` |
| `dictionary_version` | `kDictionaryVersion` |
| `dictionary_uri` | GitHub blob on `main` |
| `dictionary_sha256` | digest of `.inc` |
| `acquisition_config_id`, `acquisition_config` | `kConfigurationId`, `kConfiguration` JSON |
| `deployment_id`, `calibration_id` | always `unknown` |
| `time_semantics` | `kTimeSemantics` |
| `rotation_interval_s`, `row_groups`, `row_group_rows_max` (`90`) | runtime |
| `compression`, `purpose`, `board` | runtime and config |
| `sample_interval_ms` | hardcoded `10000` |
| `clock`, `durability` | runtime |

Then come 7 location entries (`firmware/common/logger/src/aq_logger_location.h:43-63`), which are `country_iso3166_1_alpha2`, `country_source`, `location_source`, `h3_cell_id`, `h3_resolution`, `h3_max_resolution` and `h3_center_units`. Then `reconciliation_anchor_*` entries and then board metadata (`aq_logger.cpp:881-895`).

Board metadata. CoreS3 (`telemetry_logger.cpp:141-149`) writes `pms_status`, `light_status`, `proximity_status`, `magnetic_units` and `unavailable`. Waveshare (`main.cpp:116-148`) writes `pms_status`, `gauge_status`, `unavailable`, `battery_health_state`, `gauge_status_flags`, `battery_minutes_to_empty`, `battery_presence`, `sensor_model`, `pm25_batch_candidate_formula`, `pm25_batch_candidate_validation`, `pm25_batch_candidate_status`, `hardware_revision` and `storage_bus`. These are free-text code tables, the closest thing to a display vocabulary today.

### Hive layout

Built at `aq_logger.cpp:690-710` and documented at `docs/shared/telemetry-pipeline.md:154-164`.

```text
output/station=<UUID>/year=YYYY/month=MM/day=DD/data_HHMM_<boot>_<first>-<last>-<attempt>.parquet
output/station=<UUID>/unsynced/boot=<boot>/data_unsynced_<boot>_<first>-<last>-<attempt>.parquet
```

A bench mode writes under `boot=<boot>`. The legacy CoreS3 directory is `/sd/parquet` (`telemetry_logger.cpp:189`). Partitions are UTC. Unsynced files are never renamed. The phone reconciles them from anchors.

## Q4. table-and-observation-model.md decisions and STAC

`docs/shared/table-and-observation-model.md` decides on layered, complementary models.

- Lines 12-36. Parquet plus Hive is the raw immutable archive. The versioned dictionary carries meaning. SensorThings and Iceberg are later host-side work, not firmware work.
- Lines 65-75. Iceberg is static, with `field_id` preserved for schema evolution. Line 75 says `parquet_reader.h` derives Iceberg DataFile metrics, but no board calls it.
- Lines 77-107 map SensorThings. Thing is the station UUID. Sensor is the board plus procedure. Datastream is one column per station. ObservedProperty is the property URI. FeatureOfInterest is the H3 cell.
- Line 113. URIs are a local vocabulary, not OGC definitions.
- Line 114. "Descriptions are not repeated in every row or as a large JSON object in every tiny file." This constrains any plan to embed full STAC JSON in each Parquet footer.
- Line 115. Deployment and calibration are unknown.
- Lines 161-181 define validity tokens. Line 183 says units are not UCUM.
- Line 40 is stale. It says Waveshare v3 has 56 fields, but the current table has 62.

### How STAC fits

STAC is a discovery and catalog layer. It does not replace Iceberg (table format and snapshots) or SensorThings (observation API). The three can share one source of truth.

| Concept | STAC | Iceberg | SensorThings |
|---|---|---|---|
| Board model and schema | Collection (`table:columns`, `cube:variables`) | table schema | Sensor plus ObservedProperty |
| Station | Collection per station or `opensensor:station_id` on Items | partition value | Thing |
| One Parquet file | Item with one asset | DataFile | slice of Datastream |
| Column | `table:columns[]` entry | field with `field_id` | Datastream |
| H3 cell | Item geometry and `proj:` or `grid:code` | partition or column | FeatureOfInterest |
| Time window | Item `start_datetime`, `end_datetime` | partition and metrics | phenomenonTime |

The `field_id` already gives a stable join key between STAC columns, Iceberg fields and Parquet schema elements. (Corrected, true for schema v5 only, see item 5 above.)

## Q5. Gap analysis

| Need | Today | Gap | Where to add |
|---|---|---|---|
| UCUM units | project labels (`ug/m3`, `degC`, `count/0.1L`) | not machine-parseable | semantics table, map `ug/m3` to `ug/m3`, `degC` to `Cel`, `count/0.1L` to `{count}/dL` (see item 10 above) |
| CF standard names | none | no cross-domain meaning | `cf:standard_name`, for example `mass_concentration_of_pm2p5_ambient_aerosol_particles_in_air`, `air_temperature`, `relative_humidity` |
| Vocabulary URIs | local URN per board | no shared property across boards | shared `urn:opensensor:property:<id>` plus optional QUDT or CF URI (corrected, see item 8 above) |
| English and Arabic labels | none | app hardcodes strings | `title`, `title_ar` in descriptor, or `opensensor:labels {en, ar}` |
| Aggregation | snapshot only (`kConfiguration` says latest-available-not-average) | not per column | `opensensor:aggregation` (`instant`, `mean`, `counter`, `state`) |
| Valid range | none | app cannot flag outliers | `opensensor:valid_range [min, max]` |
| Health or display scales (AQI bands) | none | app hardcodes bands | `opensensor:display` extension with scale id and thresholds |
| Status code tables | free text in board KV | not structured | `classification:classes` per code column |
| Make and model | Waveshare KV and config page 0, CoreS3 none | not per column | `opensensor:sensor {vendor, model}` per column or per procedure |
| Calibration | `calibration_id` always `unknown` | none | `opensensor:calibration` with id and date, filled later |
| Sample interval | hardcoded `"10000"` KV and config JSON | not tied to real cadence | descriptor field and live value from config |
| LIVE precision | `%.1f` for all floats | g and deg/s lose detail | `opensensor:precision` in descriptor, firmware keeps format (display only, see item 11 above) |
| Dictionary URI | GitHub `main` blob | mutable | pin to tag or commit, or serve from device |
| Per-column metadata on the wire | none | phone needs app release per board | descriptor opcode (Q6) |
| KV capacity | 64 of 64 used worst case (corrected, see item 2 above) | no free slot | raise `kFooterMetadata` or reduce `kMaxConfigMetadata` (see item 3 above) |

## Q6. Proposal for firmware emitting STAC

### Principles

1. Keep `telemetry_fields.inc` byte-identical so the released digest stays valid. Put new semantics in a sibling file.
2. Generate the STAC JSON at build time on the host, never at runtime.
3. Serve it from flash in chunks, with zero heap growth.
4. Keep the footer small, using an href and a hash, not the document.

### 1. Build-time descriptor

Add `telemetry_semantics.inc` per board with one row per column, keyed by name.

```c
SEM(pm25_atm_ug_m3, "ug/m3", "mass_concentration_of_pm2p5_ambient_aerosol_particles_in_air",
    "PM2.5", "الجسيمات الدقيقة ٢٫٥", "instant", 0, 1000, "aqi_us_pm25")
```

`tools/telemetry_contract_fixture.cpp:59-77` already compiles `kFields` on the host and prints dictionary JSON `{schema, schema_version, dictionary, firmware, sha256, fields:[...]}`. `tools/test_telemetry_contract.py:160-200` runs it with `--dictionary-out`. (Corrected, see item 6 above.) `tools/test_waveshare_contract.cpp` does the same for Waveshare. Extend that path to join `kFields` with the semantics table and emit a STAC Collection. A static assert keeps both tables the same length and order.

Embed the minified Collection as a generated `const char[]` in `.rodata` (CMake `target_add_binary_data` or a generated header). Emit a second digest, `kDescriptorSha256`, over the generated JSON bytes.

### 2. Digest versioning

| Digest | Covers | Changes when | Used by |
|---|---|---|---|
| `dict` (existing) | raw `telemetry_fields.inc` | columns or wire keys change | released apps, unchanged |
| `desc` (new) | generated STAC Collection bytes | labels, ranges, units, scales change | new apps, cache key |

Labels and scales can then evolve without breaking the released `dict` profiles. (Corrected, code tables move `dict`, see item 7 above.)

### 3. BLE and LAN command

- New read-only opcode `0x11 GET_DESCRIPTOR` (opcodes are `0x01` to `0x10`, `ble_sync.h:43-60`).
- New frame type `0x4F DESCRIPTOR_CHUNK` (frame types end at `0x4e`, plus `0x7f` error, `ble_sync.h:63-86`). Opcodes and frame types are separate number spaces.
- Chunk like the existing config pages, `{offset, total, bytes}`, so the same reassembly code works on BLE and LAN (`AQS1` on port 47390). (Corrected, see item 9 above.)
- Advertise support by adding `"desc":"<16 hex prefix>"` to `info`. The full 64 hex fits (about 375 of 400 bytes on Waveshare) but leaves only about 25 bytes, so a 16 hex prefix plus full hash inside the descriptor is safer. Old apps ignore unknown keys. Bump `proto` only if an old app would misbehave, which it should not.
- An app with a cached `desc` skips the download.

### 4. Parquet footer

Add two keys.

| Key | Value | Size |
|---|---|---|
| `stac:collection_href` | pinned URL or `urn:opensensor:collection:<board>:<desc sha>` | under 200 bytes |
| `stac:descriptor_sha256` | 64 hex | 64 bytes |

Do not put the full Collection in the footer. It would exceed the 4096-byte value cap for CoreS3 (81 columns at about 150 to 250 bytes each is 12 to 20 KB) and would break the rule in `table-and-observation-model.md:114`. To free slots, raise `kFooterMetadata` from 64 to 66 or lower `kMaxConfigMetadata` from 32 to 30 (`aq_logger.cpp:93-102`). Both boards use far fewer than 32 board entries today (5 and 13). (Corrected, raising `kFooterMetadata` alone fails in the writer, see item 3 above.)

Optionally move existing `dictionary_uri` from `main` to a tag or commit URL at the same time.

### 5. Flash and RAM cost

| Item | Estimate |
|---|---|
| Minified Collection JSON, CoreS3 81 columns | 12 to 20 KB raw, 3 to 5 KB gzip |
| Minified Collection JSON, Waveshare 62 columns | 9 to 15 KB raw, 2 to 4 KB gzip |
| App partition | 6 MB (`0x600000`, both `partitions.csv:4`), image about 1.4 MB, so cost under 0.4 percent |
| RAM | near zero. Streamed from `.rodata` into the existing frame buffer. No new heap. |
| Parquet footer | about 260 bytes per file |

Serving raw JSON is simpler and avoids a gzip dependency on the phone. Raw is affordable here.

### 6. Term mapping

| Firmware source | STAC term | Notes |
|---|---|---|
| board model plus schema name | Collection `id` (`opensensor-cores3-v5`) | one per board schema |
| station UUID | Collection per station, or Item property `opensensor:station_id` | prefer property on Items |
| one Parquet file | Item, `assets.data` with `type: application/vnd.apache.parquet` | Item per file, Hive day as partition |
| H3 cell | Item `geometry` (cell polygon), `grid:code` `H3-<cell>` | from location KV |
| first and last UTC row | `start_datetime`, `end_datetime` | unsynced files have null datetime, need `datetime` rule |
| `FIELD.name` | `table:columns[].name` | |
| physical type | `table:columns[].type` and STAC 1.1 `data_type` | `int32`, `int64`, `float32` |
| `field_id` | `table:columns[].opensensor:field_id` | join key to Iceberg |
| unit | STAC 1.1 `unit` (UCUM) | keep project label as `opensensor:unit_label`. See item 10 above |
| CF name | `cf:standard_name` (CF extension) | |
| procedure | `opensensor:procedure` | |
| validity | `opensensor:validity` | |
| property URI | `opensensor:property` | |
| LIVE key | `opensensor:live_key` | lets the app read LIVE generically |
| row group stats | STAC 1.1 `statistics {minimum, maximum}` on Item asset | host computes after sync |
| time and numeric axes | `cube:dimensions` (time, station) and `cube:variables` per measured column | Datacube extension |
| status code tables | `classification:classes` | Classification extension |
| labels, aggregation, valid range, display scale, precision | `opensensor:display` custom extension | needs a published JSON Schema |

The custom `opensensor:display` extension holds `{labels:{en, ar}, aggregation, valid_range, precision, scale:{id, bands:[{max, label:{en, ar}, color}]}}`. Its `precision` is display only, see item 11 above.

### Rollout

1. Host only. Generate the Collection from the fixture, publish it in the repo, have the app consume it keyed by the existing `dict`. No firmware change.
2. Firmware. Embed the descriptor, add `desc` to `info`, add opcode `0x11` and frame `0x4F`, add the two footer keys.
3. Host pipeline. Write Items per synced file and push them to the source.coop catalog.

Step 1 alone removes the hardcoded `LiveProfile` for released firmware.

## Q7. AQ-specific names, wire contract or internal

### Wire contract (keep)

| Name | Location |
|---|---|
| GATT base UUID `c0a5e9f0-XXXX-4b1a-9c3e-2d7f8a6b4e01` | `ble_sync.cpp:39`, `ble_advert.h:11` |
| BLE device name `AQ-xxxx` (max 11 chars), resolved in #98 as the board constant `OS-` | `ble_sync.cpp:1053-1058`, `:1120`, `:1185` |
| mDNS host `aq-xxxx`, resolved in #98 as `os-xxxx` | `sync_service.cpp:28-34` |
| mDNS service `_aqsync._tcp` and TXT `proto`, `station`, `dev`, `fw` | `wifi_link.cpp:541-551` |
| LAN handshake `AQS1`, port 47390 | `wifi_link.cpp:937`, `device_config.h:19` |
| `info`, `status`, `live` JSON keys and LIVE keys | `ble_sync.cpp:1059-1066`, `aq_logger_live.cpp`, both `.inc` files |
| Opcodes `0x01` to `0x10` and frame types `0x10` to `0x4e`, `0x7f` | `ble_sync.h:43-86` |
| NVS `parquet` with keys `station`, `codec`, `boots` | `aq_logger.cpp:384-420`, `:597`, `aq_logger_codec_store.h:19-20` |
| NVS `aqcfg` with keys `cfg`, `token`, `location`, `ntp`, `pair` | `device_config.cpp:20`, `:253`, `:450`, `:461`, `:466`, `:565` |
| NVS `aqdump` with key `reported` | `log_persist.cpp:22-23` |
| Hive paths `station=`, `year=`, `month=`, `day=`, `unsynced/boot=`, `data_HHMM_` prefix | `aq_logger.cpp:690-710` |
| Parquet column names and footer KV keys | both `.inc` files, `aq_logger.cpp:851-895` |
| Schema names `cores3-telemetry-v5`, `waveshare-sim7670g-telemetry-v5` and property URNs `urn:walkthru-earth:*` | contract headers, `.inc` headers |
| Serial lines `PARQUET …`, `AQ OWNER_PIN`, `AQ WIFI_PROFILE`, `AQ CONFIG` parsed by host tools | `aq_logger.cpp:2522-2534` and tools |

NVS names survive OTA and erase only with NVS, so renaming them loses the station identity and pairing.

### Internal (renameable)

| Name | Location | Caveat |
|---|---|---|
| Doc labels `AQCommon`, `AQSensors`, `AQRuntime`, `AQConnectivity`, `AQLogger`, `AQLocation` | `firmware/common/README.md:3-4`, `:34-44`, other READMEs | docs only |
| C++ namespaces `aqlogger`, `aqsync`, `aq::location`, `aq::network_time`, `ble`, `telemetry` | sources | none |
| Task and PM lock name `aq-lan` | `wifi_link.cpp:1392`, `:1398` | visible in diagnostics only |
| Kconfig menus and symbols `AQ_LOGGER_WORKER_WDT*`, `AQ_LOGGER_BENCH_STALL`, `AQ_LOG_PERSIST_*`, `AQ_PM_DFS_*` | `common/logger/Kconfig`, `common/runtime/Kconfig` | update `sdkconfig.defaults` of both boards and `tools/idf-bench-stall-sdkconfig.defaults` together |
| Project names `aq_cores3`, `aq_waveshare` | `esp-idf-cores3/CMakeLists.txt:22`, Waveshare `CMakeLists.txt:37` | changes `.bin` file names used by release workflow and scripts |
| Board components `aq_cores3`, `aq_waveshare_logger`, `aq_waveshare_diagnostic` | board `components/` | update `EXTRA_COMPONENT_DIRS` and `REQUIRES` |
| `created_by` strings `m5stack-aq-parquet version 0.2`, `aq-parquet version 0.1` | `telemetry_logger.cpp:180`, Waveshare `telemetry_contract.h:23` | readers do not parse it |
| `/sd/parquet` legacy directory | `telemetry_logger.cpp:189` | keep while old cards exist |
| `LiveProfile` and per-digest tables in the app | `LiveProfile.kt` | replaced by descriptor |

## Open questions for the owner

1. Should the Collection be per board schema with station as an Item property, or per station? This report recommends per board schema.
2. Which AQI scale should ship first, US EPA or WHO 2021, and who owns the Arabic labels?
3. Should `dictionary_uri` move to a tag pinned URL in the same change as the new footer keys?
