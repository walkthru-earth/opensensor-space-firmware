# STAC File Item fixtures

These files feed `pixi run stac-item-test` (#142). The test proves the board
STAC File Item builder in `firmware/common/stac` equal to the stac-opensensor
reference script, under that script's own `differences` rules. It also
validates every Item the board builds against the pinned `profile.json` and
extension `schema.json` in `tools/stac_opensensor/v0.1.0`.

## Source

Every file in `examples/`, `fixtures/`, `footers/` and `templates/` is a byte
copy from [walkthru-earth/stac-opensensor](https://github.com/walkthru-earth/stac-opensensor)
at commit `83787768cc8451109d48a03d4ef5493ffcd403dd`, at the same path.
Those files are CC-BY-4.0, Copyright (c) 2025-2026 Walkthru.Earth, the license of
that repository. Never edit them here. Copy them again from a new pinned commit.

`generate_cases.mjs` and the `cases.json` it writes belong to this repository,
also CC-BY-4.0 with Walkthru.Earth attribution. `cases.json` holds extra cases
built from the fixture files with a few edits, each with the result that
`scripts/canonical.mjs` `buildItem` gives for it. It also holds round2, rfc3339
and Item id results from the same script. Node is not part of the pixi
environment, so the results are generated once and checked in.

## Regenerate

Use a stac-opensensor checkout at the pinned commit, with `npm ci` run so that
h3-js 4.5.0 sits next to the script. From the repository root run this.

```sh
node tools/fixtures/stac/generate_cases.mjs \
  <stac-opensensor>/scripts/canonical.mjs tools/fixtures/stac \
  > tools/fixtures/stac/cases.json
```

The generator refuses any `canonical.mjs` other than the 83787768 bytes and
any h3-js other than 4.5.0. The output is the same bytes on every run.

## Manifest

`SHA256SUMS` lists the SHA-256 of every file here but this README and the
manifest itself. The test fails when a file changes, goes missing or is added
without a line. After a deliberate change, rewrite it from this folder.

```sh
find . -type f ! -name SHA256SUMS ! -name README.md | sed 's|^\./||' \
  | LC_ALL=C sort | xargs shasum -a 256 > SHA256SUMS
```
