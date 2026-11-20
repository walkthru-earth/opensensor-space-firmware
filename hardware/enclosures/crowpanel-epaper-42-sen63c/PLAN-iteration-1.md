<!-- SPDX-License-Identifier: CC-BY-4.0. Copyright Walkthru.Earth. -->

# CrowPanel 4.2 and SEN63C desk base, iteration 1 implementation plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Model, check and export the four printed parts of the approved
desk base (test plate, base body, backrest, bottom plate) as print ready
3MF files for the owner's first print.

**Architecture:** One `params.py` holds every shared number with a source or
ASSUMED comment. One build123d script per printed part exposes a `@model`
function. `assembly.py` places the parts around the vendor STEP bodies for
interference checks. The asserts in each script are the tests.

**Tech Stack:** text-to-cad `cad` 0.11.13 and build123d in the pixi `cad`
environment, Bambu Studio for slicing.

**Spec:** `hardware/enclosures/crowpanel-epaper-42-sen63c/DESIGN.md`. Rules in
`.claude/skills/hardware-design/SKILL.md`. Numbers in
`hardware/parts/boards/crowpanel-epaper-42.md` and
`hardware/parts/sensors/sen63c.md`.

## Global Constraints

- Every `cad` command runs as `pixi run -e cad cad <args>`, never a global copy.
- Vendor bodies are in `artifacts/enclosures/crowpanel-epaper-42-sen63c/vendor/`
  (ignored). SHA-256 checked 2026-11-19. Panel `crowpanel-4.2-epaper.stp`
  `4c02b3e7...4a20`, sensor `sen6x.step` `f72f9328...8ada`, foam
  `sen6x-sealing-foam.step` `f6905cd1...9557`. Never copy them into git.
- Exports go to `artifacts/enclosures/crowpanel-epaper-42-sen63c/iter1/`.
- Each new file starts with the CC-BY-4.0 Walkthru.Earth SPDX header.
- No em dashes anywhere. No screws, glue or inserts in any part.
- Material eSUN PLA+, Bambu Lab A1, 0.4 mm nozzle, 256 mm cube build volume.
- Snap strain limit 1.2 % in XY. Latch arms print flat.
- Base frame, all parts: X along the panel width, left to right as the
  viewer sees it (same as panel x). Y horizontal from the front of the base
  toward the back. Z up from the desk, Z = 0 on the desk.
- The panel frame from the board sheet maps into the base frame by a 15
  degree rotation back about X. Name it `panel_to_base` in `params.py`.
- Do not commit. The lead commits when the owner asks.

## Review Focus

1. Panel insertion with jumpers attached. The panel tips in about its bottom
   edge with 25 mm of jumper hanging from x 20.7 to 46.3. The header slot
   must be open from above for the whole tip arc, not only at rest. Base
   body task checks it with a swept section.
2. Panel removal. Pushing the latch tab back must free the catch before the
   panel starts to tip. The backrest task asserts the catch clears the shell
   top edge at full tab travel.
3. The sensor dropping out when the bottom plate slides out. The plate
   carries the sensor's weight through a foam pad, and the pocket has no
   ledge, because a ledge would block removal. Service is done with the
   base turned over, so the sensor lifts out upward. The pocket mouth gets
   a 1 mm chamfer lead in so the sensor slides past the foam. The bottom
   plate task asserts a pad seat under the sensor, and the README says to
   turn the base over first.
4. Exhaust loop. The outlet grille at the right end of the pocket and the
   inlet grilles at the left end are split by the fin. Assert the fin
   reaches at least 8 mm past the back wall.
5. Tipping on the desk. The tilted panel plus the base must keep the centre
   of mass inside the feet, tilt at least 10 degrees forward and backward
   before tipping, and hold a 0.5 N push at the top edge. Assembly task
   computes it from volumes at PLA density 1.24 g/cm³ (ASSUMED) and the
   panel mass (not in source, assume 120 g, ASSUMED). (Changed after Task 1,
   a 10 N push was unrealistic for a 300 to 600 g desk Thing.)

---

## File structure

