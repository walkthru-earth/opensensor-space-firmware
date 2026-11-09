# Native location helpers

This optional ESP-IDF `location` component wraps [official Uber H3
v4.5.0](https://github.com/uber/h3/releases/tag/v4.5.0), commit
`1b536c34225191ba24a75a840f634d4a48c3b206`. Vendored core files, Apache-2.0
license and NOTICE are unmodified and checked by SHA-256 in both builds and
host tests. The public header is generated from the upstream template with
version 4.5.0; unused H3 functions are removed by linker section collection.

Every published cell is H3 resolution 8, `kPublishedResolution` (#139).
`opensensor::location::published_cell` returns the resolution 8 parent of a
valid cell at resolution 8 or finer. It returns `PublishError::TooCoarse` for a
cell coarser than 8, which cannot be restored, and `PublishError::Invalid` for
anything that is not a valid cell. Persist and publish only its output, and
calculate the published center using `center_degrees` or `center_e7`, which
rounds to 10⁻⁷ degrees. `coarsen` is the general helper underneath, it takes
any target resolution 0 to 15 and leaves a coarser input unchanged. Neither
coordinates nor the original finer cell are retained by these stateless
helpers.

Cell text is 15 hex digits, emitted lowercase; no numeric prefixes or trailing
content are accepted. `from_degrees` accepts finite latitude ±90° and longitude
±180°. Exact ±180° endpoints are equivalent; missing or invalid data is never
converted into a default geographic location.

Country is optional and owner-declared. `valid_country` accepts the 249 assigned
uppercase ISO 3166-1 alpha-2 codes in the 2026-10-09 snapshot, using the [UN M49
ISO-alpha2 table](https://unstats.un.org/unsd/methodology/m49/overview/) and ISO's
[TW assignment](https://www.iso.org/obp/ui/#iso:code:3166:TW). [ISO documents the
assigned code set and distinguishes user-assigned/reserved codes](https://www.iso.org/iso-3166-country-codes.html).
H3 identifies cells, and does not supply an administrative country map. A cell
can straddle boundaries; its center is not evidence of a declared country.
