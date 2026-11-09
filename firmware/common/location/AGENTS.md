# Optional native H3 location component

`src/os_location.*` is board-neutral, stateless C++ wrapping the official H3 C
library. It does not initialize controllers, read GPS, perform network lookups,
write NVS or infer country. Missing location remains missing.

- Preserve the exact H3 version/tag/commit and all vendored bytes, license and
  notice under `vendor/h3/`; CMake and host tests verify `manifest.json` hashes.
- Privacy coarsening calls H3 `cellToParent`, never invents bit arithmetic or a
  geographic replacement. A cell coarser than res 8 cannot be upsampled, so
  `published_cell()` refuses it with `PublishError::TooCoarse`.
- Persist and publish only the output of `published_cell()`, always res 8
  (`kPublishedResolution`, #139). Center values come from that same
  cell and are approximate deployment geometry, not retained GPS coordinates.
- `from_degrees()` indexes a raw position straight at the requested
  resolution. Only `firmware/common/gnss` (`to_fix()`) calls it, at res 8,
  and raw degrees never leave that call.
- `location.phone_cell` (source 3) is reserved in
  `docs/shared/time-and-location.md` and not accepted yet (#140).
- Country is optional, owner-declared ISO 3166-1 alpha-2; validate assigned codes
  using the documented snapshot and never claim H3 provides country boundaries.
- Host gates compile actual C++ helpers and exact official C sources through
  pinned Pixi. They prove indexing/validation/privacy logic, not board GPS,
  persistent storage or hardware behavior.
