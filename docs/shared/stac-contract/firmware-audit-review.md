# STAC firmware audit review

> Dated history, kept as written on 2026-10-25. File names such as `aq_logger.cpp` and `station_identity` were renamed later. Read [the decisions](../decisions/README.md) and the current code for today's layout.

Independent review of `firmware-audit.md` at commit `2b49114` on 2026-10-25. No firmware, app or doc file was changed. No board was flashed and no serial port was opened. The only host command run was `pixi run python tools/test_telemetry_contract.py --dictionary-out <scratch>` (host only, all cases PASS). Web pages were fetched through `r.jina.ai`. App paths are under `/Users/yharby/Documents/gh/walkthru-earth/opensensor/opensensor-space-mobile`, not `../opensensor-space-mobile` (the orca workspace folder of that name is empty).

Marks. **Confirmed** means checked against the cited file, command output or spec text. **Plausible** means inferred from code and not exercised.

Most of the report holds up. All listed LIVE key line numbers, the two digests (reproduced with `shasum -a 256`), the contract header line ranges, the `info` buffer, the opcode and frame ranges, the Hive layout, the Q7 wire names and the NVS keys were checked and match.

## Corrections

C1. **Waveshare table starts at line 9, not 13.** The 62 FIELDs sit on `firmware/esp-idf-waveshare-sim7670g/logger/telemetry_fields.inc:9-60`, `:63-66` and `:69-74` (report line 24). Confirmed.

C2. **Per-column metadata already leaves the device at runtime, over serial.** The report says procedure, unit, validity and property URI exist "only in the source file and in the host fixture output" (report lines 10 and 50-53). The `parquet schema` console command prints every field with `procedure`, `unit`, `validity` and `property` from `config.fields` (`firmware/common/logger/src/aq_logger.cpp:2522-2534`, documented at `docs/shared/table-and-observation-model.md:138`). The data is in flash and reachable at runtime. It is not on BLE or LAN, and the LIVE key is not printed. Confirmed.

C3. **"64 of 64 used" is a compile time budget, not usage.** `aq_logger.cpp:92-102` reserves 22 fixed, 7 location, 3 anchor and 32 board entries. Real use is CoreS3 37 (22+7+3+5, `telemetry_logger.cpp:141-149`) and Waveshare 45 (22+7+3+13, `main.cpp:116-148`). Anchors are only written when the anchor source is not none (`aq_logger.cpp:886-892`). There are 27 and 19 free array slots today. Report lines 13, 140 and 223 should say so. Confirmed.

C4. **The 64 entry cap is also hardcoded in the writer.** `firmware/common/src/parquet_writer.cpp:593` checks `metadata_count > 64` as a literal, separate from `kFooterMetadata`. The report's option "raise `kFooterMetadata` from 64 to 66" (line 273) would compile and then fail every `Writer::finish` once the array is filled past 64. Confirmed.

C5. **No budget change is needed at all for two new keys.** `stac:*` keys can travel as board metadata (`Config.metadata`), which is checked only against `kMaxConfigMetadata = 32` at `aq_logger.cpp:2930`. CoreS3 would go from 5 to 7 and Waveshare from 13 to 15. Adding them to the fixed list instead means `kFixedMetadata` 22 to 24, and the static_assert at `aq_logger.cpp:99-102` then forces lowering `kMaxConfigMetadata`. Confirmed (code), the choice is a recommendation.

C6. **Footer limits fail the file, they do not truncate.** A value over 4096 bytes or a count over 64 makes `Writer::finish` return `fail(...)` (`parquet_writer.cpp:593-600`). The logger then takes the close failure path and keeps a `.partial`. Value lengths are not validated in `begin()`, so a too long board value would fail every file, not one. Report line 140 reads as if values are simply capped. Confirmed for the writer, Plausible for "every file" (follows from static board values).

C7. **The LAN `HELLO` has its own 400 byte cap.** `firmware/common/connectivity/src/wifi_link.cpp:1020-1031` copies `info` into `std::uint8_t frame[4 + 400]` and returns false above 400. Any `info` growth must respect both `ble_sync.cpp:56` (`char info_text[400]`, so 399 usable) and this frame. If the BLE `snprintf` at `ble_sync.cpp:1059-1066` overflows, `begin()` returns false and BLE does not start at all. The report covers only the BLE buffer (lines 95 and 261). Confirmed.

C8. **`info` headroom numbers.** Computed lengths are CoreS3 287 and Waveshare 301 bytes. Adding `,"desc":"<64 hex>"` gives 361 and 375 (24 bytes left on Waveshare). A 16 hex prefix gives 313 and 327. The report's "about 375" and "about 25" match. Confirmed.

