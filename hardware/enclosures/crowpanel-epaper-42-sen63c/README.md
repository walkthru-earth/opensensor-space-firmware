<!-- SPDX-License-Identifier: CC-BY-4.0. Copyright Walkthru.Earth. -->

# CrowPanel 4.2 and SEN63C desk base

A screwless printed desk base for the Elecrow CrowPanel ESP32-S3 4.2 inch
e-paper panel in its stock shell, with a Sensirion SEN63C in its own chamber
under the panel. The design and every owner decision are in
[DESIGN.md](DESIGN.md). The build plan for iteration 1 is in
[PLAN-iteration-1.md](PLAN-iteration-1.md).

Three printed parts make the base.

| Part | Script | What it does |
| --- | --- | --- |
| Base body | `base_body.py` | Cradle that holds the panel at 15 degrees, harness bay, sensor pocket with back wall grilles, backrest socket, plate rails |
| Backrest | `backrest.py` | Spine behind the panel with one release latch over the shell top edge |
| Bottom plate | `bottom_plate.py` | Slides in under the base, carries the feet and the pad under the sensor, clicks home with a detent ridge |

`test_plate.py` makes the fit coupons, `assembly.py` places everything with
the vendor panel and sensor and runs the tipping check, and `params.py` holds
every number with its source.

## Change log

- 2026-11-20, iteration 1. First full set of parts, modelled on the assumed
  values below. Print all three parts. The test plate from 2026-11-19 comes
  first.

## Files to print

The print files are kept out of git in
`artifacts/enclosures/crowpanel-epaper-42-sen63c/iter1/`.

| File | What it is |
| --- | --- |
| `base_body.gcode.3mf`, `backrest.gcode.3mf`, `bottom_plate.gcode.3mf` | Sliced Bambu Studio projects, open and print |
| `base_body.3mf`, `backrest.3mf`, `bottom_plate.3mf` | Unsliced meshes in print orientation, to slice again with other settings |
| `base_body.step`, `backrest.step`, `bottom_plate.step` | Exact solids in print orientation, for any CAD tool |
| `snapshots/front.png`, `back.png`, `left.png`, `exploded.png` | Pictures of the assembled Thing |

## Print settings

All three parts were sliced on 2026-11-20 with Bambu Studio 02.09.01.70, the
same profiles and flags as the test plate. The test plate was sliced again
the same day on the same plate type, so `test_plate.gcode.3mf` in the same
folder matches these settings.

| Setting | Value |
| --- | --- |
| Printer | Bambu Lab A1 0.4 nozzle |
| Process | 0.20mm Standard @BBL A1, 0.20 mm layers, 2 walls, 5 top and 3 bottom layers, grid infill |
| Filament | Generic PLA @BBL A1 at 220 C nozzle, used for eSUN PLA+ |
| Plate type | Textured PEI Plate, bed 65 C |

The slices are set for the Textured PEI Plate, the plate that comes with
the A1, and the filament preset gives a 65 C bed on it. If your A1 has a
different plate fitted, change the plate type in Bambu Studio before you
print, so the bed temperature follows that plate. Changing the plate type
does not change the print times or the filament weights below.

| Part | Orientation on the bed | Infill | Supports | Print time | Filament | Of which support |
| --- | --- | --- | --- | --- | --- | --- |
| Base body | Upright as it stands on the desk, bottom face down | 40 % | Tree (auto), from the build plate only, 30 degree threshold | 3 h 45 min 40 s | 93.02 g | 12.88 g |
| Backrest | Lying on its back, so the latch arm flexes along the layers | 15 % | None | 21 min 28 s | 8.05 g | 0 g |
| Bottom plate | Underside down | 15 % | None | 30 min 51 s | 17.07 g | 0 g |

The three parts take about 4 h 38 min and 118 g of filament.

The base body is set to 40 % infill on purpose. It is the heaviest part
and sits low, so its weight is what keeps the Thing standing. At 15 %
infill it weighs 55.00 g and a push of 0.464 N at the panel top edge tips
it, which is under the 0.5 N the plan asks for. At 40 % it weighs 80.14 g
and holds 0.515 N.

The supports under the base body cover four areas. The slice was checked
face by face against the STEP and every downward face flatter than 30
degrees in these areas has support under it.

1. The underside of the cradle floor over the harness bay.
2. The roof of the sensor pocket.
3. The ceiling strips along the plate grooves and the underside of the right
   wall.
