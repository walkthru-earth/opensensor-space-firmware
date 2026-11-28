<!-- SPDX-License-Identifier: CC-BY-4.0. Copyright Walkthru.Earth. -->

# Waveshare ePaper-3.97 and SEN63C desk stand, design

Status, 2026-11-26. The owner printed and fitted the shell, bezel and sensor
bar and reported the fit as right. The shell now carries a battery shelf for
the pack and the RTC coin. The owner printed the plate with the shelf and
reported it good on 2026-11-28.
Every number here comes from
`params.py` in this folder, which is the source of truth. If the two disagree,
`params.py` wins and this file is wrong.

**Note, 2026-11-26.** The owner asked for a fully autonomous design with no
questions. So this set skips the owner approval step and the test plate step
of `.claude/skills/hardware-design/SKILL.md` (steps 5 and 6) and goes straight
to the model. Every open point is settled below as a marked call made by us.
The owner can change any call, and the first print does the work the test
plate would have done.

## Intent

The Waveshare ESP32-S3-ePaper-3.97 with a Sensirion SEN63C is the fourth
OpenSensor Thing. The firmware logs the SEN63C on GPIO43 SDA and GPIO44 SCL to
Parquet on the TF card every 10 seconds (bench record of 2026-11-20 in
`docs/boards/waveshare-esp32-s3-epaper-397/` on main). Today the bare board and
sensor lie on the desk. This stand gives them a home. It holds the board
tilted back 15 degrees, puts the sensor in the bottom of the case behind the
board with its openings in the back wall, and keeps USB-C, the TF card, the
rocker and the BOOT and PWR keys usable.

It is a desk prototype. Three printed parts, eSUN PLA+ on a Bambu Lab A1 with
a 0.4 mm nozzle, screwless. A shelf inside the shell holds the LiPo pack that
ships with the board and an RTC backup coin cell behind the board.

## Goals

- The display stands tilted back 15 degrees from vertical.
- The SEN63C sits below the ESP32-S3 and away from it, its openings facing the
  back wall and sealed to it, with grilles of at least twice the Sensirion
  minimum open area.
- No screws, glue or inserts. The owner assembles and opens it by hand.
- USB-C, the TF card, the rocker, BOOT and PWR all stay usable with the board
  in the stand.
- Every part prints on the A1 with no supports.
- No printed part loads the display glass.
- The shipped LiPo pack and an RTC backup coin cell ride behind the board,
  plugged into the BAT and RTC headers, with no reprint of the shell.

## Non-goals

- Not weatherproof, not for outdoor or field use, no UV rating.
- No battery charging or power management change. The TG28 on the board
  does that as it is.
- No firmware change.
- No copy of any geometry from the reference models.

## Anti-goals

- A sensor that reads warm or slow because it breathes board air, or because
  its own exhaust loops back into its inlets.
- A latch that carries a constant load, since PLA+ creeps.
- A bezel that presses the glass.
- Metal, magnets or foil near the module antenna.

## Shape

The shape follows two MakerWorld models, credited as inspiration only. No
geometry was copied from either. Every surface is modelled from our own
numbers.

- "Waveshare ESP32-S3 4.3 LCD stand with SEN66 sensor",
  https://makerworld.com/en/models/2605407 (REF in `params.py`).
- The ePaper-3.97 back case, https://makerworld.com/en/models/3256990.

What we took from the SEN66 stand, measured from its STEP.

| Item | Reference | Ours |
| --- | --- | --- |
| Tilt of the front face | 15 degrees, front normal (0, -0.966, 0.259) | 15 degrees, `TILT_DEG` |
| Overall W x D x H | about 109.2 x 48.5 x 77.2 | 110.9 x 52.6 x 76.9 |
| Front face lean over the height | about 18.8 | 20.6 |
| Depth at the top | about 29.6 | 32.0 |
| Back wall | 2.0 | 2.0, `WALL` |
| Layout | open front case with a bezel, sensor at the bottom back | the same |

Our depth is larger because the SEN63C stands with its 21.3 height along Y and
needs a plug zone in front of it, see Sensor bay.

## Frames

The same three frames as the `params.py` header.

- **Board frame**, from `hardware/parts/boards/waveshare-epaper-397.md`. The
  vendor STEP as it is. bx along the PCB 0 to 99.5, by up the PCB 0 to 60, bz
  out of the display. PCB front face bz 0, back face bz -1.6.
- **Case frame.** X across seen from the front, Y from front to back, Z up,
  Z 0 under the shell floor. Board bx equals case X. A board point maps to
  case (bx, by sin T - bz cos T, OZ + by cos T + bz sin T) with T = 15
  degrees and OZ = 13.0, so the PCB bottom edge sits 11.0 above the floor top.
- **Sensor frame**, from `hardware/parts/sensors/sen63c.md`. x along 55.2 from
  the inlet end, y across 25.6, z from the opening face (0) to the rim (21.3).
  In the case, sensor x runs along case X from 17.0, sensor y runs up case Z
  from 3.0 and the opening face looks toward +Y, the back wall.

