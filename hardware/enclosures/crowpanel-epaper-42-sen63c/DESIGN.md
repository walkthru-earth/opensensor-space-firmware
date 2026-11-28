<!-- SPDX-License-Identifier: CC-BY-4.0. Copyright Walkthru.Earth. -->

# CrowPanel 4.2 and SEN63C desk base, design

Status is approved by the owner on 2026-11-19. Iteration 1 is modelled on the
assumed values below, the owner prints it and the comments set iteration 2.

## Intent

The CrowPanel ESP32-S3 4.2" e-paper HMI (DIE07300S001, v1.2) with a Sensirion
SEN63C is the third OpenSensor Thing. Today the panel lies loose on the desk
with the sensor hanging off its GPIO header. This base gives it a first
physical home. It stands the panel up at a readable angle, gives the sensor
honest outside air away from the ESP32 heat, and keeps every key, port and
card reachable. It is a desk prototype in eSUN PLA+ on a Bambu Lab A1 with a
0.4 mm nozzle. A field version comes later.

## Goals

- Panel stands tilted back about 15 degrees from vertical, readable from a
  seated position.
- The SEN63C sits in its own chamber in the base, below the panel and away
  from the ESP32 heat, with its openings coupled straight to outside air as
  Sensirion's design-in guide asks.
- No screws anywhere. The owner assembles and opens it by hand, and the TF
  card, the sensor and the cable can be reached without tools.
- The dial, EXIT and MENU keys, USB-C, the battery connector, the TF slot,
  BOOT and RST all stay usable with the panel in the base.
- Every part prints on the A1 without supports inside a closure. The
  base body ceilings over the plate and the harness bay print on supports
  from the bed, which are cleared before assembly.
- Fits are tuned on a small test plate before the full print.

## Non-goals

- No replacement of the stock Elecrow shell. The panel stays in its own
  printed shell and the base holds that shell.
- Not weatherproof, not for outdoor or field use, no UV rating. eSUN lists
  PLA+ as not UV resistant and its heat distortion temperature is about 54 °C.
- No battery bay in this version.
- No firmware change.

## Anti-goals

- A latch or clip that holds the sensor gasket under constant load. PLA+
  creeps, so the seal would loosen over weeks.
- Any part that needs a screwdriver, glue or heat inserts to open.
- A sensor chamber that shares air with the panel, or lets the fan exhaust
  loop back into its inlets. Readings that look fine but are biased warm
  would make the Thing worse than no enclosure.
- Metal or magnets near the ESP32-S3 antenna.

## Constraints and source numbers

This design follows `.claude/skills/hardware-design/SKILL.md`. Full sources,
revisions and SHA-256 hashes are in the shared part sheets
`hardware/parts/boards/crowpanel-epaper-42.md` and
`hardware/parts/sensors/sen63c.md`, and in
`.claude/skills/hardware-design/materials-and-closures.md`. The numbers the
design rests on are below.

**Panel** (from `hardware/parts/boards/crowpanel-epaper-42.md`, Elecrow STEP at commit
`fa49941f`, file SHA-256 `4c02b3e7...4a20`). The frame is the display face,
origin at the PCB lower left, x right, y up, z toward the viewer, z = 0 at
the PCB front.

| Item | Value |
| --- | --- |
| Panel in stock shell | 109.6 x 87.3 x 9.8 mm, z -8.1 to 1.7 |
| PCB | 107 x 87 x 1.6, R3 corners |
| Glass | 91 x 77 at x 8 to 99, y 5 to 82 |
| Active area | 84.8 x 63.6, lowest active row at about y 8.1 |
| Dial | left edge, wheel d13.4 at about (4.9, 43.5), 2.3 to 2.4 proud |
| MENU and EXIT keys | left edge, y 20.5 to 23.5 and y 63.5 to 66.5, 1.05 proud |
| USB-C | right edge, centre y 64.0, z -2.7 |
| Battery connector | right edge, centre y 43.5 |
| TF slot | top edge, x 19.75 to 36.15 |
| GPIO header | 2x10 right angle socket opening down at the bottom edge, x 20.7 to 46.3, plug pushes in toward +y |
| SEN63C signals on the header | columns 1 to 7 (x 22.1 to 44.9) carry SDA, SCL, 3V3 and GND |
| BOOT and RST | on the back, x 67.6 to 72.1 and 81.3 to 85.8, y 3.3 to 7.8, plungers 0.5 proud of the back cover |
| Shell screws | six M2.5 positions, four corners and (23.5, 43.5), (83.5, 43.5), all used by the stock shell |
| Heat sources | back right, x 68 to 104, y 17 to 60 (ESP32-S3 module, buck, charger, CH340C) |
| Antenna | ESP32-S3 module antenna end toward the right edge |

