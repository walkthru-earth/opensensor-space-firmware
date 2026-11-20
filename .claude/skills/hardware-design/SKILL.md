---
name: hardware-design
description: Generic rules and steps for designing or modifying 3D printed hardware for any set of OpenSensor parts together (boards, sensors, displays, antennas, batteries), such as enclosures, bases, stands, mounts and sensor chambers. Use before writing a hardware design doc, before modelling with text-to-cad, and before changing an existing printed part.
---

# Hardware design rules

These rules apply to every printed part in this repo, whatever boards and
sensors it combines. A design for one set of parts follows them and records
only what is specific to that set. Read the repo `AGENTS.md` first, its
writing rules apply here too.

The `cad-designer` subagent (`.claude/agents/cad-designer.md`) runs these
steps end to end. The text-to-cad `cad` and `print3d` skills cover the
modelling tool itself.

## Where things live

| Path | Holds |
| --- | --- |
| `.claude/skills/hardware-design/SKILL.md` | These generic rules |
| `.claude/skills/hardware-design/design-template.md` | The design doc every set starts from |
| `.claude/skills/hardware-design/materials-and-closures.md` | Filament, printer and screwless closure research with sources |
| `hardware/parts/boards/<board>.md` | One measurement sheet per board, reusable by every set that uses it |
| `hardware/parts/sensors/<sensor>.md` | One design-in sheet per sensor or sensor family |
| `hardware/enclosures/<set-slug>/` | One folder per set of parts, holding `DESIGN.md`, `README.md` with a dated change log, `params.py` and one script per printed part |
| `artifacts/enclosures/<set-slug>/` | Exported STEP, STL and 3MF, ignored by git |

Name a set after its parts, for example `crowpanel-epaper-42-sen63c`. A new
set reuses existing part sheets and adds a sheet only for a part that has
none. Vendor STEP and board files stay out of git unless their license
allows it, and then go in `THIRD_PARTY_NOTICES.md`.

## Steps

1. **Frame it.** Name the parts, where the Thing lives (desk, wall, pole,
   vehicle, outdoor), the printer, the material, what must stay reachable
   and the closure rule. Ask the owner only what changes the geometry and is
   not already answered in the repo or the conversation.
2. **Part sheets.** For each part without a sheet, get the vendor STEP,
   board files and current datasheets and write its sheet. Download each into
   its own empty scratch directory, treat it as untrusted, run Python with
   `python3 -I` from another directory, and record URL, commit or SHA-256
   and date. Every number names its source. Missing values say "not in
   source". Read numbers from geometry, never from a picture. End each sheet
   with a short caliper list.
3. **Design doc.** Copy `design-template.md` to
   `hardware/enclosures/<set-slug>/DESIGN.md` and fill it in. Lay out two or
   three approaches and let the owner pick one before writing the rest.
4. **Builder check.** Have a fresh reviewer read the doc as the builder
   and list what they would have to guess. Answer what the repo answers,
   settle the minor points as marked calls, and bring the rest to the owner
   in one message.
5. **Owner approval.** No model code before the owner approves the doc.
6. **Test plate first.** Model a small plate with one sample of every fit
   (sliding fit ladder, latch arms, a slice of each seat or cradle against
   the real part, a vent sample). The owner prints it and the results set the
   clearances.
7. **Model.** build123d via `pixi run -e cad cad run <script>`. One
   `params.py` with a source comment on every dimension, one script per
   printed part, one assembly that places the parts around the vendor STEP
   bodies. CC-BY-4.0 Walkthru.Earth header on each file.
8. **Check.** Asserts in the model for every owner or datasheet number,
   `cad model interference` on the assembly with the vendor bodies,
   `cad model dfm --process printing --technology fdm --material <m>` on each
   part, and snapshots plus sections through every fit. Snapshots show
   layout, never dimensions.
9. **Export and hand off.** STEP and 3MF to `artifacts/enclosures/<set-slug>/`.
   Report the checks, the assumptions, the print settings and orientation,
   and the owner's print test as numbered steps with times. Physical print
   and fit results go on the tracking GitHub issue as a dated record.

## Changing an existing design