## Constraints and source numbers

Full sources and hashes are in the two part sheets and in
`.claude/skills/hardware-design/materials-and-closures.md`. The numbers this
design rests on follow.

**Board** (BRD, board frame).

| Item | Value | Source |
| --- | --- | --- |
| PCB | 99.5 x 60 x 1.6 | BRD Outline |
| Display module | bx 0.32 to 98.32, by 1.99 to 58.23, glass top bz 1.35 | BRD Display |
| Active face | bx 9.72 to 96.12, by 4.19 to 56.03 (86.40 x 51.84) | BRD Display |
| Studs | M2.5 x 4 at (2.7, 2.7), (96.8, 2.7), (2.7, 57.3), (96.8, 57.3), OD 4.5, back end bz -5.6 | BRD Studs |
| USB-C | left edge, by 25.21 to 34.79, bz -4.85 to -0.69 | BRD USB-C |
| TF slot | left edge, by 6.28 to 22.38, bz -3.45 to -1.0 | BRD TF slot |
| BOOT and PWR | bx 71.2 to 76.7 and 81.2 to 86.7, plungers down to by -0.69, bz -3.4 to -1.2 | BRD Keys |
| Rocker | bx 72.03 to 86.03, lever end by 62.41, bz -4.2 to -1.6 | BRD Rocker |
| ESP32-S3 shield can | bx 74.73 to 92.33, by 22.1 to 37.9 | BRD Heat |
| PCB antenna | bx 93.1 to 98.8 | BRD Antenna |

**Sensor** (SEN).

| Item | Value | Source |
| --- | --- | --- |
| Outline | 55.2 x 25.6 x 21.3, largest 55.4 x 25.7 x 21.8 | SEN 1 |
| Square inlet | x 3.2 to 10.2, y 2.8 to 9.8 | SEN 2 |
| Round inlet | d 6.0 at (6.7, 18.2) | SEN 2 |
| Outlet | d 20.5 at (42.4, 12.8) | SEN 2 |
| Minimum open area | inlets 56 mm², outlet 148 mm² | SEN 2 |
| Connector | x 17.8 to 28.5, y 16.9 to 21.15 on the rim side, plug pushes toward the opening face | SEN 4 |
| Side clearance | 0.3 | SEN 7 rule 12 |
| Orientation | opening face vertical, never up | SEN 3 |

**Material and printer** (MAT, CADP).

| Item | Value | Source |
| --- | --- | --- |
| eSUN PLA+ elongation at break, XY | 4.14 % | MAT 1.1 |
| Allowed strain for a latch opened often | 1.2 % | MAT 1.2 |
| Starting latch arm | tapered 1.8 to 0.9, 6 wide, catch 1.0, 30 degree lead, 45 degree return | MAT 6.1 |
| Sliding fit, cover fit, latch gap | 0.20, 0.30, 0.4 | MAT 1.4, MAT 6.1 |
| Minimum wall | 0.8 | CADP Walls |
| Longest bridge | 5.0 | CADP Overhangs |
| Bed used | 246, inside the A1 256 cube | MAT 1.3 |

## Approach, decided calls

The owner delegated every choice, so there is no options vote. These are the
calls, each one a named constant in `params.py`.

1. **One piece shell, printed back wall down.** Floor, two side walls, top and
   back wall in one part, open at the front. Outer X -2.5 to 108.4, Y -6.59 to
   46.0 at the floor, Z 0 to 76.86. The inner width (X -0.5 to 106.4) centres
   the case on the active area, whose centre is 52.92, because the display
   border is 9.4 on the left and 2.2 on the right. That leaves 6.9 of air
   between the PCB right edge and the right wall, which keeps plastic away
   from the antenna.
2. **Bezel, printed face down.** It closes the front, frames the active area
   and holds the board from the front.
   - Window 0.25 outside the active area on every side with a 0.75
     chamfer, which leaves a 0.6 lip (three layers) between the chamfer and
     the pocket ceiling. The back pocket clears the display module by 0.3
     and stops at bz 1.65. A cut behind the front skin joins the key window
     to the pocket, so no thin sliver is left between them.
   - Front face at bz 3.0, the same plane as the shell front edges.
   - **Top latch.** An in-plane arm 18 long along X, root at X 62.0, tip at
     X 44.0, tapered 1.8 to 0.9, 7.0 deep in Y, over a 1.5 slot. Its tooth
     (X 44.4 to 48.4, 1.0 catch over the top wall inner face) clicks up into
     a window in the top wall (X 44.0 to 48.8, 4.6 deep in Y), with a 2.0
     strip of top wall left in front of the window. Lead 30 degrees. The
     return face is 30 degrees from case Y, which is 45 degrees from vertical
     as the bezel prints, so it prints without support. MAT 6.1 starts from a
     45 degree return, and the push-off force at 30 degrees
     is still about 1.06 times the deflection force (DES Latch in
     `params.py`). Root strain 0.51 % against the 1.2 % limit (Bayer tapered
     arm, MAT 4.1). The arm front is flush with the bezel face, so it lies on
     the plate in print. Its root block starts inside the bezel, so no face
     of it looks down.
   - **Bottom tabs.** Two tabs, X 10.0 to 14.4 and 92.0 to 96.4, on bosses
     behind the bezel that stay under the PCB (boss top Z 6.0). They drop 1.8
     into slots in the 2.0 floor. The slots leave 0.2 in front of each tab and
     1.5 behind it, so the bezel can tilt in.
   - Insert the bezel bottom first with the tabs in the slots, swing the top
     in and push until the tooth clicks. To release, press the tooth down
     through the top window with a fingertip or a pin and swing the top out.
