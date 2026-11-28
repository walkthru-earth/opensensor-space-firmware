<!-- SPDX-License-Identifier: CC-BY-4.0. Copyright Walkthru.Earth. -->

# Agent notes for the ePaper-3.97 and SEN63C desk stand

Read the repo `AGENTS.md`, then `.claude/skills/hardware-design/SKILL.md`,
then [DESIGN.md](DESIGN.md) and [README.md](README.md) in this folder.
`params.py` is the source of truth for every number. When the docs and
`params.py` disagree, fix the docs.

## State

- Owner printed the shell, bezel and sensor bar on 2026-11-26 and the fit was
  right. The battery shelf in the shell was added the same day and the owner
  printed the three part plate with it and reported it good on 2026-11-28.
- A separate battery cradle on the board studs was modelled first and dropped
  before printing, at the owner's request. Do not bring it back.
- The battery shelf test steps in DESIGN.md are not recorded yet.

## Commands

Run every command from this folder.

- `pixi run -e cad python -I params.py` runs the asserts and prints the pack
  and lead numbers.
- `pixi run -e cad cad run <part>.py` builds a part and runs its
  `check_geometry`. build123d only loads inside `cad run`, and `cad run` only
  takes scripts in this folder, a scratch script elsewhere fails with
  "No module named build123d". Print debug lines from `check_geometry`
  instead and take them out after.
- `cad run` caches bodies, so a second run with no change prints no REPORT
  lines. Change the script or read the first run.
- `cad model dfm <id> --process printing --technology fdm --material pla` on
  each `*_print` model id.
- `cad model interference <id> --fit 'assembly/waveshare_397' 'assembly/waveshare_397' --fit 'assembly/sen63c' 'assembly/sen63c'`
  on the assembly. One pair inside the Waveshare STEP cannot be intersected
  by the kernel, it is not ours.
- `cad model export <id> <file>.3mf --tolerance 0.01`, and STEP with no
  tolerance. Files go to `artifacts/enclosures/waveshare-epaper-397-sen63c/`,
  which git ignores.
- `python3 -I slice.py` builds the A1 presets from the installed Bambu Studio
  and slices every plate. After a shell change, put the new shell grams in
  `SLICE_MASS_G` and run the assembly again, the tipping check uses them.

## Frames

- Board frame (bx, by, bz) is the vendor STEP. Case frame is
  (bx, by S - bz C, OZ + by C + bz S) with OZ 13, S = sin 15 and C = cos 15.
- `y_at(z, bz)` gives the case Y of a board plane bz at case height z.
- The shell prints back wall down. A face whose normal points far toward +Y
  in the case frame is an overhang in print. The shelf parts are X slabs with
  YZ profiles, so they print as walls.

## Gotchas

- `part & body` returns None when the two do not meet. Test for None before
  `.volume`.
- Bambu "floating cantilever" or "floating regions" warnings come from a
  downward tip in the print pose. Fix them with a 45 degree ledge, not
  supports.
- The vendor STEP files stay out of git, README Vendor files says how to get
  them and their hashes.
- Measured by the owner on 2026-11-26, pack 61.5 x 41 x 4.0, lead about 55.
  Coin, pack and board masses are still ASSUMED in `params.py`.
- Never recommend a CR2032 for the RTC header. Use an ML2032 on a lead of 80
  or longer.
- Commit only when the owner asks.
