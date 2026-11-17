# Published board templates

Each board publishes its stac-opensensor v0.1.0 board template here, one file
per board for the current dictionary digest (`dict`).

| Board | Template |
| --- | --- |
| M5Stack CoreS3 | `m5stack-cores3/<dict sha256>.json` |
| Waveshare ESP32-S3 SIM7670G | `waveshare-esp32-s3-sim7670g/<dict sha256>.json` |
| ELECROW CrowPanel ESP32-S3 4.2" E-paper with SEN63C | `elecrow-crowpanel-epaper-42/<dict sha256>.json` |

[`index.json`](index.json) holds two maps, one entry per board in each.
`templates` maps each `dict` to its board, the firmware schema name, the
template `href` (relative to this folder) and `desc`. `by_desc` maps each
`desc` to its board, `dict` and `href`, so a reader that resolves by `desc`
needs no scan.

## Byte exact

A template file holds exactly the `kTemplateJson` bytes the board embeds from
its generated `telemetry_template.h`. It is minified ASCII JSON with no final
newline. `desc` is the SHA-256 of these exact bytes, so the SHA-256 of the
file equals `desc` in the board `telemetry_descriptor_digest.h`. Never
reformat, pretty print or re-serialize a file, and never add a newline.
`.gitattributes` marks these files `-text` so git never touches their line
endings.

Only the current `dict` of each board is published. Older digests are dropped
(no backward compatibility, owner decision 14). Code values live in each
template as `classification:classes`, generated from `telemetry_classes.inc`.

## Pinned URLs

Each template points at its field table with a raw GitHub URL at a full 40 hex
commit, and `kTemplateHref` names the template the same way (#89).

| Constant in `telemetry_contract.h` | Names | Bytes hash to |
| --- | --- | --- |
| `kDictionaryUri`, footer `dictionary.uri` and the template describedby link | `<board firmware>/telemetry_fields.inc` | `dict` |
| `kTemplateHref`, the footer template href from #88 | `docs/shared/templates/<board>/<dict>.json` | `desc` |

A `main` or branch URL moves, so the bytes behind it can stop matching the
digest. `pixi run pinned-uris-test` fails on such a URL, on a commit that is not
an ancestor of HEAD and on a commit whose bytes miss the digest. It needs full
history, CI checks out with `fetch-depth: 0`.

`kTemplateHref` sits outside the template, so pinning it never moves `desc`.
`kDictionaryUri` sits inside it, so pinning it moves `desc`. A commit URL cannot
name the commit it is in, so the pins always point at an earlier commit of the
same PR.

PRs that move dict or desc land with a merge commit, everything else squashes.
A squash or rebase merge drops the pinned commit from main and the gate fails.

## Mobile

The app copies these files verbatim into its bundled templates. Its template
index resolves a board by `desc` first, then by `dict`. Hash the bytes as
copied, never a parsed and re-serialized copy.

## Regenerate

The files come from the board headers. The command that writes them is the
board contract test with `--write-template`.

```sh
pixi run python tools/test_telemetry_contract.py --write-template
pixi run python tools/test_waveshare_contract.py --write-template
```

Each command rewrites `telemetry_template.h` and
`telemetry_descriptor_digest.h` of its board, then publishes the current
headers of every board, removes anything else under this folder but this
README and rewrites `index.json`.

A change to `telemetry_semantics.inc` or `telemetry_classes.inc` keeps `dict`
and moves `desc`. Run the command for that board, commit the result as the
content commit, then run `pixi run pin-uris` and commit the new `kTemplateHref`
as the pin commit.

A change to `telemetry_fields.inc` moves `dict`, and the command does not update
the dictionary digest or the LIVE fixture, so do these steps in order.

1. Edit `telemetry_fields.inc`, and bump `kDictionaryVersion` and `kFirmware`
   in `telemetry_contract.h` as the root `AGENTS.md` requires. Bump
   `kSchemaName` too when columns, types or annotations change, which a field
   table edit usually does.
2. Set `kDictionarySha256` in `telemetry_dictionary_digest.h` to the output of
   `shasum -a 256 telemetry_fields.inc`. The contract test refuses a stale
   digest before it writes anything.
3. In `tools/fixtures/live_keys/<board>.json` (`cores3` or `waveshare`),
   replace the old digest entry with one for the new digest. Each build in it
   holds the new firmware identity, its `emitter`, its `payload` and its LIVE
   key to field map in field order.
4. Commit the field table, the digest and the fixture.
5. Run `pixi run pin-uris`. It pins `kDictionaryUri` to that commit and stops,
   because the template embeds the URL. It also points `kTemplateHref` at the
   new `<dict>.json` name under its old commit, so the contract static_assert
   holds and the next step builds. Do not commit this step on its own.
6. Run the board command above. It publishes the template under the new
   `dict` name with the new describedby link and removes the file for the old
   one. Commit this as the content commit.
7. Run `pixi run pin-uris` again. It pins `kTemplateHref` to the content
   commit. Commit this as the pin commit. A third run prints
   `pins already current`.
8. Run the board contract test without the flag,
   `pixi run published-templates-test` and `pixi run pinned-uris-test`, then
   `pixi run gate`.
9. Add a newest-first entry to [mobile-migration.md](../mobile-migration.md)
   with the new `dict` and `desc`.
10. Land the PR with a merge commit, never squash or rebase. After a rebase,
    run `pixi run pin-uris` again so the pins name the rebased commits.