3. **Board seat on four cups.** Four cups sit behind the four studs on 3.0
   brackets from the side walls. Cup bore 5.0 (0.25 radial play around the
   4.5 studs, so the studs locate the board), outer 8.0, ring 1.0 over the
   stud end. The cup floor is 0.1 behind the stud ends (bz -5.7) and the
   bezel back is 0.1 in front of the PCB front (bz 0.1). The board is
   captured between them with 0.1 play each way, so the bezel never loads the
   PCB or the glass and the latch carries no constant load.

## Design

### Shell

Printed lying on its back wall, open front up.

- **Sensor bay.** The SEN63C stands in the bottom of the case behind the board,
  openings facing the back wall, inlet end on the left. See Sensor bay below.
- **Left wall notches.** USB-C and the TF card are reached through notches
  open to the front, so the board slides in past them. See Reachability.
- **Rocker notch** in the top wall, X 68.0 to 90.0, from the front back to
  bz -7.0.
- **Latch window** in the top wall, X 44.0 to 48.8.
- **Floor slots** for the two bezel tabs.
- **Vents** in the back, top and floor walls, see Air paths.
- **Battery shelf** on the back wall behind the board, see Battery shelf.
- **Feet.** Four 10 x 3 stick-on rubber feet on the flat floor underside,
  centres at (4.0, -0.5), (102.0, -0.5), (4.0, 40.0) and (102.0, 40.0). No
  recesses, the floor is flat there and the bottom side chamfers stop short
  of the feet.
- **Cup brackets.** Each bracket carries gussets that reach 1.0 past the
  bracket plate (`GUSSET_REACH`). The two lower gussets are trimmed by a 45
  degree plane in Y and Z that stays 0.8 off the sensor bar channel
  (`BAR_GUSSET_CLEAR` in `shell.py`), so the bar slides past them and the
  cut face prints without support.

### Bezel

Printed face down. Described under Approach item 2. Its chin below the PCB
bottom edge carries the key window, see Reachability.

### Sensor bar

Printed flat on its 4.3 face. A removable bar with a 4.3 (Z) x 2.5 (Y)
section that holds the sensor from the rim side. It goes in from the right
wall and slides along X through holes in both side walls and in two posts
that rise from the floor, one each side of the sensor pocket. Its left end
carries two barbed prongs that pass out through the left wall and snap
behind its outer face, with the whole 45 degree return outside the wall and its root 0.1 past
the wall face. The wall hole edge is 0.3 above the bar, so the return meets
it 0.4 to the right, and the head stops 0.1 off the right wall. This leaves
about 0.5 of play along X and nothing is preloaded. The
prongs flex outside the left post. A head (1.5 along X, 7.5 tall) sits 0.1
off the right wall outer face and gives a grip to pull it out. Squeeze the
two prongs together outside the left wall to release it, or pull the head
hard, the 45 degree return lets go.

- Bar Y 16.7 to 19.2, Z 7.0 to 11.3, which is sensor y 4.0 to 8.3, clear of
  the connector at sensor y 16.9 to 21.15.
- Post holes are 0.2 larger than the bar in Z each side and 2.9 deep in Y, so
  the bar sits on their front faces with 0.4 free behind it. The wall holes
  are 0.1 larger again.
- Two 3 mm foam pads sit between the sensor rim and the bar at sensor x 1.5
  to 13.0 and 42.0 to 53.7, either side of the connector. The bar compresses
  them to 1.5 at the nominal sensor. The bar bears on the rigid front faces of
  the post holes, so the pad squeeze is set by plastic, not by the prongs.
- Prongs 10.0 long, 1.5 thick, 0.6 barb. The barbs pass the post holes as
  well as the wall holes, so the 0.2 post hole sets the deflection at 0.4
  each. Root strain 0.90 % against the 1.2 % limit (constant section, MAT
  2.1). The barb catches the wall hole by 0.3.

### Battery shelf

Added on 2026-11-26 after the first three parts were printed. A first try, a
separate cradle that hung on three board studs, was dropped before printing.
The owner asked for a fixed shelf in the shell instead, which is simpler to
fit. The shelf is part of `shell.py`, so the shell needs a reprint. The bezel
and sensor bar do not change.