4. The ceiling of the solid right half, over the plate.

The ten grille slot tops are short 2.5 mm bridges and print without support,
which keeps the slots clean.

## Checks run on the model

- `cad model interference` on the assembly, 2026-11-20. No printed part
  shares volume with the panel, the sensor, the gasket or another printed
  part. The only shared volumes are between solids inside the CrowPanel
  vendor STEP itself.
- Panel sweep. The panel turns about its bottom front edge in the cradle
  from upright (0 degrees) back to its rest at 15 degrees, where the rear
  wall stops it. The check steps through 0 to 15 degrees one degree at a
  time. Nothing overlaps, and the least clearance is 0.200 mm, to the rear
  wall at 15 degrees. The bottom front edge rests on the cradle floor at
  every angle by design.
- Tipping, with the masses from the slices, the panel at 120 g ASSUMED and
  the sensor at its 22 g datasheet maximum. The whole Thing weighs 249.3 g
  with its centre of mass 45.8 mm above the desk.

| Check | Needed | Model gives |
| --- | --- | --- |
| Tilt forward before it tips | 10 degrees | 29.5 degrees |
| Tilt backward before it tips | 10 degrees | 28.2 degrees |
| Push at the panel top edge, backward | 0.5 N | 0.515 N, margin 1.03 |
| Push at the panel top edge, forward | 0.5 N | 0.542 N |

The push numbers take the feet as points at their centres. Flat feet that
tip about their outer edges give 0.619 N backward, a margin of 1.24. The
backward push still holds while the panel weighs at least 114 g, so weigh
the real panel in the print test.

The thin 1.03 margin is for a bump at the top edge only, pushing the panel
straight back or forward. Daily use does not push that way. The two keys and
the dial are on the left edge of the panel and press sideways, along the
111 mm width of the base. The part sheet gives that direction for the keys,
and for the dial it is presumed from the part type. Sideways the Thing tilts
45.0 degrees to the right and 47.9 degrees to the left before it tips, far
more than front to back.

## Assumed for iteration 1

Each value is a named constant in `params.py` or `assembly.py`, so iteration
2 changes one number.

| Item | Assumed value | Basis |
| --- | --- | --- |
| Jumper housing | 2.54 x 2.54 x 14.0 mm, standard Dupont single pin female | common Dupont housing, not measured |
| Room below the shell bottom edge for jumpers | 25 mm | board sheet socket face, bend not measured |
| Shell screw heads | M2.5 pan head, d 4.5, 1.8 tall past the back cover | not in source |
| Screw head relief | d 6.0, 2.5 deep | head plus clearance |
| Sensor gasket | 3.0 mm closed-cell foam tape, compressed to 2.0 mm by rigid stops | Sensirion asks for soft foam, no grade given |
| Sensor end pads and the pad under the sensor | 3.0 mm foam, compressed to 2.0 mm | same |
| Plug and bend below the sensor rim | 15 mm | JST GH plug plus bend, not measured |
| Sliding fits | 0.20 mm per side | middle of the research range, the test plate checks it |
| Rubber feet | 10 mm round, 3 mm tall stick-on | common size |
| PLA density | 1.24 g/cm3 | used only for the solid mass beside the slice mass, not in the eSUN sheet |
| Panel mass | 120 g | not in source, weigh it in the print test |
| Rubber foot mass | 0.5 g each | too small to matter |
| Foot contact for the push check | at each foot centre | the safe side of a 10 mm flat foot |

## Shopping list

| Item | Amount | Notes |
| --- | --- | --- |
| Closed-cell foam tape, 3 mm thick, adhesive one side | one short roll | gasket, two end pads and the pad under the sensor |
| Round stick-on rubber feet, 10 mm across, 3 mm tall | 4 | they sit in the four 1 mm recesses in the plate |
| 10 k resistors | 2 | I2C pull-ups from SDA and SCL to 3V3, soldered into the harness |
| Heat shrink tube | a short length | covers the resistor joints in the harness |

## Assembly

