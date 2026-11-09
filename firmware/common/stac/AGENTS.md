# Optional STAC File Item builder

`src/os_stac_item.*` builds the STAC File Item a board writes as `<file>.json`
next to each finalized dated Parquet file (#142). It follows the canonical form
of stac-opensensor commit `83787768cc8451109d48a03d4ef5493ffcd403dd`
(`PROFILE.md` "Canonical File Item" and `scripts/canonical.mjs` `buildItem`).

- Board neutral. It touches no storage, NVS, radio or board pins. A board opts
  in through `oslogger::Config::stac_columns`, a table built at compile time by
  `stat_columns()` from its FIELD and SEM tables. No board name appears here.
- The logger owns every file operation. It feeds rows through `Accumulator`,
  calls `ItemBuilder::prepare()` and then streams `ItemBuilder::write()` into a
  `json::Sink`. Never open, rename or delete a file from this component.
- Item geometry is the union of the resolution 8 cells of the file, traced by
  `location::cells_to_multipolygon()`. It allocates in PSRAM only, so call
  `prepare()` only at finalize in the storage worker.
- Cells in the Item come from the rows, which already hold `published_cell()`
  output (#139). Never index or coarsen a cell here.
- Numbers print through `json::format_real()` in `firmware/common/src/json_writer.h`.
  It places the shortest round trip digits of `std::to_chars` the way ECMAScript
  `Number::toString` does, so the text matches `JSON.stringify`. `round2()` and the
  sums must round each step as JavaScript does, so CMake builds this component
  with `-ffp-contract=off`.
- `pixi run stac-item-test` proves the output equal to the pinned script on the
  copied fixtures in `tools/fixtures/stac/`. A new profile commit means copying
  the fixtures again and regenerating `cases.json`, never editing them here.