The pack is the Waveshare "Lithium Batt", label 454261, 3.7 V 1500 mAh, with
an MX1.25 2 pin plug, red + and black -. The owner measured 61.5 x 41 x 4.0
with a lead of about 55 from the pack edge to the plug. No coin cell ships
with the board. The RTC header (H4) feeds the TG28 VBACKUP pin, which can
trickle charge a rechargeable coin at 100 uA up to 2.6 to 3.3 V (TG28
datasheet 6.12.2, default 2.9 V). See Bill of materials.

Seen from the back, the three MX1.25 headers on the bottom edge read SPK, RTC,
BAT from left to right (Waveshare docs, onboard resources 10 to 12). In the
board frame that is BAT at bx 31.18 to 35.43, RTC at 42.68 to 46.93 and SPK
at 54.18 to 58.43, all plugged along -by.

- **Where the pack sits.** Flat behind the board, parallel to it, pack front at
  bz -9.1 with 0.5 play behind it to the rib fronts at bz -13.6. The deepest
  board parts over the pack are the MX1.25 headers at bz -5.0, so a 4.1 gap
  stays between the board and the pack for the lead. The pack spans bx 12.7
  to 74.7 and by 23.0 to 64.5 with its play, case Z 31.7 to 73.0, 1.9 under
  the top wall inner face.
- **Ribs.** Four 1.6 ribs grow off the back wall at X 15.0, 30.5, 48.5 and
  66.5, from case Z 29.6 (0.6 over the sensor pocket) to 72.0. The three on
  the right stand in the gaps between the back vents, the first sits left of
  them, so no vent is closed. Their front edges follow the board tilt, so the
  pack lies flat on them.
- **Feet.** Each rib carries a 2.0 foot under the pack lower edge (by 21.0 to
  23.0), reaching forward to bz -7.9. The pack stands on the feet and cannot
  slide down when it leans onto the board parts.
- **Fences.** A fence rib on each side keeps the pack from sliding sideways.
  The left fence at X 11.1 to 12.7 covers only the top 12 of the pack's left
  edge, so the lead leaves that edge freely below it. The right fence at X
  74.7 to 76.3 runs the full height and stands in the middle of a back vent
  gap. Both reach forward to bz -9.6.
- **Pack lead.** It leaves the left edge, turns forward into the 4.1 gap and
  runs across behind the board to BAT. From near the lower corner the route is
  39.5, from the middle of the edge 51.6, both inside the 55 lead. So the pack
  goes in with its lead edge on the left and the lead at or below the middle
  of that edge. Turning the pack face over moves a high lead low, so every
  pack fits one way.
- **Coin slot.** Right of the pack, behind the ESP32-S3 can, a 4.0 wide slot
  (X 76.3 to 80.3) for a 20 x 3.2 coin with welded tabs (ASSUMED), standing on
  edge. Its floor sits at case Z 15.0 to 16.6, 1.2 over the right bar post. A
  1.6 front stop, 8.0 tall, holds the coin's lower half, the back wall holds
  the rest. The coin drops in from the open front. The slot block joins the
  sensor bay right rib and stays under the back vents (top at Z 38.0 against
  46.0). The block corner behind the pack is cut away so the pack clears it.
- **Coin lead.** Over the front stop, down to the board and across behind it
  to RTC, about 65 on a square route. Buy a coin lead of 80 or longer.

### Sensor bay

- **Position.** Sensor X 17.0 to 72.2, Z 3.0 to 28.6, opening face at Y 42.0,
  rim at Y 20.7. The pocket clears the largest sensor plus 0.3 on each side,
  X 16.7 to 72.7, Z up to 29.0. Side ribs 1.6 thick reach 8.0 forward from the
  back wall.
- **Away from the heat.** The sensor top (28.7 at the largest sensor) is 5.6
  below the bottom of the ESP32-S3 shield can (case Z 34.35). The round inlet
  centre at case X 23.7 is 51 mm left of the can, which is on the right.
- **Openings face the back wall, never up.** Two gasket rings of 3 mm
  closed-cell foam, one round both inlets and one round the outlet, sit
  between the sensor face and the back wall. Each ring is 2.5 wide and is
  notched 0.3 round the stand-offs it meets. Four rigid 2.0 standoffs on the back wall at the sensor corners
  (sensor x 0.2 to 2.2 and 53.0 to 55.0, y 0.2 to 2.2 and 23.4 to 25.4) stop
  the sensor, so the rings are compressed to 2.0 by plastic, not by a clip.
  Each stand-off merges into the nearest side rib and into the floor or the
  top rib, so none of them stands alone on the back wall.
  The rings seal the inlets from the outlet and both from the case interior.
