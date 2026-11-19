# Optional STAC File Item builder

`src/os_stac_item.*` builds the STAC File Item a board writes as `<file>.json`
next to each finalized dated Parquet file (#142). It follows the canonical form
of stac-opensensor commit `204365f929e6f77bcaa6e70a0245c96cadc5c115`
(`PROFILE.md` "Canonical File Item" and `scripts/canonical.mjs` `buildItem`).
That `buildItem` adds two no Item rules. The rotation interval must be 600,
900, 1800 or 3600 seconds, and the HHMM of the file name must be the start of
the window that holds the earliest row.

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
  copied fixtures in `tools/fixtures/stac/`, and valid against the pinned
  `profile.json` and extension `schema.json`. A new profile commit means copying
  the fixtures again and regenerating `cases.json`, never editing them here.
- The same gate pins the device only sidecar lines of `os_logger.cpp` with
  source checks and listed mutants. These are the SHA-256 feed from the CRC
  read, the digest finish after the Parquet rename, the `O_EXCL` partial and
  the check that never replaces a `.json`. Editing those lines means editing
  the patterns in `tools/test_stac_item.py` in the same change.
