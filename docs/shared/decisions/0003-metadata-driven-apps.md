# 0003 Metadata-driven apps

| | |
| --- | --- |
| Status | Decided 2026-11-17 by the owner. Hints and the setup manifest built in plugin contract 0.1.7 (#184). The setup record and the board settings page are not built |
| Date | 2026-11-17 |
| Issue | [#17](https://github.com/walkthru-earth/opensensor-space-firmware/issues/17), mobile [#219](https://github.com/walkthru-earth/opensensor-space-mobile/issues/219) |
| Related | [0001](0001-standalone-thing-stac.md), [plugin contract](../plugins/README.md), [sync protocol](../ble-sync-protocol.md), [STAC contract](../stac-contract/README.md) |
| Spec | stac-opensensor `PROFILE.md` at [`f423805a55a0a69e859287036f5325ee1b3c1551`](https://github.com/walkthru-earth/stac-opensensor/blob/f423805a55a0a69e859287036f5325ee1b3c1551/PROFILE.md) lines 43 to 47 and 352, which this record reverses |

## Context

A new board needs app code today before the phone shows it well. The app
keeps a reviewed LIVE profile per `dict` digest, labels and scales by column
name, and a fixed setup wizard and Device settings order. The CrowPanel
(#182) showed the cost. Its template is verified and complete, yet the app
shows no LIVE for it and its panel settings are reachable only from the knob.

## Decision

1. **Display hints are published.** A board template may carry, per column,
   `title` with translations, `description`, `opensensor:decimals`,
   `opensensor:group` and a default `opensensor:scale`, and per template a
   main metric `opensensor:primary` and the scale definitions in
   `opensensor:scales`. Core `title` and `description` stay plain strings in
   the default language, and other languages go in `opensensor:title_i18n`
   and `opensensor:description_i18n`, maps keyed by BCP 47 tag.
   stac-opensensor adds a shared property registry, `registry/properties.json`
   keyed by the `opensensor:property` URI with label, short label,
   definition, group, default decimals and default scale, and
   `registry/scales.json`, in the spirit of STAC Browser's
   `@radiantearth/stac-fields`. The template value wins over the registry, in
   the order below. The app profile keyed by `dict` goes away.
2. **LIVE for any verified template.** A phone may show LIVE for any template
   it has verified as the sync protocol describes, the full SHA-256 matching
   the footer `template.sha256` or the template link `file:checksum`, and
   starting with the 16 hex `desc` the device serves. `desc` alone never
   verifies. `opensensor:plausible_range` and `opensensor:valid_when` still
   filter each value, and a value whose gate column has no LIVE key is not
   shown. There is no reviewed digest list.
3. **The board declares setup and settings.** The board `board.json` lists
   ordered setup steps, ordered settings sections and typed `config_keys`.
   The generator copies them into the template `firmware` block, so they
   are hashed with it and a change moves `desc`, never `dict`. The app keeps
   a fixed catalog of setup steps (name, location, mobility, site, wifi,
   pairing, lan), of settings sections (location, site, wifi, pairing, lan,
   bonds, sensor) and a generic form. A board's own step or
   section carries a label. Find, Pair, Permission and Done
   stay app owned. The device stores which steps are done. Board specific
   settings that live only on the device today, such as the CrowPanel panel
   and SEN63C options in NVS `ospanel`, get `SET_CONFIG` keys on a new
   `GET_CONFIG` page. The NVS namespace and its keys stay as they are. Setup
   is local. It lives in BLE and the template, never in a STAC Collection or
   Item. The template declares names and types, never values.
4. **App safety floors stay in the app.** No manifest can lower them.
   Location is required for a fixed Thing. PIN, Wi-Fi PSK and LAN token are
   never declared, echoed or logged, and the app keeps its own secret key
   list. Reboot, token rotation, forget bonds and pairing mode `none` keep
   their confirmations. Debug cannot send `REBOOT` or `SET_CONFIG`. The silent
   clock, token and `lan.on` actions stay hidden below the Developer level.

## Resolution order

| Hint | First match wins |
| --- | --- |
| Label | Column `opensensor:title_i18n` for the language, column `title` (owner config over template), registry label for the property, then the column name with `_` read as spaces |
| Decimals | Column `opensensor:decimals`, registry default, then 0 for an integer column and 2 otherwise |
| Scale | Column `opensensor:scale`, registry default, then none |
| Primary | Collection `opensensor:primary`, template `opensensor:primary`, the first `physicalMeasurement` column with a scale, then the first `physicalMeasurement` column |

`opensensor:group` is one of `particulate`, `gas`, `climate`, `light`,
`motion`, `location`, `power`, `time` or `device`.

## Shapes

The column hints, the template hints and the setup, settings and config key
manifest are built ([#184](https://github.com/walkthru-earth/opensensor-space-firmware/issues/184),
plugin contract 0.1.7). So are the setup record and the board settings page,
and the CrowPanel declares its `config_keys`. The
[sync protocol](../ble-sync-protocol.md#setup-record) has the device side and the
[plugin contract](../plugins/README.md#setup-settings-and-config-keys) has
the gate rules.

| Part | Where | Shape |
| --- | --- | --- |
| Column hints | Module manifest column, template `table:columns` | `title`, `opensensor:title_i18n`, `description`, `opensensor:description_i18n`, `opensensor:decimals` (on every column, authored or else `firmware.decimals`), `opensensor:group`, `opensensor:scale`, `classification:bitfields` on a `bitmask` column, and class `title`, `description` and `color_hint` |
| Template hints | `board.json`, template top level | `opensensor:primary` names one `physicalMeasurement` column, `opensensor:scales` holds the registry scales its columns name |
| Setup steps | `board.json` `setup`, template `firmware.setup` | Ordered `[{"id", "req", "keys"}]`, 1 to 31 steps. `id` names a step in the app catalog, which lists no `keys`, or a board step, which must. `req` marks a step setup cannot finish without |
| Settings sections | `board.json` `settings`, template `firmware.settings` | Ordered `[{"id", "keys"}]`. `id` names a section in the app settings catalog, which is separate from the setup catalog, or a board section, which must list `keys`. `name` and `mobility` are setup steps only, `bonds` and `sensor` are settings sections only |
| Config keys | `board.json` `config_keys`, template `firmware.config_keys` | Map from `SET_CONFIG` key to `type` (`boolean`, `integer`, `enum` or `text`), `values` (an enum list, or a range `{"min", "max"}` that an integer needs), `applies` (`immediately`, `next sample`, `next boot` or `action`), `page` 7, a plain `label` and an optional `label_i18n` map of the same shape as `opensensor:title_i18n`. Never a value, never a secret key. A decimal is a scaled integer |
| Setup record | `GET_CONFIG` page 1 `thing`, NVS `setup` in `oscfg` | `setup_ack`, a bitmask in `firmware.setup` order from 0 to 2147483647, and `setup_v`, the first four bytes of SHA-256 over the ordered step ids joined by `\n`, read big endian with the top bit cleared, 0 read as 1, and `0x63b0c442` for the empty list. The phone writes both in one `SET_CONFIG`. A different `setup_v` reads as no step done |
| Board settings | `GET_CONFIG` page 7 | The board specific keys from `config_keys`, a JSON object nested by key prefix with booleans as `true` or `false`, within the 480 byte page limit. No key is a dotted prefix of another, so `panel.display` and `panel.display.x` cannot both exist, and every key maps to one leaf |

A step is done when its `setup_ack` bit is set. A required step whose result
the config proves is derived from the config instead, as the advert `setup`
bit is today. The required `location` step is done when the Thing is fixed
and has a cell, or when `thing.mobility` is `moving`. A moving Thing finishes
it by choosing moving in the `mobility` step, with no cell. Setup cannot
finish with a fixed Thing that has no cell. An unknown step id
renders a generic form from its typed `config_keys`. Without keys it is
skipped, and an unknown required step leaves setup pending.

## Order

Clean state first, then the contracts in this repo and stac-opensensor, then
the mobile refactor. There is no backward compatibility. Firmware and app
move in lockstep (owner decision 14). Portolan adoption comes later.

## Open questions for the owner

1. **CrowPanel air bands.** The CrowPanel screen colours PM2.5 with the US
   EPA 2024 AQI bands in
   `firmware/esp-idf-crowpanel-epaper/display/air_band.h`, applied to the
   live value. The stac-opensensor scales and the app use WHO 2021 guideline
   and interim targets, and each scale now carries a `period`, `P1D` for the
   WHO PM2.5 and PM10 scales, so neither is meant for a single live value.
   Pick one for the panel, or publish both as scales.
   No display code changes until then.
2. Translation shape. Resolved, the generator follows stac-opensensor
   [#16](https://github.com/walkthru-earth/stac-opensensor/pull/16).
3. Board settings page and `setup_v`. Resolved as page 7 and the rule in the
   table above. The CrowPanel keys are `panel.*` and `sen6x.*`, listed in
   the [sync protocol](../ble-sync-protocol.md#board-keys-page-7).
4. A CO2 scale is missing. The registry has a PM4 label. Bitfields are
   resolved, the generator emits them for `sen6x_device_status`,
   `imu_fresh_mask` and `gauge_status_flags`.
5. CoreS3 `als` is gated on `light_status`. Resolved, `light_status` is LIVE
   as `lst` from dictionary `cores3-telemetry-v8`, so a phone can check the
   gate and show `als`.

## What would reopen it

A template the phone cannot verify, a display hint that changes a stored
value, or a manifest that needs to weaken a safety floor.
