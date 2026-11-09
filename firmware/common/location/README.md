# Native location helpers

This optional ESP-IDF `location` component is the firmware policy over
[walkthru-earth/embedded-h3-location
v2.0.0](https://github.com/walkthru-earth/embedded-h3-location/releases/tag/v2.0.0),
commit `49753cb9a8795ff1ae6bc6eed461fee0fc30cdc7`. The library wraps [official
Uber H3 v4.5.0](https://github.com/uber/h3/releases/tag/v4.5.0), commit
`1b536c34225191ba24a75a840f634d4a48c3b206`, vendored unmodified with its
Apache-2.0 license and NOTICE and checked by SHA-256 when it configures.
`idf_component.yml` pins the tag and each board `dependencies.lock` records the
commit. Host tests build the same commit through `tools/h3_library.py`, which
fails when the locks disagree. Unused H3 functions are removed by linker
section collection.

What stays in this repository

- `src/os_location.h` re-exports the library names the firmware uses under
  `opensensor::location` and adds the res 8 rule.
- `src/os_h3_alloc.cpp` defines the `os_h3_` allocator hooks for H3.

Every published cell is H3 resolution 8, `kPublishedResolution` (#139). The
library has no default resolution, so the firmware passes the cap on every
call. `opensensor::location::published_cell` returns the resolution 8 parent of
a valid cell at resolution 8 or finer through the library's `parent_at`. It
returns `Error::TooCoarse` for a cell coarser than 8, which cannot be restored,
and `Error::InvalidCell` for anything that is not a valid cell. Persist and
publish only its output, and calculate the published center using
`center_degrees` or `center_e7`, which rounds to 10⁻⁷ degrees. `coarsen` is the
general library helper, it takes any target resolution 0 to 15 and leaves a
coarser input unchanged. Neither coordinates nor the original finer cell are
retained by these stateless helpers. Every fallible call returns
`std::expected`.

Cell text is 15 hex digits, emitted lowercase. No numeric prefixes or trailing
content are accepted. `from_degrees` accepts finite latitude ±90° and longitude
±180°. Exact ±180° endpoints are equivalent. Missing or invalid data is never
converted into a default geographic location.

Country is optional and owner-declared. The library's `valid_country` accepts
the 249 assigned uppercase ISO 3166-1 alpha-2 codes. H3 identifies cells and
does not supply an administrative country map. A cell can straddle boundaries,
and its center is not evidence of a declared country.

H3 allocates only inside `cells_to_multipolygon`. Each board sets
`CONFIG_EMBEDDED_H3_LOCATION_ALLOC_PREFIX="os_h3_"` in its `sdkconfig.defaults`,
so H3 calls `os_h3_malloc`, `os_h3_calloc`, `os_h3_realloc` and `os_h3_free`.
The hooks allocate from PSRAM with no internal RAM fallback and round every
zero byte request up to one byte, because ESP-IDF returns NULL for
`malloc(0)` and H3 asks for zero bytes on the whole globe path.
