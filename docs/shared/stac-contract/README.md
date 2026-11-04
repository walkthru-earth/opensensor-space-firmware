# STAC metadata contract research

Research snapshot from 2026-10-25 at commit `2b49114`. Nothing here is implemented. It records why and how the firmware could publish a STAC description of its columns, so the mobile app and host tools stop being air quality only.

| File | Content |
| --- | --- |
| [firmware-audit.md](firmware-audit.md) | Where per-column metadata lives, what reaches the phone, footer and LIVE limits, a STAC emission proposal with flash and RAM cost, and which AQ names are wire contract |
| [firmware-audit-review.md](firmware-audit-review.md) | Independent line-by-line check of the audit. Its corrections win where the two disagree, for example footer slot use, descriptor size, request per chunk transfer and unit systems |

The app-side design is `docs/superpowers/specs/2026-10-25-sensor-agnostic-stac-contract.md` in walkthru-earth/opensensor-space-mobile, tracked by walkthru-earth/opensensor-space-mobile#182. Firmware work is tracked by epic #83 and issues #84 to #93.

Who writes the STAC Items, Collections and Catalog is settled in [decision 0001](../decisions/0001-standalone-thing-stac.md) (#109). Since 2026-11-04 the board writes the File Item and the Thing Collection, and only the catalog host writes the root Catalog.

When any part lands, move the settled contract into `table-and-observation-model.md` and `ble-sync-protocol.md`, and keep this folder as history.

## Field names are superseded

The two audits are dated and propose draft field names. Their findings still stand, but the names were replaced by the released extension [stac-opensensor v0.1.0](https://github.com/walkthru-earth/stac-opensensor). Read the audits for evidence and use the extension for names.

v0.1.0 splits the metadata into three layers, as its [PROFILE.md](https://github.com/walkthru-earth/stac-opensensor/blob/83787768cc8451109d48a03d4ef5493ffcd403dd/PROFILE.md) defines.

1. Board template. The firmware build generates one per schema, plain JSON with `"type": "OpenSensorTemplate"`, not STAC and not a profile Collection. It holds the public columns in `item_assets.data.table:columns` (core `unit`, core `roles` and the `opensensor:` column fields, no `title` or `opensensor:scale`), `instruments`, `opensensor:sample_interval_s`, and a `firmware` block with `schema`, `proto_min`, `dict`, `live_keys` and `unit_labels`. Its SHA-256 is the `desc` digest and `GET_DESCRIPTOR` streams it. The dictionary digest reaches it as `file:checksum` on a `describedby` link to `telemetry_fields.inc`. There is no `cube:dimensions`.
2. Thing Collection. The board writes it since [decision 0001](../decisions/0001-standalone-thing-stac.md). It copies the template columns and adds extent, license and a second `describedby` link to the template with `file:checksum` set to `1220` plus `desc`. Column titles and scales are optional catalog host additions.
3. App profile. `primary`, `hero`, `family`, `display_step` and column labels live in an app file keyed by the dictionary digest, outside any published schema.

| Draft name in the audits | v0.1.0 |
| --- | --- |
| `opensensor:ucum`, or a UDUNITS and UCUM pair | Core `unit` with a UCUM code only |
| `opensensor:unit_label` | Board template `firmware.unit_labels` |
| `opensensor:live_key` | Board template `firmware.live_keys` |
| `opensensor:validity` | `opensensor:valid_when`, a gate `{column, equals}` or `{column, finite: true}` |
| `opensensor:aggregation` (`instant`, `mean`, `counter`, `state`) | `opensensor:aggregate` with CF words `mean`, `minimum`, `maximum`, `sum`, `point`, `mode` |
| `opensensor:valid_range` | `opensensor:plausible_range`, values outside are flagged, not masked |
| `opensensor:display` scale and bands | `opensensor:scales` on the Collection, `opensensor:scale` on the column |
| `opensensor:labels`, `title_ar` | Column `title` plus app profile labels, scale step labels in `opensensor:scales` |
| `opensensor:precision` | App profile `display_step` |
| `opensensor:codes`, or a `bands` entry per code column | `classification:classes` in the column |
| `opensensor:sensor` (vendor, model) | `opensensor:procedure` on the column and core `instruments` on the Collection |
| `opensensor:station_id` on Items | One Collection per Thing UUID, one Item per file. The footer names it `thing.id` (#83) |
| `opensensor:calibration` | Not in v0.1.0 |
| Role values (measurement, quality, state and others) | Core `roles` with ACDD values `physicalMeasurement`, `auxiliaryInformation`, `qualityInformation`, `referenceInformation`, `coordinate`, and `modelResult` for derived columns |
| `cube:dimensions`, `cube:variables` | Not used. Cadence is `opensensor:sample_interval_s` |
| Shared `urn:opensensor:property:<id>` | `opensensor:property` with a shared URI such as NERC P07 where one fits, otherwise a per board URI |
| Footer `stac:collection_href`, `stac:descriptor_sha256`, `urn:opensensor:collection:` | One `opensensor` footer key whose `template` group holds `href` and `sha256`, per stac-opensensor `footer.json` v0.1.0 (now commit `4408c4e52366bb3d154d22bbfffc88ff09aed6f1`, #88, #83) |
| `opensensor:field_id`, `opensensor:procedure`, `opensensor:property` | Unchanged |

`opensensor:settle_s` and `opensensor:derived` are new in v0.1.0. Upstream proposals may move some fields later, see stac-extensions/table#16 (`field_id`), table#17 (common metadata in columns), classification#55 (classes in table columns) and cf#11 (CF fields in table columns).

## Owner decisions after the audit (2026-10-26)

The owner decided these points after both audits were written. They supersede the matching parts of the audits and of the correction list in `firmware-audit.md`. Epic #83 holds the full decision.

- There is no backward compatibility for now. Released firmware, app builds and old archives do not have to keep working. Contract tests and digest gates stay. #86 publishes the current digest per board only, so legacy digests and the #31 ambiguity are dropped. This supersedes the backward compatibility goal on line 5 of `firmware-audit.md`.
- Property ids become board neutral (#84). Use a shared vocabulary first, such as NERC P07 or CF, and otherwise `urn:walkthru-earth:property:<quantity>`. Board and sensor go in `opensensor:procedure`. This supersedes correction item 8 and the per board URI row in the table above.
- Every declared LIVE key is always present, with a number or null (#91). Per field precision lands in #91 with a new dictionary digest. An absent key means the board has no such field. Correction item 11 and its "display hint only" reasoning no longer hold. So do "omits invalid values" in the LIVE paragraph of `firmware-audit.md` Q2 and review E2 in `firmware-audit-review.md` ("absent means invalid or unrepresentable").
- The BLE name prefix becomes `OS-` and the mDNS host becomes `os-xxxx` (#90). Other internal and wire AQ names are renamed in #93. The audit's list of AQ names that are wire contract is history.
- The footer keys were frozen as `opensensor:template_href` and `opensensor:template_sha256` (#88). The owner then superseded this with stac-opensensor a56dfb0. Each file the firmware finalizes now has one `opensensor` key holding versioned JSON, and the template link and digest sit in its `template` group. A partial file rebuilt by `tools/rebuild_partial.py` has no `opensensor` key (see the #88 entry in `../mobile-migration.md`).
- Station becomes Thing with a required `mobility`, and the location moves to a
  native `geometry` column with a `geo` footer key (#83, bridge decisions 31 to
  35). The firmware follows stac-opensensor commit `4408c4e52366bb3d154d22bbfffc88ff09aed6f1`. See the
  Thing entry in `../mobile-migration.md`.