- **Grilles.** One hole in the back wall per opening, each 0.75 larger per
  side than the sensor opening. Square inlet 8.5 x 8.5 (72.3 mm²), round inlet
  d 7.5 (44.2 mm²), outlet d 22.0 (380.1 mm²). Inlets together 116.4 mm², 2.08
  times the 56 mm² minimum. Outlet 2.57 times the 148 mm² minimum. The
  `params.py` check counts them as open holes. If the shell adds bars across
  them, the bar area comes off that margin.
- **Under the sensor** a 1 mm foam strip on the floor top carries it, so no
  hard plastic touches the sensor anywhere.
- **Plug zone.** 10 mm clear in front of the rim for the JST GH plug and its
  bend. At the connector top the zone ends at Y 10.2, which is 1.4 behind the
  board's back plane (stud ends) at that height.

### Air paths

- Back vents, 12 slots 2.0 wide on 4.5 pitch, centres X 28.25 to 77.75, Z 46.0
  to 64.0. They sit high, well above the sensor grilles (Z 3 to 29).
- Top vents, 13 slots 2.0 wide in the top wall, case Y 24.0 to 40.0. They skip
  X 40 to 52 around the latch window and X 64 to 92 around the rocker notch.
- Floor vents, 15 slots 2.0 wide on 5.0 pitch, centres X 18.0 to 88.0, case Y
  4.0 to 13.0, in front of the bar posts (post front at Y 14.5).
- Room air enters under the board through the floor, rises past the board
  back and leaves through the top and back vents. This keeps the case
  interior at room pressure (SEN 7 rule 7) and carries the ESP32 heat up and
  away from the sensor. The sensor itself breathes only through its sealed
  grilles.
- The grilles face the back of the desk, which shades them from the room
  light.

### Reachability

| Control | How it is reached | Numbers (board frame) |
| --- | --- | --- |
| USB-C | notch in the left wall, open to the front | by 23.0 to 37.0, back to bz -6.52, sized for a 12.5 x 6.5 plug overmold plus 0.75 each side (call) |
| TF card | notch in the left wall, open to the front | by 7.8 to 21.0, back to bz -4.5 (11 wide card plus finger room) |
| Rocker | notch in the top wall | X 68.0 to 90.0, back to bz -7.0, 4 each side of the 14 wide rocker for a finger |
| BOOT and PWR | key window in the bezel chin, press the plungers up | X 68.0 to 90.0, by -8.0 to 1.5, below the display edge at by 1.99 |

The rocker lever end passes 0.8 under the top wall inner face while the board
goes in (assert in `params.py`).

### Printability

| Part | Orientation | Notes |
| --- | --- | --- |
| Shell | back wall down | grilles and back vents lie on the bed and print flat, the shelf ribs, fences and coin slot stand up as walls off the bed |
| Bezel | front face down | the latch arm lies flat and flexes along the layers |
| Sensor bar | flat on its 4.3 face | the prongs lie flat and flex along the layers |

Bridges, each at most 5.0 (CADP Overhangs).

| Bridge | Span |
| --- | --- |
| Sensor bar holes in the side walls | 4.9 |
| Sensor bar holes in the posts | 4.7 |
| Latch window in the top wall | 4.8 |
| Floor slots for the bezel tabs | 4.8 |
| Top and floor vent slots | 2.0 |
| Coin slot front stop | 4.0 |

Downward faces that would otherwise overhang, such as the cup brackets, get
chamfers instead. Bed faces are flat and bed edges get a small chamfer
against elephant foot. No part needs supports.

## Assumed

No source gives these. Each is a named constant in `params.py`, so one line
changes it after the first print.

| Constant | Value | Why | How to replace it |
| --- | --- | --- | --- |
| `ASSUMED_GASKET_T` | 3.0 | common closed-cell foam sheet, Sensirion gives no grade | measure the foam bought |
| `ASSUMED_GASKET_COMPRESSED_T` | 2.0 | about a third squeeze, set by the rigid standoffs | check the seal on the first print, change the standoff height with it |
| `ASSUMED_PAD_COMPRESSED_T` | 1.5 | half squeeze at the nominal sensor, 1.0 at the largest | press the sensor through the grilles, if it moves change this |
| `ASSUMED_STRIP_T` | 1.0 | thin foam strip under the sensor | measure the strip bought |
| `ASSUMED_PLUG_ZONE` | 10.0 | JST GH plug plus its bend, plug height and bend radius not in source | measure the mated plug and bend on the real cable |
| `ASSUMED_FOOT_D` | 10.0 | common stick-on rubber foot | measure the feet bought |
| `ASSUMED_FOOT_H` | 3.0 | same | same |
| `ASSUMED_BOARD_MASS_G` | 60 g | board mass not in source, used for the tipping check | weigh the board |
| `ASSUMED_BAT_MASS_G` | 28 g | a typical 1500 mAh pouch cell | weigh the pack |
| `ASSUMED_RTC_D`, `ASSUMED_RTC_T` | 20.0, 3.2 | an ML2032 with welded tabs | caliper the coin bought |
| `ASSUMED_RTC_MASS_G` | 3 g | a 2032 coin with tabs and lead | weigh it |
| `SLICE_MASS_G` | shell 68.00 g, bezel 8.25 g, bar 1.44 g | Bambu Studio slice at the Print settings below, not a weighed part | weigh the printed parts |