Elecrow published CAD for v1.0 only. The stock shell in the same STEP fits
the outline, holes and edge parts, so they are taken to carry over to v1.2
within about 0.2 mm. The caliper list at the end of the board sheet
confirms it on the owner's unit.

**Sensor** (from `hardware/parts/sensors/sen63c.md`, SEN6x datasheet v1.0
October 2026, mechanical design-in guide).

| Item | Value |
| --- | --- |
| Outline | 55.2 +0.2/-0.4 x 25.6 +0.1/-0.2 x 21.3 ±0.5 mm |
| Pocket clears | 55.4 x 25.7 x 21.8 (largest part) |
| Openings | all on one 55.2 x 25.6 face, two inlets near one end, Ø20.5 outlet at 42.4 from that end |
| Minimum grille area | inlets 56 mm², outlet 148 mm² |
| Orientation | opening face vertical. Never up, avoid down |
| Thermal | under 5 K over ambient, below and away from heat sources, never insulated |
| Connector | on the open underside, plug pushes toward the opening face, cable under 10 cm |
| Mass | 20 g typical |
| Fan tone | 1000 Hz dominant, so stiff panels and soft mounting |

**Material and printer** (from `materials-and-closures.md` in the skill, eSUN PLA+
datasheet February 2026).

| Item | Value |
| --- | --- |
| Elongation at break | 4.14 % in XY, 2.92 % in Z |
| Safe repeated snap strain | about 1.2 % in XY (derived, see the research file) |
| Starting latch arm | tapered 1.8 to 0.9 mm, 15 to 20 mm long, 6 mm wide, printed flat, 1.0 to 1.2 mm catch, 30 degree lead in, 45 degree release |
| Sliding fits | 0.20 to 0.25 mm per side, covers 0.30 to 0.40 mm, to be tuned on the test plate |
| Build volume | A1 256 x 256 x 256 mm |

## Approaches considered

1. **Wedge base with a sensor chamber (chosen by the owner).** The panel
   stands in a cradle on a low base and the SEN63C lives in a chamber inside
   the base. The sensor ends up below the heat, the panel keeps its stock
   shell, and the base is a small print that can change without touching the
   panel.
2. **Full replacement enclosure.** One housing for the bare PCB, panel and
   sensor. It looks the most finished, but it puts the sensor in the same
   box as the ESP32 heat, needs the stock shell removed, and every fit
   mistake means reprinting the whole thing.
3. **Rear clip-on pod.** A sensor pod clipped to the back of the stock shell
   with a fold-out stand. Small and cheap, but the pod would sit beside or
   above the warm back right of the board and the panel would be easy to
   knock over.

## Design

The base has four printed parts.

```
        side view (left side)               back view

              ___  latch                 _________________
             /  /|                      |   backrest      |
   panel -> /  / |  <- backrest         |   spine         |
           /  /  |                      |                 |
          /  /   |                   ___|_________________|___
     ____/__/____|____              | [inlet] | [ outlet ]    |  <- grilles
    |  cradle  harness |            |_________________________|
    |  [sensor]  bay   |             bottom plate slides out sideways
    |_________________ |
     bottom plate, feet
```

### 1. Base body

Printed upright on its bottom face.

- **Cradle.** A channel on top of the base takes the bottom edge of the
  shell at the 15 degree tilt. It has a front lip about 3.5 mm tall, low
  enough to stay below the lowest active row at y 8.1, a rear wall, and side
  lips that rise no higher than y 15 so the MENU key at y 20.5 stays free on
  the left and USB-C and the battery connector stay free on the right.
- **Header slot.** The channel floor opens under the GPIO header from x 19
  to x 48 (header plus clearance). The owner keeps straight female jumpers
  on the header, so the slot runs through to the harness bay with room
  below for the jumper housings and their bend. The panel goes in by
  threading the jumpers down through the slot and lowering the panel, so
  the slot is open from above with no lip that would catch a housing. The
  jumper housing length is measured on the owner's jumpers, not taken from
  a catalogue, and sets the base height.
- **Screw head relief.** The stock printed shell is held by six screws
  through the back. The cradle rear wall gets relief pockets at the two
  bottom corner screws, (4, 4) and (103, 4), sized from the measured head
  height and diameter.
- **BOOT and RST window.** The rear wall of the cradle has a window behind
  x 65 to 88 up to y 10, so it never presses the plungers and a pin can
  still reach both buttons.
