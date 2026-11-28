<!-- SPDX-License-Identifier: CC-BY-4.0. Copyright Walkthru.Earth. -->

# Waveshare ePaper-3.97 and SEN63C desk stand

A screwless printed desk stand for the Waveshare ESP32-S3-ePaper-3.97 with a
Sensirion SEN63C in the bottom of the case behind the board. The design, every
call and the owner print test are in [DESIGN.md](DESIGN.md). The board
measurements are in
[hardware/parts/boards/waveshare-epaper-397.md](../../parts/boards/waveshare-epaper-397.md)
and the sensor rules in
[hardware/parts/sensors/sen63c.md](../../parts/sensors/sen63c.md).

Three printed parts make the stand. The shell holds the board at 15 degrees
on four cups and the sensor in a sealed bay at the back wall. The bezel closes
the front with one top latch and two bottom tabs. The sensor bar slides in
through the side walls and holds the sensor on foam.

A fixed shelf inside the shell, behind the board, holds the LiPo pack that
ships with the board and an ML2032 RTC backup coin. The pack lies flat on four
ribs off the back wall, the coin stands in a slot to its right. The bezel and
bar are unchanged, only the shell needs a reprint for the shelf.

## Files

| File | What it does |
| --- | --- |
| `params.py` | Every number with its source tag, the frames and the `_checks` asserts |
| `vendor.py` | Loads the board and sensor vendor bodies and places them in the case frame |
| `shell.py` | One piece shell with the battery shelf, printed back wall down |
| `bezel.py` | Front bezel with the top latch arm and the two bottom tabs, printed face down |
| `sensor_bar.py` | Removable sensor bar with two barbed prongs, printed flat |
| `shapes.py` | Shared solid helpers and the pack and coin bodies |
| `slice.py` | Builds the A1 presets from the installed Bambu Studio profiles and slices every plate |
| `AGENTS.md` | Notes for an agent picking this design up, frames, commands and gotchas |
| `assembly.py` | Places the three parts, the pack and the coin around the vendor bodies and runs the CAD checks |

## How to run

Run every command from this folder. build123d only loads inside `cad run`.

```sh
pixi run -e cad python -I params.py
pixi run -e cad cad run shell.py
pixi run -e cad cad run bezel.py
pixi run -e cad cad run sensor_bar.py
pixi run -e cad cad run assembly.py
```

The first line runs the `params.py` asserts on their own. The part scripts and
the assembly need the vendor files below.

To make the print files, export each print pose model as STEP and 3MF with
`cad model export <id> <file>` (the id is printed by `cad run`, a 3MF takes
`--tolerance 0.01`), then slice with Bambu Studio installed.

```sh
python3 -I slice.py
```

Exports (STEP, 3MF and snapshots) go to the git ignored folder
`artifacts/enclosures/waveshare-epaper-397-sen63c/`.

## Vendor files

The vendor files stay out of git. `vendor.py` reads the `.xbf` copies from
`artifacts/enclosures/waveshare-epaper-397-sen63c/vendor/` and stops with an
error when one is missing. The STEP hashes below were checked on 2026-11-26.

To set the folder up again, work in an empty scratch folder and treat each
download as untrusted.

1. Download the Waveshare structure zip from
   https://files.waveshare.com/wiki/ESP32-S3-ePaper-3.97/ESP32-S3-EPAPER-3.97.zip
   and check its SHA-256 is
   `b792c9deff606aeaa739dcf0ad5cb8d15734ba88571e179c084bc17528596fc9`.
2. Unzip it and copy the three files into the vendor folder under the names in
   the table.
3. Download the SEN6x STEP and the example sealing foam STEP from the URLs in
   the SEN63C sheet, as `sen6x.step` and `sen6x-sealing-foam.step`.
4. Check each SHA-256 against the table. Stop if any differs, since a new
   vendor revision needs the part sheets checked again.
5. Convert each STEP to `.xbf` from the vendor folder.

```sh
pixi run -e cad cad step export waveshare-epaper-397.stp waveshare-epaper-397.xbf
pixi run -e cad cad step export sen6x.step sen6x.xbf
pixi run -e cad cad step export sen6x-sealing-foam.step sen6x-sealing-foam.xbf
```

| File in the vendor folder | Source | SHA-256 |
| --- | --- | --- |
| `waveshare-epaper-397.stp` | `ESP32-S3-EPAPER-3_97-20260821.stp` in the zip | a28e504d7a519a17988f713fbfd917ca8127e2310a577105ac29cf115fe36901 |
| `waveshare-epaper-397.dxf` | `ESP32-S3-EPAPER-3.97-20260821.dxf` in the zip | 1c2dbb3787fcee8dd27a630ca45899189bf5606cf2e63d6807a2f4364dffa316 |
| `waveshare-epaper-397-drawing.pdf` | `ESP32-S3-EPAPER-3.97-20260821.pdf` in the zip | 48b5c61dc3615aec4df61ff95de9b788bd775665d87b9792e2e0abcc31f152f2 |
| `sen6x.step` | Sensirion SEN6x STEP | f72f93285def36a74cb6219e817ebbf5b43f1aa73ec37b8804d92918a25e8ada |
| `sen6x-sealing-foam.step` | Sensirion example sealing foam STEP | f6905cd105c17c0623a879e74ec9908dba35d6b5a2fd80ec5d75379f2fde9557 |