C9. **The fixture's "widest value" LIVE test is not the worst case.** `tools/telemetry_contract_fixture.cpp:24-39` and `tools/test_waveshare_contract.cpp:56-71` set Int64 fields to `INT64_MIN`, which makes `utc` overflow the x1000 scaling and get omitted (`aq_logger_live.cpp:66-76`), and set floats to `-999.9`. Measured with a scratch model of `format_live`, the test case is 418 bytes on CoreS3 and 334 on Waveshare. A valid 19 to 20 digit `utc` adds about 27 bytes (445 CoreS3). The true upper bound with every float printing 31 characters is about 520 bytes on CoreS3 (over the 480 cap) and 411 on Waveshare. Realistic rows are about 308 and 249 bytes. Real headroom for new CoreS3 LIVE keys under the test's own rule is about 35 bytes, not the 62 the test implies. Confirmed (code), sizes Plausible (scratch model, not the fixture).

C10. **"Append only per Iceberg v3 rules" applies only from schema v5.** Field ids were introduced in commit `e166978` (#22), the same commit that renamed position 9 `event_time_utc_ns` to `event_time_utc_us` and changed its unit from `ns_since_unix_epoch` to `us_since_unix_epoch` in place (`git show e166978 -- '*telemetry_fields.inc'`). Earlier files carry no field ids and the same positions mean different units (`table-and-observation-model.md:73`). So `field_id` is a stable join key only for schema v5 files, not across the archive (report line 203). No later commit reordered or removed a FIELD in either table (`git log --follow` on both `.inc` files). Confirmed.

C11. **The fixture does not take `--dictionary-out`.** The fixture binary is invoked as `<exe> dictionary` (`tools/telemetry_contract_fixture.cpp:59-76`). `--dictionary-out` is an option of the Python test (`tools/test_telemetry_contract.py:119`, used at `:194-195`). Report line 243. Confirmed.

C12. **Descriptor size is underestimated.** The current CoreS3 dictionary JSON from the fixture, with no STAC wrapping, no labels, no CF names and no scales, is 21,383 bytes pretty printed and 15,360 bytes minified (zlib about 1.9 KB). The largest single field object is 218 bytes. A STAC Collection with `table:columns`, `cube:variables`, `classification:classes`, two languages and display scales will likely be 25 to 40 KB raw, not 12 to 20 KB (report lines 273 and 281). Confirmed for the 15.4 KB baseline, Plausible for the Collection estimate.

C13. **Image size is from the Arduino era.** "image about 1.4 MB" (report line 283) matches Arduino builds in `docs/boards/m5stack-cores3/bench-verified.md:233` and `:376`. No ESP-IDF 6.1 image size is recorded in the worktree. The 6 MB app slot is right for both boards (`firmware/esp-idf-cores3/bringup/partitions.csv:4`, `firmware/esp-idf-waveshare-sim7670g/logger/partitions.csv:4`). The conclusion (cost well under 1 percent of the slot) still holds even at 40 KB. Confirmed.

C14. **"Every new board or column needs an app release today" is too strong.** `LiveProfile.kt:114-135` has a `VOCABULARY` fallback for unknown digests, so known LIVE keys still render. What needs a release is a new key, a retyped key or a reviewed temperature source (`LiveProfile.kt:12-25`, `:100-107`). Report line 134. Confirmed.

C15. **"The phone receives none of the per-column metadata" needs a qualifier.** Synced Parquet files give the app column names, physical types, field ids and all footer KV. `DailyParquetBuilder.kt:488` (`COPIED_METADATA`) already copies `dictionary_sha256`, `pms_status`, `light_status` and other code tables into derived files. The gap is units, labels and meanings for LIVE before any file sync. Report line 11. Confirmed.

C16. **"App hardcodes bands" undersells the app.** `Metrics.kt` in the app already defines a metric registry with WHO 2021 scales and the EPA PM2.5 correction. Open question 2 should start from that. Report line 215. Confirmed.

C17. **n16r8 leftovers are not empty.** `firmware/esp-idf-n16r8/build/diagnostic` in the main checkout holds 1,158 untracked build files (sdkconfig, flash args, a build sha256 JSON). The other folders are empty. No tracked schema exists, so the conclusion stands. PR #68 is merged, docs only, and adds `product/board-options.md` (`gh pr view 68`). Report line 28. Confirmed.

C18. **Small line and wording fixes.** The LIVE key length limit is `kMaxLiveKey = 15` in `aq_logger_live.h:9`, not `aq_logger.h:13-23` (report line 106). `device_config.cpp` and `device_config.h` live in `firmware/common/runtime/src/` (report lines 332 and 336). The CoreS3 digest header comment spans `telemetry_dictionary_digest.h:5-9`. Confirmed.

## New edge cases

E1. **LIVE overflow is silent and leaves a stale snapshot.** If the object exceeds 480 bytes, `format_live` returns 0 and `publish_live` skips both BLE and LAN with no log (`aq_logger.cpp:1614-1622`). The last good LIVE value stays cached and keeps being served. Adding keys can therefore freeze LIVE without any error. Confirmed (code), not exercised on a board.

E2. **Float edge cases in LIVE.** `%.1f` is applied only when `isfinite`, so NaN and Inf keys are omitted. `-0.0` prints `"-0.0"`. A value whose text is 32 characters or more (about |x| >= 1e28) is silently omitted (`aq_logger_live.cpp:85-95`). `utc` outside the x1000 range is omitted (`:66-76`). A descriptor must say "absent means invalid or unrepresentable", not only "invalid". Confirmed.

E3. **Precision field would be wrong for small values.** LIVE prints one decimal for all floats. Accelerometer `g` values below 0.05 print as `0.0` or `-0.0`. An `opensensor:precision` field that differs from 1 would mislead unless LIVE formatting changes, which is a wire change. Confirmed (code).

E4. **`.inc` edits and line endings.** `.gitattributes:3-4` forces `eol=lf` on `firmware/**/telemetry_fields.inc`, so CRLF checkouts do not move the digest. Comment and whitespace edits still do. The digest is only checked, never regenerated, by `tools/build-idf-board.sh:19-23` (`shasum -c`) and by `tools/test_telemetry_contract.py:154` and `tools/test_waveshare_contract.py:174`. CMake does no check. A new `desc` digest needs the same three gates or it will drift. Confirmed.

E5. **Code tables differ by board and are not desc only.** Waveshare `pms_status` adds `5=model-mismatch` (`main.cpp:116-148`), CoreS3 stops at 4 (`telemetry_logger.cpp:141-149`), the protocol LIVE table lists 0 to 4 (`docs/shared/ble-sync-protocol.md:179-214`) and the app `PmsStatus` enum has 0 to 4. Owner decision #39 plans `pms_status 6`. Under the LIVE stability rule (`ble-sync-protocol.md:208-214`) a new code value is a layout change, so the `dict` digest must move too. The report's split "dict for columns, desc for labels" (lines 249-254) must put code values on the `dict` side. Confirmed.

E6. **Same key, different meaning across boards.** `t` is ambient from the PMS5003T on Waveshare (`.inc:34`) and always null on CoreS3, where the app uses `imu_t`, a board temperature (`cores3 .inc:48`). `pct` is `battery_range_0_100` from M5 power on CoreS3 (`.inc:56`) and a truncated gauge value capped at 100 on Waveshare (`.inc:43`). `bat` is mV on both from different sources. Waveshare `sensor_model` KV is the literal `PMS5003T` while config page 0 uses the NVS owner provisioned model. A shared `urn:opensensor:property` (report line 211) would collapse these. `table-and-observation-model.md:138` says property URIs include the column name precisely so CF=1 and atmospheric PM cannot be conflated. Keep per board properties and add a separate "comparable to" link. Confirmed.

E7. **Unsynced files cannot be valid STAC Items.** STAC requires `datetime`, or a null `datetime` with both `start_datetime` and `end_datetime` (common metadata, URL below). Unsynced files have null UTC on every row (`docs/shared/telemetry-pipeline.md:164`). Reconciliation happens on the phone, which rebuilds dated daily files. Emit Items only for dated files or for reconciled derived files. Files split on any clock epoch or window change (`telemetry-pipeline.md:60`), so one file never mixes epochs. RTC restored files (`clock_status = 2`) are dated but approximate, so Items should carry `clock_status` and `clock_epoch`. Confirmed (spec and docs), Item policy is a recommendation.

E8. **Derived daily files drop new footer keys.** `DailyParquetBuilder.kt:488` copies a fixed list. A `stac:*` key added on the device will not reach host or source.coop files built from the phone unless that list grows. Confirmed.

E9. **Station identity is per flash, not per place.** `station_identity` (`aq_logger.cpp:384-420`) reads NVS `parquet/station` and creates a UUIDv4 if absent. Erasing NVS makes a new station (`telemetry-pipeline.md:54`). A board moved to a new site keeps its station UUID. STAC Items keyed by station need the H3 cell and time range to tell deployments apart. Confirmed.

E10. **Pre-pairing access is not possible on GATT.** Every characteristic, including `info`, has `BLE_GATT_CHR_F_READ_ENC` (plus `READ_AUTHEN` in MITM mode) at `ble_sync.cpp:1123-1134`. LAN needs `AQS1` plus the token before `HELLO` (`wifi_link.cpp:38`, `:937`). A descriptor served by opcode is therefore post-pairing only. The only pre-pairing surfaces are the advert and the mDNS TXT (`proto`, `station`, `dev`, `fw` at `wifi_link.cpp:541-551`). A board level descriptor holds no secrets, so this is fine. Do not put station or location data in it. Confirmed.

E11. **`grid:code` with H3 can leak location.** Location is provisioned as an H3 cell coarsened to a privacy cap (`ble-sync-protocol.md:308`). Public Items with `grid:code` or a cell polygon geometry publish that cell. Use the capped cell only and never the center lat and lon at fine resolution. Plausible.

E12. **Boards without BLE, Wi-Fi or SD.** PR #68 lists LoRa solar options. A LoRa only board cannot use opcode `0x11`. The descriptor must also be reachable by `desc` hash from a host catalog, which the report's rollout step 1 already allows. Plausible.

E13. **Unknown opcode probe.** An old firmware answers an unknown opcode with ERROR 11 (`ble-sync-protocol.md:218`, `:274`). The app can probe `0x11` without the `desc` key in `info`, the same way it probes `LIST_SINCE`. Opcode `0x11` and frame `0x4F` are unused in firmware, `tools/ble_sync.py` and the app. Frame `0x11` already exists (LIST_END), which is allowed because the number spaces differ, but it is easy to misread in logs. Confirmed.

E14. **Old apps and a new frame type.** The app's `Frames.kt:499` throws on an unknown frame type. `LinkSession.kt:444-456` catches this and logs `UnknownDropped`. On LAN, `TcpLinkDriver.kt:191-199` treats a decode failure during the handshake as a protocol error. A new frame is only safe if it is never sent unsolicited, which holds for a request driven opcode. Confirmed.

E15. **App JSON tolerance.** `AqJson` sets `ignoreUnknownKeys=true` (`JsonModels.kt:13-18`), so a new `desc` key in `info` is safe for released apps. `LiveSample` (`JsonModels.kt:132-185`) has fixed types (`pm` and `c` as Double, `n` as Long, `bat`, `pct`, `chg`, `vbus` as Int, `als` as Long). Retyping an existing LIVE key breaks parsing. Confirmed.

## Proposal risks and changes

P1. **Use a READ style transfer, not CONFIG_CHUNK.** `CONFIG_CHUNK 0x4A` is BLE only, uses `flags u8, total u16, offset u16`, slices of at most 128 bytes, no terminal frame and contiguous offsets (`ble-sync-protocol.md`, config pages section). A 25 to 40 KB descriptor would be 200 to 300 notifications, and Android drops 16 to 18 frames in bursts (`ble-sync-protocol.md:276`). Prefer `GET_DESCRIPTOR(offset u32, length u16)` that returns one chunk per request up to the negotiated frame size (512 on BLE, `payload_max` 1024 on LAN), so the app can re-request lost ranges. Plausible.

P2. **Do not put the `desc` hash in `info` on Waveshare at full length.** 24 bytes would be left before both 400 byte caps (C7, C8). Use the 16 hex prefix or rely on the ERROR 11 probe (E13). Confirmed numbers.

P3. **Units need two fields, not one.** STAC common metadata asks for UCUM or UDUNITS-2. The CF extension says `unit` corresponds to CF `units`, which are UDUNITS. The report maps `degC` to UCUM `Cel` (line 209), which is not a UDUNITS string. Pick UDUNITS where `cf:standard_name` is used, or keep UCUM in an `opensensor:ucum` field. Watch the traps. `g` in the CoreS3 table means standard gravity (`.inc:39-41`), which is `[g]` in UCUM, while UCUM `g` is gram. `byte` is `By`. `count` is `{count}`. `YYYYMMDD` and `hhmmss` are formats, not units. Confirmed (spec text and `.inc`).

P4. **CF names need care.** `mass_concentration_of_pm2p5_ambient_aerosol_particles_in_air` fits the `atm` columns only, not the CF=1 columns. `air_temperature` must not be used for `imu_temperature_c`. PMS5003T `t` and `rh` accuracy is unmeasured (`docs/boards/waveshare-esp32-s3-sim7670g/pms5003t.md:23`). An empty `cf:standard_name` is the spec's answer when no name fits. Confirmed (spec), mapping is a recommendation.

P5. **`classification:classes` placement.** The Classification extension targets assets, bands and Item properties and requires `value` (integer) and `name` (letters, digits, `-`, `_`). It does not list Table Column Objects. Putting it inside `table:columns[]` is allowed by STAC's open schema but is not a documented location. A `bands` entry per code column or a custom `opensensor:codes` is cleaner. Plausible.

P6. **Lower `kMaxConfigMetadata` or pass keys as board metadata (C5), never raise 64 alone (C4).** Confirmed.

P7. **Make the generator gate fail closed.** The new digest must be checked in `build-idf-board.sh`, the two contract tests and CMake, the same way as `dict` (E4). Add a test that `format_live` of a row with every LIVE key valid and widest real values stays under 480 bytes, including a real `utc` (C9). Plausible.

P8. **Pin `dictionary_uri` to a commit.** The repo is public (`gh repo view walkthru-earth/aq-parquet`), so the URL resolves today but points at mutable `main` (`telemetry_contract.h:22-24`). A commit URL plus the existing digest is enough. Confirmed.

P9. **Collection per board schema plus Items per dated file is consistent with `table-and-observation-model.md:114`.** Keep the full document out of the footer as the report says. Confirmed.

## STAC spec check

Sources fetched through `r.jina.ai` from `raw.githubusercontent.com` (github.com HTML was rate limited).

| Report claim | Spec | Mark |
|---|---|---|
| `table:columns[]` with `name`, `type` | Table v1.3.0. Column `name` REQUIRED, `type` native type (Parquet types recommended). Common metadata `unit`, `data_type`, `statistics` allowed in a Column. https://github.com/stac-extensions/table | Confirmed. Use the v1.3.0 schema URI. `opensensor:field_id` is a custom field. |
| `cf:standard_name` | CF v1.0.0 (Proposal maturity) fields are `cf:standard_name`, `cf:cell_methods`, `description`, `unit`. Usable in Items, Collections, assets, bands, `cube:variables`. https://github.com/stac-extensions/cf | Confirmed. Note Proposal maturity. |
| STAC 1.1 `unit`, `data_type`, `statistics`, `bands` | Common metadata defines them, `unit` as UCUM or UDUNITS-2. https://github.com/radiantearth/stac-spec/blob/master/commons/common-metadata.md | Confirmed |
| `start_datetime`, `end_datetime`, null `datetime` | `datetime` is string or null, range fields are RFC 3339 UTC. Same URL. | Confirmed, see E7 |
| `cube:dimensions`, `cube:variables` | Datacube v2.3.0. `cube:dimensions` is REQUIRED when the extension is used. A temporal dimension needs `extent`. A "station" dimension must be a custom type, never `spatial` or `geometry`. https://github.com/stac-extensions/datacube | Confirmed |
| `classification:classes` | Classification v2.0.0. Class `value` and `name` REQUIRED. https://github.com/stac-extensions/classification | Confirmed, see P5 |
| `grid:code` `H3-<cell>` | Grid v1.1.0, Item scope only, form `{designation}-{code}`. No H3 designation is listed but proprietary grids are allowed. https://github.com/stac-extensions/grid | Confirmed. Not usable on a Collection. |
| `application/vnd.apache.parquet` | Listed in STAC best practices media types. https://github.com/radiantearth/stac-spec/blob/master/best-practices.md | Confirmed |
| `opensensor:display` custom extension | Needs a published JSON Schema and a `stac_extensions` URI, as the report says. | Confirmed |

CF standard names table and UCUM are free to reference. Copying the UCUM table into firmware or the app would bring its own license terms. Referencing codes only avoids that. Plausible, license text not fetched.

## Open questions

1. Should Items exist for unsynced files at all, or only for phone reconciled daily files (E7, E8)?
2. Units, UDUNITS for CF compatibility or UCUM for STAC preference, or both fields (P3)?
3. Is the `pms_status 6` change (#39) planned before or after the descriptor, given it moves `dict` (E5)?
4. Accept a request per chunk transfer instead of CONFIG_CHUNK style streaming (P1)?
5. What H3 resolution may appear in public Items (E11)?
6. Should the app start from its existing WHO 2021 and EPA metric registry for display scales (C16)?
7. For LoRa only boards, is a catalog lookup by `desc` hash enough (E12)?