- **Sensor pocket.** Inside the base, under the left half of the panel, as
  far as possible from the heat zone at x 68 to 104. The SEN63C stands with
  its opening face vertical against the back wall of the base, its 55.2 mm
  length horizontal and the inlet end toward the outer left. The pocket
  clears the largest sensor (55.4 x 25.7 x 21.8) plus the gaskets.
- **Back wall grilles.** Grilles in the back wall line up with the inlets
  and the outlet. Each grille has at least twice the Sensirion minimum open
  area, so at least 112 mm² for the inlets and 300 mm² for the outlet, because
  printed bars block part of each opening and Sensirion asks for more area
  under a mesh without saying how much. Slots run vertically so the wall
  prints without bridges. A short fin outside the wall between the inlet
  and outlet grilles stops the exhaust from flowing straight back into the
  inlets. The grilles face the back of the desk, which shades them from the
  room's light. The owner places the base free standing with open air
  behind it, so no rear standoffs are needed.
- **Gaskets.** A soft closed-cell foam gasket between the sensor face and
  the back wall seals the inlets from the outlet and both from the chamber.
  The gasket compression is set by rigid stops in the pocket, never by a
  latch.
- **Soft mounting.** Foam pads at the two ends of the sensor, between the
  sensor's open underside rim and rigid ribs, hold it against the back wall
  gasket and take up the ±0.5 mm sensor tolerance. The pads leave the
  connector and the plug bend clear. No hard plastic touches the sensor.
- **Harness bay.** The space between the header slot and the sensor
  connector holds the cable, kept under 10 cm, and the two 10 k pull-up
  resistors from SDA and SCL to 3V3, soldered into the harness and covered
  with heat shrink. The bench record from 2026-11-17 shows the sensor still
  running on the ESP32 internal pull-ups, so the bay leaves room for this
  fix.
- **Ambient vent.** The chamber interior vents through a few small slots in
  the bottom plate, away from the grilles, so it sits at room pressure.
- **Thermal wall.** A solid roof separates the sensor pocket from the
  cradle, so warm air from the panel does not reach the sensor except
  through the narrow header slot, which sits on the cool left side.

### 2. Backrest with top latch

Printed flat on its back, so the latch arm flexes along the layers.

- A spine that slides into a dovetail socket at the back of the base and
  rises behind the panel at the 15 degree tilt to its top edge.
- It sits at x 47 to 67, left of the heat zone at x 68 and away from the
  antenna end, and clear of the TF slot at x 19.75 to 36.15. (Moved from x
  55 to 75 on 2026-11-19, which overlapped the heat zone by 7 mm.)
- At the top one tapered release latch, using the starting arm numbers
  above, hooks over the top edge of the shell. The catch overlaps the front
  by at most 1.2 mm, which lands on the shell border above the glass.
- To insert the panel, set its bottom edge in the cradle and tip it back
  until the latch clicks. To remove it, push the latch tab back and tip the
  panel forward. Gravity holds the panel in the cradle, and the latch only
  stops it falling forward when the base is carried or knocked.
- The spine stays in its socket because the panel leans on it, with a small
  detent bump at the end of the dovetail.

### 3. Bottom plate

Printed flat.

- Slides into the bottom of the base on dovetail rails with an end detent
  bump, and closes the sensor pocket and harness bay from below.
- Carries the ambient vent slots and four places for stick-on rubber feet.
  The feet help against the fan's vibration, as Sensirion asks.
- Sliding it out exposes the sensor and cable. The sensor then lifts out of
  its pocket without tools.

### 4. Test plate

Printed flat first, about 30 minutes, so the full print only happens once
the fits are right. Each piece is labelled on its face.

- Clearance ladder for the dovetails, 0.15, 0.20, 0.25 and 0.30 mm per side.
- Three latch arms, 15, 18 and 20 mm long, each with a hook against a
  shell-edge stand-in.
- A 15 mm wide slice of the cradle to try on the real panel bottom edge,
  with the header slot and the BOOT and RST window.
- A 5 mm thick ring of the sensor pocket outline to try on the real SEN63C
  with the chosen foam.
- One grille sample to check the slots print open.

## Assembly and maintenance

1. Stick the gasket on the sensor's opening face and the end pads on the
   ribs.
2. Plug the cable into the sensor, push the sensor into its pocket from
   below and route the cable up through the harness bay.
3. Slide in the bottom plate until it clicks.
4. Slide the backrest into its socket.
5. Plug the cable into the header, set the panel in the cradle and tip it
   back until the latch clicks.

The TF card comes out from the top edge without moving anything. To service
the sensor, slide the bottom plate out.

## Checks the builder runs

- The panel and sensor STEP files as reference bodies in the assembly, with
  `cad model interference` clean.