The `.xbf` files are made from the STEP files, so their hashes are not
checked. The ones used on 2026-11-26 are listed in the board sheet.

## Check results

Run on 2026-11-26. The owner printed and fitted the shell, bezel and bar on
2026-11-26, before the battery shelf, then printed the plate with the shelf
and reported it good on 2026-11-28. The battery shelf test steps in DESIGN.md
are not recorded yet.

| Check | Result |
| --- | --- |
| `params.py` asserts | all pass |
| DFM shell (FDM, PLA) | OK, 59.3 cm³ with the shelf, thinnest wall 1.46, no overhang past 45 degrees, longest bridge 5.0. The support estimate counts only bridge ceilings and the hole floors under them. Two holes on the plate give an info note about elephant foot |
| DFM bezel | OK, 7.64 cm³, arm root strain 0.51 % against 1.2 % |
| DFM sensor bar | OK, barb catch 0.30 over the wall hole, prong strain 0.68 % against 1.2 % |
| Shelf fit | one solid, pack 61.5 x 41 x 4.0 and coin 20 x 3.2 fit with no clash, pack lead route 39.5 from the lower corner and 51.6 from mid edge against 55, coin lead route 65.3 |
| Interference | clear between our parts, the pack, the coin, the board and the sensor. One pair inside the board STEP could not be intersected by the kernel and is not ours |
| Assembled size | X -4.6 to 110.0, Y -6.6 to 46.0, Z -3.0 to 76.9 with the feet |
| Mass used | shell 68.0 g, bezel 8.2 g, bar 1.4 g from the slice, board 60 g, pack 28 g and coin 3 g ASSUMED, sensor 22 g, total 192.7 g |
| Centre of mass | (50.3, 21.4, 36.2), 39.2 over the desk |
| Tilt before tipping | forward 29.2, backward 25.3, left 49.7, right 52.8 degrees, rule is at least 10 |
| Push at the top edge that tips it | forward 0.52 N, backward 0.44 N, reported only |

The tipping push is below a 0.5 N light touch. The board has no touch screen
and its keys sit low on the chin, so this stays a report. DESIGN.md owner test
step 9 says what to do if it tips too easily.

The slice (Bambu Studio 02.09.02.62, A1 0.4, 0.16 mm, 3 walls, 15 % gyroid,
Generic PLA, no supports) is in DESIGN.md Print settings. The shell alone
takes 2 h 33 min and 68.0 g, all three parts on one plate 2 h 57 min and
77.5 g.

Files in `artifacts/enclosures/waveshare-epaper-397-sen63c/`.

| File | What it is |
| --- | --- |
| `shell_print.step`, `bezel_print.step`, `sensor_bar_print.step` | each part in its print pose |
| `shell_print.3mf`, `bezel_print.3mf`, `sensor_bar_print.3mf` | the same meshes for a slicer |
| `*.gcode.3mf` | sliced plates for the A1, ready to send, one per part |
| `shell_print.gcode.3mf` | the shell with the shelf alone, 2 h 33 min, 68.0 g, no supports, for a stand already printed |
| `all_parts.gcode.3mf` | all three parts on one plate, 2 h 57 min, 77.5 g, no supports, the one file for a new Thing |
| `assembly.step` | everything assembled with the vendor bodies, pack, coin, foam and feet |
| `slice/` | the presets `slice.py` built, the logs and results |
| `snapshots/` | assembly front, back and an x-ray front, shell front |

## Credits

The shape follows two MakerWorld models, as inspiration only. No geometry was
copied from either.

- Waveshare ESP32-S3 4.3 LCD stand with SEN66 sensor,
  https://makerworld.com/en/models/2605407
- Waveshare ESP32-S3-ePaper-3.97 back case,
  https://makerworld.com/en/models/3256990

## Licence

CC-BY-4.0, Walkthru.Earth. The vendor files keep their own terms and are not
part of this repo.

## Change log

- 2026-11-26, first model, not printed yet.
- 2026-11-26, DFM clean on all three parts, sliced without supports, tipping
  check on the sliced masses.
- 2026-11-26, owner printed and fitted the shell, bezel and bar, fit right.
- 2026-11-26, battery cradle added for the shipped LiPo pack and an ML2032
  RTC coin. No change to the other parts. DFM clean, sliced without supports
  or slicer warnings, not printed yet.
- 2026-11-26, the cradle is dropped before printing. The owner asked for a
  fixed shelf behind the board instead, and measured the pack 4.0 thick. The
  shell now carries four ribs, two fences and a coin slot. DFM clean, sliced
  without supports or slicer warnings, not printed yet.
- 2026-11-28, owner printed the three part plate with the shelf and reported
  it good. `slice.py` added so the plates can be made again from the scripts.
