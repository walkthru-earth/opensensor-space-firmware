# Plugin contract changelog

Newest first. Versions follow section 7 of the [README](README.md).

## 0.1.5, 2026-11-09

- The board schema gains an optional `tooling` object (#141). It carries
  the host tool facts of a board, the short alias, the app variants with the
  `board.json` folder first, the backup prefixes, the 16 MiB flash size, the
  PSRAM mode, required sdkconfig lines and the required and forbidden build
  components. `tools/boards.py` requires it on every
  `firmware/esp-idf-*/*/board.json` and fails closed when it is missing or
  wrong. Both boards set it.
- A new optional field is a patch under section 7. No `dict`, `desc`,
  `shipped.lock` or row moves.

## 0.1.4, 2026-10-27

- Step 5 of section 11. The Waveshare logger collect runs a `static
  constexpr` module table of `pms5003t`, the waveshare identity module,
  `max17048` and `battery_health`, in
  `firmware/esp-idf-waveshare-sim7670g/logger/sensor_modules.h`. The logger
  and `Hooks::collect` keep their signatures.
- `os_sensor_module.h` adds `ossensor::Writer`, tied to one module's
  `Column` enum, with typed writes checked against the board field type at
  build time, runtime writes that skip
  `kNoSlot`, and a `[[nodiscard]]` `init_module`. Section 10 documents them.
- The old `apply_pms`, `apply_gauge` and `apply_battery_health` leave the
  Waveshare contract header. They live on as the test reference in
  `tools/waveshare_collect_reference.h`, and a row equality fixture in
  `waveshare-contract-test` compares both.
- Under the 0.1.3 reserved id scan, `integer`, `counter`, `number` and
  `init_module` are now reserved module ids too.
- No schema change. No `dict`, `desc` or row moves.

## 0.1.3, 2026-10-27

- Step 7 of section 11. `pixi run plugin-new-board <board.json>`
  (`plugin_contract.py --write-board`) writes `telemetry_fields.inc`,
  `telemetry_semantics.inc`, `telemetry_classes.inc` and a first
  `shipped.lock` for a new board. It refuses when any of them exists, when
  the slug is taken, when a cross rule fails or when the path is not a
  `firmware/esp-idf-*/*/board.json`, and it writes nothing then. A failure
  mid write removes the files that run created.
  The files are frozen afterwards under Option A.
- `plugin-contract-test` writes the section 9 weather board into a temp
  copy and proves its rows and lock pass the gate. No real board is added.
- The orphan header scan covers headers directly under `modules/` and in a
  folder with no manifest left. `pixi run plugin-headers` removes an orphan
  only when it starts with the generated preamble, and fails on any other.
- Module ids that `os_sensor_module.h` declares in `ossensor`, such as
  `detail` and `resolve`, plus `ossensor`, `std`, `telemetry` and
  `oslogger`, are reserved.
- `sensor-module-test` resolves a module column at index 0.
- No schema change, so the schemas stay under `v0.1.0/`. No board file,
  header, lock, `dict` or `desc` moves.

## 0.1.2, 2026-10-27

- Step 4 of section 11. The gate generates one C++ header per module
  manifest, `<id>_module.h` beside its `module.json`, for the six shared
  modules and the five board local modules. `system` has none, because the
  logger fills its columns by name.
- Each header has `kProcedure`, a `Column` enum with the column names
  verbatim, `kColumns` in manifest order, a `Segment` enum and
  `kColumnSegments` with the segment of each column, `kStatusColumn` and the
  `kStatus` constants when the module names a status column, and an error
  only `InitError` when the manifest lists `init_errors`.
- The headers are checked in. `pixi run plugin-contract` fails on a
  missing, stale or orphan header, and `pixi run plugin-headers` rewrites
  them.
- `firmware/common/logger/src/os_sensor_module.h` adds `ossensor::Module`,
  `ossensor::init_error` and the `consteval` `ossensor::resolve`. A second
  `resolve` form takes the selected segments and maps columns of other
  segments to `ossensor::kNoSlot`, so a board can ship only `max17048.core`.
  No board includes it yet.
- `pixi run sensor-module-test` compiles the headers against both boards'
  field tables and proves a missing name stops the build.
- No schema change, so the schemas stay under `v0.1.0/`. No board file,
  `dict` or `desc` moves.

## 0.1.1, 2026-10-27

- Optional release `baseline` in the lock schema, with `reason`,
  `migration` and `segments`. A baseline is per segment. Such a release may
  edit only the shipped segments it lists by module and segment id, it must
  edit each one it lists, and every other shipped segment keeps the old
  rule. Its `migration` names a mobile migration notes heading that starts a
  line, and that entry names the release dict. A patch per section 7, so the
  schema stays under `v0.1.0/`.
- The Waveshare lock gains its first baseline release for the gauge
  validity token rename (#28).

## 0.1.0, 2026-10-27

- Module, board and lock schemas under `v0.1.0/`, with refs into the vendored
  stac-opensensor v0.1.0 schemas.
- `status` names the status column only. Its codes are that column's
  `classification:classes`.
- Board `class_subsets` lists the classes a board ships for a column.
- Examples `examples/bme280.module.json` and `examples/weather.board.json`.
- Manifests for the logger segments, six shared sensor modules, five board
  local modules and both boards. They reproduce today's CoreS3 and Waveshare
  board files byte for byte, so `dict` and `desc` do not move.
- `shipped.lock` per board at today's `dict`. A lock absent at the base
  branch prints a notice instead of passing silently.
- `tools/plugin_contract.py` and `pixi run plugin-contract-test` in
  `pixi run gate`.
