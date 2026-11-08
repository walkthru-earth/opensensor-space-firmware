# Porting an ESP32-S3 board

The [firmware epic](https://github.com/walkthru-earth/opensensor-space-firmware/issues/6) uses
board-neutral services in `firmware/common`. A board adapter owns pins,
controller initialization, power, mount policy, measurements and capabilities.
Extend hooks or configuration for a new capability. Never branch on a board
name inside common code or include a sibling trial.

Sensor modules and boards are declared as versioned manifests, gated against the board files. See [Sensor plugins](plugins/README.md).

## Logger hooks and capabilities

The source contracts are [`os_logger.h`](../../firmware/common/logger/src/os_logger.h)
and [`sync_service.h`](../../firmware/common/connectivity/src/sync_service.h).
Descriptors, identity strings and callback contexts must outlive the services.

| Interface | Board supplies | Ownership and limits |
| --- | --- | --- |
| `oslogger::Config` | Static field dictionary, schema/dictionary identities, digest, firmware, `created_by`, acquisition metadata and archive roots. Optional sample period (`sample_interval_ms`, 10 s default), `deployment_id` and `calibration_id` (`unknown` default) and a required BLE name prefix (`OS-` on both boards) | Required runtime fields and types are in the [adapter contract](../../firmware/common/logger/README.md#adapter-contract). Measurements remain null when unavailable. Benchmark output stays outside synced archives. Check the [shared caps](../../firmware/common/README.md#shared-caps-a-new-board-must-check). The phone finds boards by the GATT service UUID or a prefix it knows |
| `Hooks::collect` | Sensor acquisition into a zero-initialized row | Main loop only. The runtime supplies clock, identity and standard counters |
| Storage hooks | `storage_total_bytes`, `storage_used_bytes`, `storage_free_bytes`, `storage_cluster_bytes` | Worker only with bus lock held. Free-space failure returns false and unknown cluster size returns zero |
| `Hooks::storage_remount` | Optional unmount/remount of the board-owned card | Worker only with bus lock held and no file open. Never format. Without this hook automatic remount recovery is unavailable |
| `Hooks::lock_bus`, `unlock_bus` | Both callbacks or neither | Borrow an existing bus mutex or let OSLogger create one. Complete display DMA before releasing a shared bus |
| `Hooks::rtc_read`, `rtc_write` | Optional UTC-second read/write with validation and verified write readback | Main loop only. Reject unset, invalid or voltage-low RTC data. Without RTC, UTC stays null until a real host/network anchor |
| `report_battery(state, mv)` | Optional battery state from `opensensor::battery::Monitor` in `firmware/common/power` | Main loop only, normally from `collect`. Prints one `POWER STATE` line per change and raises one emergency close of the open file per discharge, run only by the storage worker with bounded retries. Only ok or external power re-arms it. Never resets or sleeps |
| `Hooks::power_shed` | Optional load shedding on a battery state change | Main loop only, after the `POWER STATE` line. Default is no action. Must not block, reset or sleep |
| `Hooks::gnss_fix` | Optional newest GNSS `Fix` for moving Things, normally `opensensor::gnss::latest` | Main loop once per row, and the storage worker for GET_CONFIG page 1 `location.gnss_cell`. It must be safe from any task, never from an ISR. A fixed Thing ignores the fix for rows. Must not block. Without it a moving Thing writes location nulls. See [adding GNSS](#adding-gnss) |
| `start_links(display_detected)` | Actual display capability | Selects the first-boot pairing default. No display requires physical owner access to the per-device PIN |
| `Config::sync` | Optional `ossync::ServiceConfig` containing BLE/LAN options and command/sample queue depths | Shared defaults own ring/credit sizes, timeouts, link parameters and advertising policy. No board-name conditionals |
| Board sdkconfig defaults | Flash size, PSRAM mode, console, DFS and hardware-specific deltas | Use the shared SDK/connectivity defaults and pinned target checker. CoreS3 Quad PSRAM and Waveshare OPI PSRAM are different capabilities |

Mount storage before `begin(config, hooks, sd_mounted)`. Use the shared
[`archive_mount_config()`](../../firmware/common/src/fat_volume.h) and
[`archive_files.h`](../../firmware/common/src/archive_files.h) descriptor budget,
including four persistent archive readers. Reserve the 4 KiB internal DMA SD
transfer buffer before radios and keep it alive while mounted. Then call
`start_links(display_detected)` once and `poll()` on every main-loop pass.
OSLogger alone accesses files after startup. OSConnectivity alone initializes
radios, the LAN task writes sockets, and the NimBLE host sends BLE responses.

## Adding GNSS

GNSS is core location, not a plugin module. It adds no column, LIVE key,
manifest or dictionary change. The shared code lives in
[`firmware/common/gnss`](../../firmware/common/gnss/README.md) (pure parser,
fix-to-H3 and source rule) and `firmware/common/gnss_reader` (the UART task).
Only the pins, the UART number and receiver power stay in the board.

1. Add `../common/gnss` and `../common/gnss_reader` to the board
   `EXTRA_COMPONENT_DIRS` and `gnss_reader` to the board component
   `REQUIRES`. A board without a receiver lists `../common/gnss` only, because
   the logger needs it.
2. Fill one `opensensor::gnss::ReaderConfig` with designated initializers. Set
   a free `uart_port` (never the console or a sensor UART), the ESP `tx_pin`
   and `rx_pin` as wired on the exact PCB revision, the receiver `baud`, the
   `dialect` (`&opensensor::gnss::simcom_dialect()` for SIMCom modems) and an
   optional `power_on` callback with its `context`. Keep the default
   `reader_stack_bytes`, `priority` and `poll_period_ms` unless measured.
3. The power callback runs once on the reader task. It drives the receiver or
   modem supply on and returns true. It must never drive it off, and it must
   not touch strapping pins it does not own.
4. Call `opensensor::gnss::start(config)` before `oslogger::begin` and print
   `OS ERROR operation=gnss-start reason=<n>` when it fails. Logging continues
   without GNSS.
5. Set `.gnss_fix` in `oslogger::Hooks` to a function that calls
   `opensensor::gnss::latest(fix)`.
6. Add the `os-gnss` task to that image in `tools/stack_budget.json`, run
   `pixi run stack-budget --write-budgets --image <board>/<variant>`, then
   `--check`. Bump the board firmware identity and add a dated bench line
   once a real fix is seen.

A new receiver family, for example plain NMEA, is a new `Dialect` in
`firmware/common/gnss`, tested in `tools/test_gnss.cpp`. Board code and the
logger stay as they are. The Waveshare SIM7670G logger is the reference
example.

## Porting sequence

1. Add one justified native `firmware/esp-idf-<board>/` application with board
   docs, exact PCB revision, upstream wiring sources, pinned manifests,
   partition table and setup/build/flash wrappers. Keep source checks and bench
   results separate. Do not copy another board's GPIO or controller policy.
2. Register common component directories with CMake `EXTRA_COMPONENT_DIRS` and
   declare the required components. A logger uses
   `REQUIRES common runtime connectivity logger location`. A board with a
   Plantower PMS module selects the model explicitly. Preserve each board's
   schema and provenance.
3. Add Pixi tasks, target checks and board contract fixtures. Build both existing
   boards plus the new target using the [development gates](development.md#verification).
   A generic compile fixture never gets flashed. Write the board's
   `board.json` against the [plugin contract](plugins/README.md#writing-a-new-board-once),
   then run `pixi run plugin-new-board <path to board.json>` once. It writes
   the three `telemetry_*.inc` files and a first `shipped.lock`, and they are
   frozen from then on. Then follow [Contract and publish](#contract-and-publish).
4. Audit strapping pins, peripheral ownership, flash capacity, PSRAM, console
   and recovery. Before any physical write follow the full checked-port
   [identification and backup sequence](../../AGENTS.md#before-any-physical-flash-write).
   Current backup/flash helpers accept only the two existing 16 MiB targets.
   Extend them deliberately for a different capacity. Preserve NVS identity,
   credentials, keys, types and partition offsets.
5. Qualify a small diagnostic, sensor byte contract, card logging and immutable
   readback on the exact board. Keep captures and image hashes in ignored trial
   `artifacts/` and owner copies in `exports/`. Record only observed results in
   that board's bench record. Host tests do not prove SD timing, coexistence,
   reconnect, power or durability. Add the board to the docs router and README.

## Contract and publish

After `pixi run plugin-new-board` the board has its field tables but no
contract yet. `pixi run plugin-contract` fails until the published template of
step 4 is in. It also refuses a slug another board uses (step 1) and a
`shipped.lock` release that `mobile-migration.md` does not name (step 6). The
host contract tests and `pixi run gate` check the other steps. Nothing checks
step 8 until the shared writer exists.
Use the Waveshare V2 files as the example.

1. Board slug. Pick one lowercase slug that matches `^[a-z0-9]+(-[a-z0-9]+)*$`,
   vendor first, such as `m5stack-cores3`. The vendored stac-opensensor
   `tools/stac_opensensor/v0.1.0/footer.json` enforces the pattern.
   `docs/shared/plugins/v0.1.0/board.json` and `tools/footer_contract.py` check
   it too, and `pixi run plugin-contract` refuses a slug another board already
   uses. The same slug names the footer `thing.board`, the folder in
   `docs/shared/templates/` and the folder in `docs/boards/`.
2. Board contract header. In the board `telemetry_contract.h` set `kBoard` to
   the slug, and set `kSchemaName`, `kDictionaryVersion`, `kFirmware` and
   `kCreatedBy`. Set `kDictionarySha256` in `telemetry_dictionary_digest.h` to
   `shasum -a 256 telemetry_fields.inc`.
3. Host tooling. Add the board to `BOARDS` in `tools/published_templates.py`.
   Write a board contract test like `tools/test_waveshare_contract.py`, add its
   `*-test` task to `pixi.toml` and list it in `tools/gate.sh`. The gate refuses
   a test task it does not list. Add the LIVE fixture
   `tools/fixtures/live_keys/<fixture>.json` and name it in the `fixture` field
   of the new `BOARDS` entry.
4. Template. The contract test with `--write-template` builds the template
   through `tools/board_template.py`. It writes `telemetry_template.h` and
   `telemetry_descriptor_digest.h`, publishes
   `docs/shared/templates/<slug>/<dict>.json` byte exact and rewrites
   [`index.json`](templates/index.json). Then run `pixi run pin-uris` twice,
   with a commit after each run, as the
   [templates README](templates/README.md#regenerate) lists. `kDictionaryUri`
   and `kTemplateHref` end up as raw GitHub URLs at a full 40 hex commit of
   this repo. A template href may be any immutable URL whose bytes hash to
   `template.sha256`, and this repo is the canonical host. A catalog MAY mirror
   the file at `templates/<board>/<template sha256>.json`.
5. Footer fixture and gate. The board contract test builds a real footer
   through `footer::build` and checks it with `tools/footer_contract.py`
   against the vendored stac-opensensor `footer.json` and the GeoParquet 2.0.0
   schema. `thing.board` must equal the template folder slug, and `dict` and
   `desc` must equal the board headers. `pixi run published-templates-test`
   and `pixi run pinned-uris-test` check the published copy and the pins.
6. Mobile. Add a newest-first entry to [mobile-migration.md](mobile-migration.md)
   with the new board, `dict` and `desc`, so the app can map its LIVE profile.
7. Uploader prefix. A dev upload goes to
   `<product>/dev/opensensor-space-firmware/<board>/<archive name>`. The
   `<board>` segment is whatever slug the board passes, and no code checks it
   against `kBoard`. A prod upload goes to the intake prefix the owner sets.
   The [uploader software](telemetry-pipeline.md#uploader-software-129) section
   has the exact rules.
8. STAC writer duty. Since OD-2026-11-04-D
   ([decision 0001](decisions/0001-standalone-thing-stac.md)) the board writes
   STAC. At finalize it writes a File Item sidecar `<file>.json` next to each
   dated `data_HHMM_` file, with the data href `./<file>.parquet`. It writes
   the Thing Collection from its own upload ledger and is the only writer under
   `thing=<uuid>/`. Owner only fields come from setup config. The shared writer
   is not built yet. The File Item is [#142](https://github.com/walkthru-earth/opensensor-space-firmware/issues/142) and the Thing Collection is
   [#145](https://github.com/walkthru-earth/opensensor-space-firmware/issues/145). It follows the stac-opensensor PROFILE Writers and
   Canonical form sections. When it lands, a new board measures its flash,
   internal RAM and PSRAM cost on that board. Sidecar transport is future
   work, over the uploader in [#144](https://github.com/walkthru-earth/opensensor-space-firmware/issues/144) and over BLE and LAN in [#143](https://github.com/walkthru-earth/opensensor-space-firmware/issues/143). Today
   both accept only `.parquet` names.
9. Builds. Build both existing boards and the new one, `pixi run cores3-build`,
   `pixi run waveshare-build` and the new board task, then run `pixi run gate`.
