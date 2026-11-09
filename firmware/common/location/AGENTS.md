# Optional native H3 location component

The H3 work lives in the pinned library
[walkthru-earth/embedded-h3-location](https://github.com/walkthru-earth/embedded-h3-location)
v2.0.0, fetched by the component manager from `idf_component.yml`. This
component keeps only the firmware policy over it. `src/os_location.h` holds the
res 8 rule and `src/os_h3_alloc.cpp` holds the H3 allocator hooks. Nothing here
initializes controllers, reads GPS, performs network lookups, writes NVS or
infers country. Missing location remains missing.

- Never edit, patch or copy the library into this repository. Bump it by
  changing the tag in `idf_component.yml`, rebuilding every board so each
  `dependencies.lock` records the new commit, and checking that
  `tools/h3_library.py` still finds one agreed commit. The library verifies
  its vendored H3 bytes against `vendor/h3/manifest.json` at configure time and
  the host tests repeat that check.
- Privacy coarsening calls H3 `cellToParent` through the library, never invents
  bit arithmetic or a geographic replacement. A cell coarser than res 8 cannot
  be upsampled, so `published_cell()` refuses it with `Error::TooCoarse`.
- Persist and publish only the output of `published_cell()`, always res 8
  (`kPublishedResolution`, #139). The library sets no resolution limit, so the
  firmware passes this cap explicitly on every call. Center values come from
  that same cell and are approximate deployment geometry, not retained GPS
  coordinates.
- `from_degrees()` indexes a raw position straight at the requested
  resolution. Only `firmware/common/gnss` (`to_fix()`) calls it, at res 8,
  and raw degrees never leave that call.
- `location.phone_cell` (source 3) goes through `published_cell()` like
  `location.cell` and lives in RAM only, never in NVS (#140). The runtime
  drops it `config::kPhoneCellMaxAgeUs` (300 s) after the last SET of the key.
  Rules are in `docs/shared/time-and-location.md`.
- Country is optional, owner-declared ISO 3166-1 alpha-2. The library's
  `valid_country()` checks the 249 assigned codes. Never claim H3 provides
  country boundaries.
- `cells_to_multipolygon()` traces a cell set into caller buffers through H3's
  `cellsToMultiPolygon`. It is the one H3 call that allocates. Each board sets
  `CONFIG_EMBEDDED_H3_LOCATION_ALLOC_PREFIX="os_h3_"` in its
  `sdkconfig.defaults`, and `src/os_h3_alloc.cpp` defines the four hooks. They
  send every H3 allocation to PSRAM, with no internal RAM fallback, and pass
  every size, both calloc arguments included, through `nonzero_alloc_size()`
  so the zero byte request on the whole globe path does not read as out of
  memory. A `static_assert` fails the build when the prefix is anything else.
  The component links `WHOLE_ARCHIVE` because only the library archive, which
  links after it, calls the hooks. Call it only at finalize in the storage
  worker. `pixi run h3-polygon-test` proves cleanup on every failed
  allocation, the globe through the rounding, bit equality with
  `cellsToLinkedMultiPolygon` and prints the 720 cell heap.
- Host gates build the library at the commit every board lock names, through
  `tools/h3_library.py`, with the pinned Pixi toolchain. They prove indexing,
  validation and privacy logic, not board GPS, persistent storage or hardware
  behavior.