1. Cut the gasket from the foam tape as two frames and stick them on the
   sensor's opening face. Measure along the sensor from its inlet end and
   across from the long edge nearest the square inlet.
   - The inlet frame runs from 0 to 17.8 mm along the sensor, with a window
     from 2.3 to 16.4 mm along and 2.3 to 21.7 mm across.
   - The outlet frame runs from 30.1 mm to the far end, with a window from
     31.5 to 53.3 mm along and 1.9 to 23.7 mm across.
   - Leave the band from 17.8 to 30.1 mm bare. The two rigid stops in the
     pocket press on the sensor there and set the gasket squeeze. Do not use
     Sensirion's example foam shape. It covers that band and parts of the
     grilles.
2. Stick an end pad on the face of each of the two ribs at the ends of the
   pocket, and the third pad in the seat on top of the bottom plate under the
   sensor.
3. Solder the two 10 k resistors into the harness from SDA and SCL to 3V3,
   cover the joints with heat shrink, and keep the cable under 10 cm.
4. Plug the cable into the sensor. Turn the base over, push the sensor into
   its pocket from below with the openings toward the back wall, and route
   the cable up through the harness bay.
5. Slide the bottom plate in from the right end (seen from the front) on its
   rails until the ridge clicks past the right wall.
6. Stick the four rubber feet in the recesses under the plate.
7. Turn the base upright and slide the backrest tongue into its socket.
8. Plug the jumpers onto the panel's GPIO header. Hold the panel upright,
   set its bottom edge in the cradle and tip it back until the latch clicks
   over the shell top edge.

## Access

- **TF card.** It comes out of the top edge of the panel with nothing moved.
- **BOOT and RST.** From behind the base, push a pin into the two 3.2 mm
  holes in the back wall, left of the grilles when you face the back. Keep
  the pin square to the panel back. The panel and the backrest stay in
  place.
- **Panel.** Push the thumb pad on the latch head back, tip the panel forward
  to upright and lift it out of the cradle.
- **Sensor.** Turn the base over, pull the bottom plate out toward the right
  end by its thumb grip, lift the sensor out of its pocket and unplug the
  cable.

## Owner print test for iteration 1

Print the test plate first if it has not been printed yet. Each step says
the printer time from the slice or a rough hands-on time.

1. Print the bottom plate. 31 min printer time. Check that it lies flat on a
   table with no corner lifted, and note any warp.
2. Print the backrest. 22 min printer time. Check that the latch arm is
   whole and bends back by hand without a crack.
3. Print the base body. 3 h 46 min printer time. Check the corners of the
   bottom face for lift.
4. Remove the supports from the base body. About 15 to 30 min by hand. Note
   which of the four support areas was hard to clear, whether any support
   was left in the plate grooves, and whether the grille slots are open.
5. Slide the bottom plate in and out five times without the sensor. About 5
   min. Note how hard it slides and how clear the detent click feels.
6. Fit the gasket, pads and sensor, close the plate and stick on the feet
   (Assembly steps 1, 2, 4, 5 and 6). About 20 min. Press on the sensor through the grilles and note
   whether it twists or rocks in its pocket.
7. Fit the backrest, plug in the jumpers and seat the panel (Assembly steps 7
   and 8). About 10 min. Note whether the jumpers fit under the panel without
   bending hard, whether the panel sits flat against the rear wall, and how
   hard the latch is to click and to release.
8. Weigh the panel on a kitchen scale before you seat it, and the whole
   Thing after. About 5 min. The tipping check needs the panel at 114 g or
   more.
9. Stand the Thing on the desk, check that all four rubber feet touch it,
   and push the panel top edge gently forward and back with one finger. About 5 min. Note whether it rocks or tips
   before the base slides.
10. Press a pin into each 3.2 mm hole in the back wall, and take the TF
    card out and back in. About 5 min. Note whether BOOT and RST click and
    the card is easy to reach.

Measure for iteration 2, and post the results with the date on the tracking
issue.

| What | How |
| --- | --- |
| Plate slide force and detent feel | Push the plate with a kitchen scale until it moves, read the grams, and say whether the click is too weak, right or too strong |
| Backrest latch force | Push the thumb pad with a kitchen scale until the tooth lifts, read the grams |
| Panel seat and tip | Gap between the shell and the rear wall, rocking in the cradle, and the push result from step 9 |
| Jumper room | Height of the jumper housings and the wire bend below the shell, with a caliper |
| Sensor twist | Whether the sensor turns in its pocket under a finger push |
| Support removal | Time taken and the hardest of the four areas |
| Warping | Lift at any corner of the plate and the base body, with a feeler gauge or a caliper |