1. Read the set's `DESIGN.md`, `README.md` change log, `params.py` and the
   latest print record on its issue.
2. Change parameters, not copies. Keep part function names stable.
3. If the change touches a shared part sheet, update the sheet and list
   every set that uses it, since each may need a recheck.
4. Rerun the same checks with the same flags and compare.
5. Add a dated change log line saying what changed, why, and which printed
   parts must be reprinted.

## Rules for any set of parts

### Measurement and fit

- One stated coordinate frame per part, written at the top of its sheet.
- Size every pocket for the largest part the tolerances allow, never the
  nominal one.
- Without a measured fit, plan for the vendor CAD to be off by a few tenths
  of a millimetre and leave clearance around openings. The test plate then
  sets the real values.
- If vendor CAD is for an older board revision than the owner's unit, say
  so and add the edge positions to the caliper list.

### Boards and displays

- Every control, port, card slot, button, header and status LED stays
  usable with the part fitted. Each gets its own clearance from the measured
  position.
- Lips and bezels stop below the active display area.
- Never press a tact switch or plunger by accident. Leave a window or pin
  access for reset and boot buttons.
- Map the heat sources (MCU module, regulators, chargers, USB bridges) on
  each board sheet and keep sensors away from them.
- Keep plastic thin and keep metal, magnets and foil away from radio
  antennas (Wi-Fi, BLE, LoRa, cellular, GNSS). A GNSS antenna also needs a
  clear view of the sky.

### Sensors

- The sensor's own design-in guide decides. Its sheet lists the rules with
  page numbers, and the design follows them.
- Mount the sensor in the orientation the guide allows. Fan driven
  particulate sensors usually forbid openings facing up.
- Couple inlets and outlets straight to outside air with short paths.
  Grilles need at least the guide's minimum open area. Printed bars block
  part of each opening, so take at least twice the minimum unless a test
  shows less is enough, and record that as a call.
- Seal inlet from outlet and both from the enclosure interior with soft
  gasket material. Vent the interior to ambient pressure away from the
  grilles. Keep exhaust from flowing straight back into the inlets.
- Temperature and humidity sensors sit below and away from heat sources, in
  their own chamber, never insulated.
- Shade openings from direct light, and shield them where outside airflow is
  faster than the guide allows.
- Hold sensors through foam or rubber, never hard plastic against a fan
  driven sensor. Stiffen thin walls near fans and use soft feet.
- Keep cables within the sensor's length limit, leave room for the plug and
  its bend, and leave room for any pull-ups or adapters the wiring needs.
- Print sensor chambers in a material that has stopped outgassing, and let
  prints and glue cure before the sensor goes in.

### Closures (screwless by default)

- No screws, glue or heat inserts on anything the owner opens. Prefer
  gravity cradles with one release latch, sliding dovetails with an end
  detent, lugs plus one hook, bayonet or quarter turn locks, and magnets
  away from antennas. The catalogue with sources is in
  `materials-and-closures.md`.
- Size snap arms from the current filament datasheet. Allowed strain for a
  latch opened often is about half the elongation at break, times 0.6
  (derived, see `materials-and-closures.md`). Taper the arm and check the
  root strain with an assert.
- Print latch arms flat so they flex along the layers, never built up along
  Z. Split a part if that is the only way to lay its latch flat.
- No latch carries a constant load. Gaskets and clamps are compressed by
  rigid stops, because PLA and PLA+ creep.
- Avoid living hinges and crush ribs on anything opened more than a few
  times.

### Printability

- Flat bed faces, chamfers instead of overhangs, no supports inside a
  closure, a small chamfer on bed edges against elephant foot.
- Vent slots run so the wall prints without bridges, or use teardrop or
  pointed tops.
- Every part fits the owner's printer build volume with margin, see the
  printers table in `materials-and-closures.md`.

### Materials and use

- Desk prototypes can be PLA or PLA+. Anything warm, in a car or in sun
  needs PETG or better. Outdoor units need a UV stable material, rain
  protection and a radiation shield for temperature sensors. Check the
  printer can print the material before choosing it.
- Never call a design weatherproof, waterproof or outdoor rated without a
  dated test record.