## Checks

`params.py` runs these asserts on import (`_checks`).

1. The case is centred on the active area within 0.1.
2. The bezel width, the shell height and the shell depth each fit the 246 bed.
3. The inlet grille holes add up to at least twice 56 mm² and the outlet hole
   to at least twice 148 mm².
4. The plug zone in front of the largest sensor stays 0.5 clear of the board
   back plane (stud ends) at the connector top.
5. The largest sensor top sits below the ESP32-S3 shield can, and the round
   inlet sits more than 40 left of it.
6. The rocker lever clears the top wall inner face by 0.5 while the board goes
   in.
7. The latch arm, fully deflected, clears the PCB top by 0.5, and its slot
   floor clears the display pocket by 0.5.
8. The latch arm root strain and the prong root strain are each at most 1.2 %.
9. The two prongs leave at least 0.2 between them when both are squeezed in.
10. The wall hole for the bar, the latch window, the floor tab slot and a vent
    slot each bridge at most 5.0. The post holes are smaller than the wall
    holes, so they pass too.
11. The prong roots sit more than 1.0 left of the left post, so the prongs
    flex in free air.
12. The bezel tab bosses stay 2.0 under the PCB bottom edge.
13. The latch tooth fits on the arm and sits inside the top window with 0.2
    to spare.
14. The pads stay compressed at the largest sensor and never past their own
    thickness.
15. The pack front stays 0.5 more than a plug depth behind the deepest board
    part over it, and the feet stop 2.0 behind it. The fences stay 4.0 behind
    the rocker and the ESP32-S3 can.
16. The pack top sits more than 1.5 under the top wall inner face. The rib
    tops sit over the pack top and 2.0 under the top wall.
17. The lowest foot point sits over the rib bottom and in front of the sensor
    rim, and the rib bottom sits 0.5 over the sensor pocket.
18. Each rib sits inside the pack outline and 0.4 clear of every back vent,
    the right fence too. The coin block stays under the back vents. The left
    fence and the coin rib stay clear of the cup brackets.
19. The coin floor sits 1.0 over the bar post and 2.0 over the bar hole. The
    coin block sits 2.0 behind the board. The slot holds the coin with 2.0 to
    spare, and its front stop bridges at most 5.0.
20. The pack lead from the middle of its edge reaches BAT with 2.0 to spare.
    The coin lead route is 5.0 shorter than the lead to buy.

`shell.py` also probes the shelf ribs, feet, fences, coin floor and front
stop, checks that the pack space, the lead corner, the coin slot and the back
vents stay open, and fits a pack box and a coin cylinder with no clash.

`assembly.py` places the three parts with the board and sensor vendor bodies,
the pack and the coin, and runs the CAD checks. The results go in the README Check results section.

- `cad model interference` on the assembly with the vendor bodies.
- `cad model dfm --process printing --technology fdm --material pla` on each
  part.
- Tipping. The Thing must tilt at least 10 degrees each way before it tips,
  with the board at the assumed 60 g, the sensor at its 22 g maximum, the
  pack and coin at their ASSUMED mass and the printed parts at their sliced
  mass (`SLICE_MASS_G`). It also reports the
  push at the top edge that tips it.

## Print settings

The SEN66 reference was sliced at 0.16 mm layers, 2 walls and 15 % infill. We
keep the layer height and infill and add a wall.

| Setting | Value |
| --- | --- |
| Printer | Bambu Lab A1, 0.4 nozzle |
| Filament | eSUN PLA+ |
| Layer height | 0.16 mm |
| Walls | 3 |
| Infill | 15 % gyroid |
| Top and bottom shells | 6 and 4 layers |
| Supports | none |
| Plate | Textured PEI |

Orientation per part is in the Printability table.

Sliced on 2026-11-26 with Bambu Studio 02.09.02.62, A1 0.4 profile "0.16mm
Optimal @BBL A1" with the changes above and Generic PLA, each part alone on
the plate. `slice.py` builds those presets from the profiles installed with
Bambu Studio and runs the slices again. eSUN PLA+ prints close to Generic PLA, so the mass is a guide.

| Part | Time | Filament | Supports |
| --- | --- | --- | --- |
| Shell | 2 h 33 min | 68.0 g | none |
| Bezel | 23 min | 8.3 g | none |
| Sensor bar | 4 min | 1.4 g | none |
| Total | about 3 h 0 min | 77.7 g | |

All three parts also fit on one plate (`all_parts.gcode.3mf`), 2 h 57 min and
77.5 g, no supports and no slicer warning. The shell with the shelf was
sliced again on 2026-11-26. The shell without it took 2 h 7 min and 55.5 g.