- `cad model dfm --process printing --technology fdm --material pla` on
  every part, every error fixed and the warnings reported.
- Asserts in the model for the grille areas, the cradle lip heights against
  y 8.1 and y 15, the latch strain against 1.2 %, and the pocket against the
  largest sensor.
- Snapshots from the front, back and left, and sections through the cradle,
  the latch and the sensor pocket.

## Decided by the owner

- Desk use first, field version later.
- Header edge down, TF slot up. The owner confirmed the screen reads
  upright that way (2026-11-19).
- Straight female jumpers stay on the GPIO header.
- The panel stays in Elecrow's stock printed shell with its back screws.
- The base stands free with open air behind it.
- Wedge base with the sensor in its own chamber (approach 1).
- No screws, opened by hand for maintenance.
- First prints in eSUN PLA+ on a Bambu Lab A1, 0.4 mm nozzle.

## Calls made in this design (owner can change)

- 15 degree tilt.
- Grilles on the back wall, facing away from the user and the room light.
- Twice the Sensirion minimum grille area.
- A separate backrest printed flat, because a latch arm printed upright
  would flex across the layers.
- Room in the harness bay for the missing 10 k pull-ups.
- BOOT and RST are reached with a pin through two d3.2 channels in the
  deck, so the backrest can stay fitted.
- The panel tips forward to upright (0 degrees) for insertion and removal.
  The rear wall is the rest stop at 15 degrees, so the panel envelope is 0
  to 15 degrees. The jumper block is checked wider, 0 to 35 degrees.
- The base body prints upright with supports from the bed under the plate
  ceilings, the pocket roof and the cradle floor over the harness bay
  (iteration 1). A layout with shorter ceilings is an iteration 2 item if
  the support is hard to clear or the plate binds.
- The skin between the backrest socket and the d3.2 BOOT pin channel is
  1.15, under the usual 1.35, so the backrest tongue keeps its width.
- The pocket walls and the two gasket stops hold the sensor's yaw, with no
  third stop. The print test checks that the sensor does not twist.
- Buy stick-on rubber feet and closed-cell foam tape. Neither source gives
  a foam grade, so the test plate picks the thickness.

## Left to the builder

- Wall thicknesses, rib and fillet sizes, chamfers, the exact grille slot
  pattern and the dovetail profile, within the print3d skill's rules and
  the test plate results.
- The exact base footprint and height, kept to a stable stance on the desk.
- The cable route inside the harness bay.

## Assumed for iteration 1

The owner chose to print iteration 1 on assumed values instead of measuring
first (2026-11-19). Each one is a named constant in `params.py` marked
ASSUMED, so iteration 2 changes one number.

| Item | Assumed value | Basis |
| --- | --- | --- |
| Jumper housing | 2.54 x 2.54 x 14.0 mm, standard Dupont single pin female | common Dupont housing, not measured |
| Room below the shell bottom edge for jumpers | 25 mm (13 mm housing outside the shell, 1 mm of it sits inside to the socket face at y 1.0, plus 10 mm for the wire bend) | board sheet socket face y 1.0, bend not measured |
| Shell screw heads | M2.5 pan head, d 4.5, 1.8 tall past the back cover | not in source, board sheet caliper item 10 |
| Screw head relief | d 6.0, 2.5 deep | head plus 0.75 radial and 0.7 depth clearance |
| Sensor gasket | 3.0 mm closed-cell foam tape, compressed to 2.0 mm by rigid stops | Sensirion asks for soft foam, no grade given |
| Sensor end pads | 3.0 mm foam, compressed to 2.0 mm | same |
| Plug and bend below the sensor rim | 15 mm | JST GH plug plus bend, not measured |
| Sliding fits for the full parts | 0.20 mm per side | middle of the research range, the test plate checks it |
| Rubber feet | 10 mm round, 3 mm tall stick-on | common size |

## Open items

- The length of the owner's jumper housings and the height they need below
  the header, measured before the base height is set.
- Screw head height and diameter on the back of the shell, added to the
  board sheet's caliper list.
- The caliper list in the board sheet, mainly the shell
  outline, the header socket position and the BOOT and RST plunger height.
- The plug height above the sensor rim and the cable bend radius. No source
  gives them, so the model reserves `ASSUMED_PLUG_ZONE` (15 mm) in front of
  the rim and keeps it behind the jumper block. Measure the real plug and bend
  with the sensor seated in the pocket ring on the test plate. The ring is
  only 5 mm tall, so it gives the numbers but cannot show a clash in the full
  base. The first full print (README owner print test steps 6 and 7) is the
  check for that.
- Foam material and thickness, settled on the test plate.
- Which panel FPC connector is fitted, only if it changes the back face.
