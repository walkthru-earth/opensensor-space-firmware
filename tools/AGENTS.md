# Host tools

Run these scripts through pinned `pixi` tasks; `pixi.toml` names the task and
`docs/shared/development.md` explains setup. `tools/host_fakes/` enables pure
C++ contract tests and cannot prove behavior on a physical board.

| Need | Tool / gate |
| --- | --- |
| Parquet footer/writer and reader compatibility | `parquet-test --sanitize`, `inspect_parquet.py` |
| Native GEOMETRY column and the `geo` footer key through the real writer, read by PyArrow and DuckDB | `geoparquet-test` |
| Footer reader and Iceberg DataFile metrics | `reader-test` (ASan + UBSan, crafted footers, fuzz loop) |
| File size, footer share and host encoding or compaction variants | `size-survey`, `size-survey-test` |
| Telemetry dictionary, units and validity | `telemetry-contract-test --sanitize` |
| BLE/LAN archive and framing | `archive-sync-test`, `test_ble_sync.py` |
| Protocol 3 wire codec through the shipped Python client, advert bytes, BYE, DIAG and ABORTED golden frames, the LAN `FrameRing`, BLE notify credits and link auth gates | `ble-sync-test`, `ble-advert-test`, `disconnect-test`, `frame-ring-test`, `ble-policy-test` |
| Router/local/public NTP order and H3 privacy geometry | `network-time-test`, `location-test`, `config-test`, `control-sync-test` |
| GNSS SIMCom `CGNSSINFO` dialect, fix to H3 cell, freshness, the per row location source rule and no file split on a GNSS cell (no receiver or UART proof) | `gnss-test` |
| Shared numeric rows, UTC helpers and archive wire primitives | `common-test` |
| Checked serial readback, fetch and bounded capture, send a command on cue over the one port open | `parquet_device.py`, `capture_serial.py --send-on`, `capture-serial-test` |
| Footer rebuild for a quarantined `.partial` from its page headers and the board field table, output in a separate directory and checked by PyArrow and DuckDB | `rebuild-partial --board waveshare --out <dir> <files>`, `rebuild-partial-test --sanitize` |
| Power cut cycles at the write, footer and rename stages, per boot loss account, baseline SHA-256 manifest and compare (owner bench only, tests use a fake port) | `bench-power-cut run|analyze|manifest|compare`, `bench-power-cut-test` |
| Discharge run curve of `battery_mv`, SOC, CRATE and `battery_health_state` against UTC as SVG, with power-critical closes and reboot loop small files per boot from serial or LOG_TAIL logs, overlapping polls joined, logs oldest first (owner bench, #61 item 1, tests use a generated run) | `analyze-discharge <files> --log <log> --out <dir>`, `analyze-discharge-test` |
| Full-flash backup publication (offline fake esptool) | `backup-test` |
| Native FAT mount root and archive directory creation | `logger-directory-test` |
| Storage worker units on host fakes. Queue and wakeups, watchdog supervisor, persisted codec, STATUS formatter, UTC rotation, LIST_SINCE ring and walk, SD space governor, OPEN CRC cache, SD DMA buffer, archive reset, UART provisioning and ring only radio diagnostics | `logger-work-queue-test`, `logger-watchdog-test`, `logger-codec-store-test`, `logger-status-test`, `logger-rotation-test`, `logger-list-since-test`, `storage-governor-test`, `crc-cache-test`, `sd-transfer-buffer-test`, `logger-archive-reset-test`, `logger-provision-test`, `debug-log-test` |
| Per-file C++ dialect and source path map of an SDK fixture build, and every fixture entry point calling it without swallowing its exit | `check_idf_target.py --compile-dialect <build>`, `compile-dialect-test` |
| One board built from two checkout paths gives the same images (SDK, CI job) | `repro-paths-check [board] [variant]`, `repro-paths-test` |
| Native SDK and affected board builds | `idf-setup`, `cores3-build`, `waveshare-build [logger|diagnostic]` |
| SDK compile fixtures for shared connectivity and logger, no board target, C++ dialect check only. They need ESP-IDF, so the gate skips them | `connectivity-build-test`, `logger-build-test` |
| Bench only `parquet bench-stall` image, parser bounds and default-off guard | `OS_BENCH=stall cores3-build` or `waveshare-build logger`, `logger-bench-stall-test` |
| Shared sensor drivers on fake buses | `pms-frame-test`, `ltr553-test`, `max17048-test` |
| Battery health states, thresholds, debounce, settling and the boot Low or Critical tie | `battery-health-test` |
| Uploader SigV4 against the public AWS vectors, STS body and bounded XML parser, upload policy rules, append only ledger and the session on fakes (#81) | `upload-test` |
| COREDUMP line once per saved core dump, NVS record and its read or write failures | `coredump-report-test` |
| Board-declared LIVE keys and JSON formatting | `logger-live-test` |
| The one `opensensor` footer key, its pinned JSON bytes, string escaping, fail closed errors (missing field, range, overflow), sample period, row group cap, deployment and calibration ids, BLE name prefix and mDNS host, and a non AQ fixture board (no PMS, 30 s, `SL-`) read by PyArrow and DuckDB. A source check proves `begin()` runs `footer::check_config()` before any file work and logs `operation=footer-config` and returns false on failure, each rule proved by mutation | `logger-footer-test` |
| POWER STATE lines, one finalize per discharge, the report wake, emergency close outcomes, retry and pass lines | `logger-power-test` |
| Battery close stopping LIST, READ and slow peer waits, the Retry remount recheck | `archive-sync-test`, `wifi-link-test` |
| LIVE key set frozen per dictionary digest (`fixtures/live_keys/`) | `telemetry-contract-test`, `waveshare-contract-test` |
| Footer per board built through `footer::build`, validated by `footer_contract.py` against the pinned stac-opensensor `footer.json` (Draft 7 with format checks), key set and order matched to the example, `dict` and `desc` matched to the headers, the `template.href` target published in `docs/shared/templates` with SHA-256 `desc`, a written file read back with exactly one key, the `geo` key checked against GeoParquet 2.0, `thing.board` equal to the template folder slug, and mutation proofs (stray key, number as string, wrong `desc`, missing field, free text board, short `boot_id`, bad `mobility`, rotation over 3600, extra member). Spec pinned at stac-opensensor `83787768cc8451109d48a03d4ef5493ffcd403dd` | `telemetry-contract-test`, `waveshare-contract-test` |
| Board template (#85) built by `board_template.h` from the FIELD, SEM and CLASS tables, checked against the checked-in `telemetry_template.h` and `telemetry_descriptor_digest.h`, validated against the pinned stac-opensensor v0.1.0 schemas in `stac_opensensor/`. Regenerate with `--write-template`, which also rewrites the published copy and `index.json` in `docs/shared/templates` | `telemetry-contract-test`, `waveshare-contract-test` |
| Published board templates (#86) in `docs/shared/templates/<board>/<dict>.json`, the exact `kTemplateJson` bytes with no final newline, SHA-256 equal to desc, one file per current dict and no stale file, `index.json` (`templates` by dict, `by_desc` by desc) matching the headers, `--write` pruning anything else under the folder, `live_keys` in field table and LIVE fixture order, `-text` in `.gitattributes`, each check proved by mutation | `published-templates-test` (`published_templates.py [--write]`) |
| Plugin contract (#17). Module, board and lock manifests validated against `docs/shared/plugins/v0.1.0/` (Draft 7, refs into the vendored stac-opensensor schemas, format checks). Each board's segment list expands to FIELD, SEM and CLASS rows equal to its `telemetry_*.inc`, and to the published template columns, instruments, LIVE keys and unit labels. Cross rules for unique names, properties and LIVE keys, one LIVE key to one column across boards, `kMaxColumns`, gates and derived columns. `shipped.lock` per board is append only, its newest `dict` is the SHA-256 of the raw `telemetry_fields.inc`, each release is named in the mobile migration notes, only a `baseline` release whose migration entry names its dict may edit a shipped segment, and only the segments its baseline lists, and every base branch entry (`origin/$GITHUB_BASE_REF` or `PLUGIN_CONTRACT_BASE`) stays byte identical. Each module manifest but `system` has a checked-in `<id>_module.h` beside it, generated by the gate and equal byte for byte. A missing, stale or orphan header fails, at any depth under `modules/`. The write path removes only an orphan that starts with the generated preamble and never overwrites a hand written file. Module ids that `os_sensor_module.h` declares in `ossensor`, plus `ossensor`, `std`, `telemetry` and `oslogger`, are reserved. Write mode for a new board writes its three `telemetry_*.inc` and a first `shipped.lock` once and refuses when any exists, the slug is taken or a cross rule fails. Each rule proved by mutation | `plugin-contract-test` (`plugin_contract.py`, `plugin-contract`), `plugin-headers` (`plugin_contract.py --write-headers`), `plugin-new-board` (`plugin_contract.py --write-board`) |
| Generated module headers and `os_sensor_module.h` (#17 step 4). Each board's module headers compile against its real `telemetry::contract::kFields`, `ossensor::resolve` equals the column positions the gate expands, each `Column` value is its `kColumns` index, and the example `InitError` builds a `Module` table that writes a `Row`. A module column the board lacks and an unknown name each stop the build, proved by negative compiles. The write helpers (#17 step 5) skip `kNoSlot` and past-the-row slots, `Writer` compiles unselected columns to nothing, a write of the wrong type, a write of another module's `Column` and a dropped `init_module` result fail to compile | `sensor-module-test` (`test_sensor_module.py`) |
| Waveshare row equality (#17 step 5). The module table in `sensor_modules.h` and the old collect in `waveshare_collect_reference.h` give byte identical rows for every PMS status, gauge read failure and sensor identity, with one gauge read per row | `waveshare-contract-test` |
| Pinned URIs (#89). `kDictionaryUri` (also the template describedby href) and `kTemplateHref` in each board `telemetry_contract.h` are raw GitHub URLs at a full 40 hex commit that is an ancestor of HEAD, and the bytes at that commit hash to `dict` and `desc`. A main, branch or tag URL, a short sha, a commit absent or off HEAD, wrong bytes and a shallow clone each fail, proved by mutation on a scratch repo, and `pinned_uri.h` static_asserts are compiled with the same URLs. `pin-uris` pins each URL to the commit that last wrote its bytes, dictionary first, and refuses an uncommitted change to any file it reads or writes (pinned files, digest headers, `telemetry_template.h`, `telemetry_contract.h`). PRs that move dict or desc land with a merge commit, everything else squashes | `pinned-uris-test` (`test_pinned_uris.py`), `pin-uris` (`pinned_uris.py --write`) |
| desc pre-build header check and post-build check that the app image holds the template bytes and their NUL, run by `build-idf-board.sh` | `check_board_template.py <board dir> [--build <dir>]`, `board-template-check-test` |
| Column semantics, classes and derived tables against the FIELD tables, each defect proved by a negative case | `column-semantics-test` |
| Formatting and static diagnostics, no board-name `#if` in `firmware/common` (`check_board_conditionals.py`), no product BLE name prefix literal in shared code or host tools (`check_name_prefix.py`), no retired AQ wire, NVS, Kconfig, serial, CMake, build variable, manifest, SDK cache, pixi, board component, logger source or common source file name outside history (`check_retired_names.py`, #93, #111) | `fmt-check`, `lint`, `board-conditionals-test`, `retired-names-test` |
| Sign app images with the owner key in `secrets/` (owner machine only, never CI), verify them and write SHA256SUMS. Tests use throwaway keys in a temp dir | `release-sign --out <dir> <app.bin>`, `release-sign-test` |
| Tag release workflow keeps least privilege per job, no credentials, cache or secrets, the push tag gate, `--draft`, the existing release guard and the tag pattern, each rule proved by mutation | `release-workflow-test` |
| Nothing under `secrets/` and no PEM private key tracked by git, run by `lint` | `secrets-check`, `secrets-check-test` |
| Full host gate, same entry point as CI | `gate` (`tools/gate.sh`, add new host test tasks to its list) |
| CI image size table from `idf.py size --format json2`, absolute sizes with a warning when the baseline is malformed | `size_report.py`, `size-report-test` |
| App image share of the smallest app slot in the board `partitions.csv`, fail closed over the budget in the tool | `ota-slot-check --label --partitions --build`, `ota-slot-check-test` |
| Chunked LAN OTA rules (contiguous chunks, slot size cap, chip and project match, SHA-256 before the boot switch, abort on any owner error) and the boot self test verdict and mark plan, host built with sanitizers | `ota-transfer-test`, `boot-self-test-test` |
| Serial core dump readback that prints without memory and keeps no raw dump on the host, even on a failed decode. The partition stays unless `--erase`, which clears only the coredump partition after a good read. Hardware only, never in the gate | `coredump-read <board> <checked-port> [variant] [--erase] [--dry-run]`, `coredump-read-test` |
| Static task stack worst case, xTaskCreate name and stack size cross check in every `#if` branch, and linker section budgets per image (`stack_budget.json`) | `stack-budget [--check] [--path]`, `stack-budget-test` |

- Never write eFuses or format SD; flash writes require the root `AGENTS.md`
  safety sequence and an explicit checked serial port. Only one process owns
  a serial port at a time.
- `owner-pin` and provisioning output are secrets. Do not retain them in logs,
  artifacts, fixtures, tests or documentation. Keep LAN tokens in ignored
  artifacts and never print them.
- Many tools still hard code the two boards (`BOARDS` tuples, `--board` choices). A new board goes into each list by hand until #141 derives them from `board.json`.
- Keep host fixtures deterministic. Record actual hardware evidence only in
  the relevant board's `bench-verified.md` with date, image hash and limits.

- `.github/workflows/ci.yml` runs `pixi run gate` and the board image builds on
  every pull request. Keep the gate free of tasks that need hardware, Bluetooth,
  a LAN peer or the SDK. Each gate task is bounded by `OS_GATE_TASK_TIMEOUT`
  (300 s), and the gate fails if a pixi `*-test` task is missing from its list,
  except the two SDK fixtures `connectivity-build-test` and `logger-build-test`.
  `CI result` is the only required check, so a new CI job must join its
  `needs` list, see `docs/shared/development.md#required-checks`.
- Firmware builds use the pinned native ESP-IDF wrapper and exact managed
  component pins. Host tests remain in Pixi. Never introduce an Arduino build
  route or infer physical native behavior from fixture compilation.