## Bill of materials

| Item | Amount | Use |
| --- | --- | --- |
| Closed-cell foam sheet, 3 mm, EPDM or neoprene, adhesive one side | one small sheet | two gasket rings and two pads |
| Foam strip, 1 mm | about 55 x 25 | under the sensor |
| Stick-on rubber feet, 10 across, 3 tall | 4 | under the floor, see Feet |
| Sensirion SEN63C with its JST GH cable | 1 | the sensor |
| 10 k resistors and heat shrink | 2, if the harness has none yet | SDA and SCL pull-ups to 3V3, required per the board README |
| 3.7 V 1500 mAh LiPo pack, MX1.25 2 pin | 1 | ships with the "With Lithium Batt" and Kit versions of the board, lies on the shell battery shelf |
| ML2032 rechargeable 3 V coin cell with welded tabs on an MX1.25 2 pin lead, at least 80 long | 1 | RTC backup on the RTC header, not shipped with the board |

The coin is an ML2032 (lithium manganese, 3 V). Its charge voltage fits the
TG28 backup charger, 2.6 to 3.3 V. Never fit a CR2032, which must not be
charged. A LIR2032 is safe there but wants 4.2 V, so it only half charges. The
firmware does not turn the backup charger on today (TG28 register 0x18 bit
2), so the coin keeps the charge it was sold with. The MX1.25 plug is keyed,
but sellers wire leads both ways. Hold the coin plug next to the pack plug
before plugging it in, the red wire must sit on the same side, since both
headers carry the signal on pin 1 and GND on pin 2 (schematic H3 and H4).

## Assembly and maintenance

1. Cut two gasket rings from the 3 mm foam, 2.5 wide. One goes round both
   inlets, its hole at least the 8.5 square and the 7.5 round grille holes.
   The other goes round the outlet, its hole at least 22.0 round. Notch the
   rings 0.3 round the four corner stand-offs. Stick them on the sensor
   opening face.
2. Stick the 1 mm strip on the floor of the sensor bay and the two 3 mm pads on
   the sensor rim at sensor x 1.5 to 13.0 and 42.0 to 53.7.
3. Plug the cable into the sensor. Set the sensor in its bay from the front,
   openings to the back wall, inlet end on the left.
4. Push the sensor bar, prongs first, in through the right wall, the two
   posts and the left wall until both prongs click outside the left wall.
5. Lay the board face down on a soft cloth. Plug the pack lead into BAT, the
   right hand header of the three as seen from the back, and the coin lead
   into RTC, the middle header. Check the coin polarity first, see Bill of
   materials.
6. Lay the shell on its back wall, open front up. Drop the coin on edge into
   its slot right of the shelf, lead up over the front stop.
7. Lay the pack flat on the ribs, its lower edge on the feet, its lead edge on
   the left under the left fence, the lead at or below the middle of that
   edge. If the lead sits higher, turn the pack face over. Lay the spare lead
   flat in front of the pack.
8. Plug the sensor cable into the board. Lower the board onto its four cups,
   studs into the cups, USB-C and TF on the left, without pinching a lead.
9. Set the bezel bottom first with its tabs in the floor slots, swing the top
   in and push until the latch clicks.
10. Stick the four feet under the floor at the four corners.

To take it apart, lay it on its back, press the latch tooth through the top
window and swing the bezel out. Lift the board off the cups, unplug BAT and
RTC, and lift the pack and the coin out. Then squeeze the prongs outside the
left wall, pull the bar out to the right by its head and lift the sensor out.

Without the pack and coin, skip steps 5 to 7. The stand works the same.

## Decided by the owner

- Desk use, Bambu Lab A1 with a 0.4 nozzle, eSUN PLA+, screwless.
- The shape follows the two MakerWorld models as inspiration only.
- Fully autonomous design with no questions and no approval or test plate
  step (2026-11-26).
- Add a holder for the pack that ships with the board and for an RTC coin
  cell, pack 41 x 61.5 x 4.0 with a lead of about 55, measured by the owner
  (2026-11-26).
- A fixed shelf in the shell behind the board, not a separate cradle
  (2026-11-26).

## Calls made in this design (owner can change)

- 15 degree tilt, from the reference.
- Keys down and rocker up, the board frame as it is.
- One piece shell printed back wall down, bezel printed face down, separate
  sensor bar.
- Board captured on four cups behind the studs with 0.1 play front and back.
- Top in-plane latch plus two bottom tabs.
- Sensor inlet end on the left, away from the ESP32-S3 on the right.
- Grille holes 0.75 larger per side than the sensor openings, at least twice
  the minimum area, no fin between inlet and outlet on the outside.
- Gasket rings around each opening rather than Sensirion's example foam shape.
- USB-C notch sized for a 12.5 x 6.5 overmold.
- 0.16 mm layers, 3 walls, 15 % gyroid.
- Four shelf ribs in the back vent gaps, a 4.1 lead gap in front of the pack,
  0.5 of play each way.