```
hardware/enclosures/crowpanel-epaper-42-sen63c/
  DESIGN.md         approved spec (exists)
  PLAN-iteration-1.md   this plan
  README.md         change log, print settings, owner print test
  params.py         shared numbers, Task 1 only writes it
  vendor.py         loads and places vendor STEP bodies
  test_plate.py     @model test_plate
  base_body.py      @model base_body
  backrest.py       @model backrest
  bottom_plate.py   @model bottom_plate
  assembly.py       @model assembly, interference and tipping checks
```

### Task 1: Shared parameters and vendor placement

**Files:**
- Create: `params.py`, `vendor.py`

**Interfaces:**
- Produces in `params.py`: plain float constants in mm, grouped as
  `PANEL_*` (from the board sheet), `SEN_*` (sensor sheet), `PLA_*`
  (material), `FIT_*` (clearances), `ASSUMED_*` (every row of the DESIGN.md
  "Assumed for iteration 1" table), `TILT_DEG = 15.0`, and the derived
  layout constants every part shares: `CRADLE_*`, `POCKET_*`, `GRILLE_*`,
  `DOVETAIL_BACKREST_*`, `DOVETAIL_PLATE_*`, `BASE_W`, `BASE_D`, `BASE_H`.
  Plus `panel_to_base() -> build123d.Location` and
  `sensor_to_base() -> build123d.Location`.
- Produces in `vendor.py`: `panel_body() -> build123d.Compound` and
  `sensor_body() -> build123d.Compound`, already placed in the base frame.

- [ ] **Step 1: Write the asserts first** at the bottom of `params.py`.
  `POCKET` inner size ≥ 55.4 x 25.7 x 21.8 plus gasket and pad at their
  compressed thickness. `CRADLE_FRONT_LIP_H` keeps the lip top below panel
  y 8.1 after tilt. Side lips top below panel y 15. Header slot spans at
  least x 19.0 to 48.0. Inlet grille area ≥ 112 mm², outlet ≥ 300 mm².
  The pocket's right end x is left of 68.0 minus 10 (heat zone margin).
  `BASE_W`, `BASE_D` and the backrest height each fit in 246 mm.
- [ ] **Step 2: Run** `pixi run -e cad python -I params.py` from the set
  folder and see the asserts fail on unset constants.
- [ ] **Step 3: Fill the constants** until the asserts pass. Every line
  carries a source comment (sheet and row) or `# ASSUMED`.
- [ ] **Step 4: Write `vendor.py`.** Load both STEPs, apply the board
  sheet's STEP to viewer mapping (x = 53.5 - X, y = Z + 43.5, z = Y + 0.5),
  then `panel_to_base()`. Place the sensor with its opening face on the back
  wall inner face, inlet end at the left.
- [ ] **Step 5: Verify** with a scratch `@model` that returns both vendor
  bodies. `cad run` it, then `cad step snapshot` from the left and the back.
  Expected: panel leaning back 15 degrees, header edge down, sensor below
  its left half with openings facing +Y.

### Task 2: Test plate

**Files:**
- Create: `test_plate.py`

**Interfaces:**
- Consumes: `params.py` constants only.
- Produces: `@model test_plate()` returning one solid, flat on Z = 0,
  within 180 x 180 mm.

- [ ] **Step 1: Asserts first.** Dovetail ladder at 0.15, 0.20, 0.25 and
  0.30 per side, each pair labelled. Three latch arms at 15, 18 and 20 mm,
  root 1.8 to tip 0.9, 6 wide, root strain at full catch deflection ≤ 1.2 %
  (use `params.latch_strain_tapered`, the Bayer tapered arm formula, and
  report the constant section value too).
  Cradle slice 15 mm wide taken at x 20 to 48 so it holds the header slot,
  with the screw relief at (4, 4) in a second slice at x 0 to 15, and the
  BOOT and RST window in a third slice at x 64 to 89. Pocket ring 5 mm
  tall. Grille sample. Every text label embossed 0.4 mm, at least 5 mm
  tall.
- [ ] **Step 2: Run** `pixi run -e cad cad run test_plate.py`. Expected:
  asserts fail.
- [ ] **Step 3: Model** until the asserts pass.
- [ ] **Step 4: Check** `cad model dfm --process printing --technology fdm
  --material pla` with no errors. Snapshot from the top.

### Task 3: Base body

**Files:**
- Create: `base_body.py`

**Interfaces:**
- Consumes: `params.py`, `vendor.py`.
- Produces: `@model base_body()` in the base frame, printed upright as
  modelled.

- [ ] **Step 1: Asserts first.** Cradle front lip, side lips, header slot,
  screw reliefs at panel (4, 4) and (103, 4), BOOT and RST window x 65 to
  88 up to panel y 10, sensor pocket with rigid gasket stops and a 1 mm
  chamfer lead in at the open bottom, back wall grille areas, fin ≥ 8 mm, solid roof between pocket and
  cradle, harness bay holds a 30 x 10 x 8 mm pull-up bundle, backrest
  dovetail socket and bottom plate rails from the `DOVETAIL_*` constants.
  Swept check: the panel plus a 25 mm jumper block, rotated about the
  cradle pivot from 0 degrees (upright, the insertion and removal pose) to
  15 degrees (the rear wall rest stop) in 1 degree steps, has zero overlap
  with the body, and the jumper block alone stays clear to 35 degrees. Draft the
  front lip inner face if the forward tip needs it. BOOT and RST pin
  channels d3.2 through the deck along panel z at panel (69.805, 5.5) and
  (83.5, 5.5), so a pin reaches both with the backrest fitted.
- [ ] **Step 2: Run** `cad run`, expect failures.
- [ ] **Step 3: Model** until they pass. Vertical grille slots, chamfers
  under every overhang, 0.4 mm chamfer on bed edges.
- [ ] **Step 4: Check** dfm clean, sections through the cradle, the header
  slot and the pocket.

### Task 4: Backrest and bottom plate

**Files:**
- Create: `backrest.py`, `bottom_plate.py`

**Interfaces:**
- Consumes: `params.py` (`DOVETAIL_*`, `FIT_*`, latch numbers).
- Produces: `@model backrest()` and `@model bottom_plate()`, each in the
  base frame at its assembled position, plus `backrest_print()` and
  `bottom_plate_print()` that lay each flat on Z = 0 for export.

- [ ] **Step 1: Asserts first.** Backrest spine at panel x 47 to 67, clear
  of the TF slot (x 19.75 to 36.15) and the heat zone (x 68 to 104). The
  tongue stays at x 57.2 to 66.855 from `params.py`. Latch arm from the test plate's 18 mm variant,
  strain ≤ 1.2 %, catch overlap of the shell front ≤ 1.2 mm, catch clears
  the shell top at full tab travel. Dovetail male fits the socket at
  `FIT_SLIDE` per side with an end detent of 0.4 mm. Bottom plate rails
  match, end detent 0.4 mm, vent slots at least 20 mm from any grille, four
  feet recesses for the 10 mm feet,
  and a pad seat under the sensor that leaves 1.0 mm of pad compression.
- [ ] **Step 2: Run** both, expect failures.
- [ ] **Step 3: Model** until they pass.
- [ ] **Step 4: Check** dfm clean on both print orientations.

### Task 5: Assembly, export, slicing and hand off

**Files:**
- Create: `assembly.py`, `README.md`

**Interfaces:**
- Consumes: every `@model` above and `vendor.py`.
- Produces: `@model assembly()` and the files in `iter1/`.

- [ ] **Step 1: Asserts first.** `cad model interference` on the assembly
  reports no overlap between printed parts and vendor bodies (foam is
  allowed to overlap the sensor face by its compression). Tipping check from
  Review Focus 5 passes.
- [ ] **Step 2: Run** and fix in the owning part script.
- [ ] **Step 3: Export** each part in print orientation to STEP and 3MF in
  `iter1/` with `cad model export`. Slice each 3MF with Bambu Studio's CLI
  for the A1, 0.4 nozzle, 0.20 mm layers, generic PLA profile at eSUN's
  temperature, and record print time and filament. If the CLI cannot slice,
  say so and hand over the 3MF only.
- [ ] **Step 4: Write `README.md`.** Dated change log line for iteration 1,
  the ASSUMED list, print settings and orientation per part, a shopping list
  (foam tape, feet, two 10 k resistors, heat shrink), and the owner's print
  test as numbered steps with times, ending with what to measure and report
  for iteration 2.
- [ ] **Step 5: Snapshots** front, back, left and an exploded view into
  `iter1/snapshots/`.