- Pack lead edge on the left, a short left fence so the lead leaves under it.
- Coin slot right of the pack behind the ESP32-S3 can, the coin on edge.
- ML2032 for the RTC coin, on a lead of 80 or longer.
- Every other constant marked `call` in `params.py`.

## Left to the builder

- Fillets, chamfers and the exact shape of the cup brackets and posts, within
  the printability rules above.
- The cable route from the sensor connector to the board.

## Open items

- The board sheet caliper list, mainly the stud end, the USB-C height and the
  rocker lever end.
- Where the SEN63C cable meets the board. The board README puts GPIO43 and
  GPIO44 on the TX and RX header, and the STEP does not say which connector
  that is. Route the cable on the bench.
- Board mass, plug height and bend, foam grade and squeeze, all ASSUMED above.
- Whether the sensor exhaust reaches its inlets outside the back wall. The
  inlets and outlet are 22 apart on the sensor face. Sensirion gives no
  minimum spacing.
- Temperature offset tuning with the stand in its final pose on the desk, as
  the SEN63C sheet asks (SEN 7 rule 19).
- Where the pack lead leaves the pack. The shelf needs it at or below the
  middle of the edge on the left, which turning the pack over gives. The lead
  route through the 4.1 gap is checked as a length, not as a bent cable.
- Whether this board has the RTC header fitted. The schematic marks H4 as
  reserved and likely left off in later batches. With no header, leave the
  coin out.
- Battery operation of the board is not bench verified. The 2026-11-20 bench
  record logged `battery=0`.
- Pack and board warmth. The pack sits 4.1 behind the board parts and the
  coin behind the ESP32-S3 can. Their effect on the SEN63C temperature
  reading is not measured.

## Owner print test

Print times come from the 2026-11-26 slice. Post the results with the date
on the tracking issue.

1. Print the sensor bar. About 4 min. Check that both prongs bend
   in by hand without a crack and spring back.
2. Print the bezel. About 23 min. Check that it lies flat, the latch
   arm bends back by hand and the window edges are clean.
3. Print the shell. About 2 h 33 min. Check the back wall corners for
   lift, and that the grille holes, vent slots, bar holes and latch window
   printed open.
4. Dry fit the bar through the empty shell five times. About 5 min. Note how
   hard it slides and how clear the prong click is.
5. Dry fit the board and bezel with no sensor (Assembly steps 8 and 9). About
   5 min. Note whether the studs drop into the cups, whether the board rattles,
   how hard the latch clicks and releases, and whether the window lines up
   with the active area.
6. Try every control with the board in place. About 5 min. Plug USB-C, take
   the TF card out and back in, rock and press the rocker, press BOOT and PWR
   through the chin window. Note anything hard to reach.
7. Fit the gaskets, pads, strip, sensor and bar (Assembly steps 1 to 4). About
   20 min. Press on the sensor through the grilles and note whether it moves.
8. Weigh the board alone and the whole Thing. About 5 min.
9. Stand the Thing on the desk and tip it gently forward and back with one
   finger. About 5 min. Note whether it rocks or tips before it slides. The
   model says a 0.44 N backward push at the top edge tips it with the pack
   and coin in, so a firm push will. If
   that is too easy, print the shell with more walls or infill, or stick a
   small weight on the floor inside, in front of the sensor bay.

Measure and write on the tracking issue.

| What | How |
| --- | --- |
| Board mass | kitchen scale, replaces `ASSUMED_BOARD_MASS_G` |
| Board play on the cups | caliper or feeler gauge between the PCB front and the bezel back |
| Latch force and feel | too weak, right or too strong, and whether the tooth releases cleanly |
| Bar slide and prong click | too tight, right or too loose |
| Sensor hold | whether the sensor moves under a finger push through the grilles |
| Gasket squeeze | foam thickness after a day, through a grille, with a caliper |
| Plug room | gap from the plug and cable bend to the board back |
| Warping | lift at any corner of the shell and bezel |
| Board sheet caliper list | the numbers it asks for |

### Battery shelf test

1. Print the new shell, alone or on the three part plate. Check that the four
   ribs, the two fences and the coin slot printed clean and the back vents
   are open.
2. Check that the RTC header, the middle one of the three, is fitted on the
   board, and note where the pack lead leaves the pack. About 2 min.
3. Fit the pack, the coin and the board (Assembly steps 5 to 8). About 10
   min. Note whether the pack lead reaches BAT with slack, whether the coin
   seats in its slot, and whether the board drops onto the cups without
   touching the pack.
4. Click the bezel on and shake the Thing gently. About 2 min. Note whether
   the pack or the coin rattles, and whether the rocker still moves.
5. Unplug USB with the board running. About 5 min. Note whether it keeps
   running and whether the log shows `battery=1` in the `OS POWER` line.
6. Weigh the pack and the whole Thing, and repeat the tip test of step 9
   above. About 5 min.
