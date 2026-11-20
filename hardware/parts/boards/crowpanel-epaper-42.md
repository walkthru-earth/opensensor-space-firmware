# CrowPanel ESP32-S3 4.2 inch E-paper HMI, mechanical sheet

Board ELECROW CrowPanel ESP32-S3 4.2" E-paper HMI, model DIE07300S001, sold as v1.2.
This sheet is reusable by any hardware set that uses this board. It was first written for
the desk base in `hardware/enclosures/crowpanel-epaper-42-sen63c/`. The vendor files are not
kept in the repo, fetch them again from the source URLs and check the SHA-256.

All numbers are in mm. Every number below was read from geometry (STEP faces and
bounding boxes through `cad`, or Eagle `.brd` XML). Nothing was read from a picture.
"not in source" means neither file holds the value.

## Sources

| File | Origin | SHA-256 |
|---|---|---|
| board.stp (Creo, header date 2024-11-18) | Elecrow-RD/CrowPanel-ESP32-4.2-E-paper-HMI-Display-with-400-300, commit fa49941f (2025-03-25, initial commit) | 4c02b3e7cfaa6a49b5ff77cc5b47e9a8434f89119694b7022684ad74ba404a20 |
| board.brd (Eagle 9.6.2) | same commit | 8d628ef569f552545f6736c88e65d8b2835a571c06b7a266545d37f2331539d8 |
| board.pdf, board.sch | same commit, used for net names only | |

The STEP has 71 leaves. It models the PCB, panel, three printed covers, standoffs,
connectors, keys, dial and the ESP32 module. Passives, the regulator, the charger and
the CH340 are not modeled, so their heights are not in source.

## Frame used in this sheet

The viewer frame looks at the display face.

- Origin is the PCB lower left corner seen from the display face.
- x runs right, y runs up, z points toward the viewer.
- z = 0 is the PCB front face (display side). The PCB back (component side) is z = -1.6.
- From STEP, x = 53.5 - X, y = Z + 43.5, z = Y + 0.5.
- From Eagle brd (component side is the back), x = x_e - 37.137, y = 117.015 - y_e.

On this frame the dial and the two keys are on the left edge, USB-C and BAT on the right
edge, the TF slot on the top edge, and the GPIO header plus BOOT and RST on the bottom
edge. The wide 10.3 panel border is at the top.

Which edge is "up" in normal use is not in source. If the owner holds it the other way
round, rotate 180 degrees with x' = 107 - x, y' = 87 - y.

`cad` camera presets use STEP axes, so their names do not match the board edges.
The snapshot file names below use board edges.

## Envelope

| Case | x | y | z | Size |
|---|---|---|---|---|
| With printed shell | -2.449 to 107.15 | -0.15 to 87.15 | -8.1 to 1.7 | 109.599 x 87.3 x 9.8 |
| Bare PCB stack (no shell) | -2.449 to 107 | 0 to 87 | -8.1 to 1.7 | 109.449 x 87 x 9.8 |

The x minimum is the dial wheel. The z minimum is the BOOT and RST tact plungers, which
stand 0.5 past the bottom cover. With the brd TF footprint the y maximum is 87.37.

## PCB

| Item | Value |
|---|---|
| Outline | 107 x 87, x 0 to 107, y 0 to 87 |
| Thickness | 1.6 (z -1.6 to 0) |
| Corner radius | 3.0 in STEP, arcs about 2.97 to 2.99 in brd |
| Mounting holes | 6 x d3.2 plated, pad d5 |
| Corner hole centres | (4, 4), (103, 4), (4, 83), (103, 83) |
| Middle hole centres | (23.5, 43.5) and (83.5, 43.5) in STEP, (23.493, 43.509) and (83.493, 43.509) in brd |
| Panel FPC slot | x 44.35 to 83.35, y 81.5 to 83, width 1.5, R0.75 ends |

## Display panel

| Item | x | y | z | Size |
|---|---|---|---|---|
| Glass | 8 to 99 | 5 to 82 | 0.5 to 1.7 | 91 x 77 x 1.2 |
| Active area (face step) | 11.1 to 95.9 | 8.1 to 71.7 | 1.5 to 1.7 | 84.8 x 63.6 |

- Pixel pitch from the active area is 0.212 for 400 x 300.
- Border from glass edge to active area is 3.1 at left, right and bottom, and 10.3 at the top.
- The STEP leaves a 0.5 gap between the PCB front (z 0) and the glass back (z 0.5). What fills it (tape or foam) is not in source.
- The panel FPC tail wraps through the slot and lies on the back at z -1.9 to -1.6, x 45.5 to 82.2, y 48.9 to 82.78.

## Printed shell (three covers)

| Layer | z | Thickness | Plate outline |
|---|---|---|---|
| Top cover | 0.5 to 1.5 | 1.0 | 107.3 x 87.3, x -0.15 to 107.15, R3.15 |
| Middle cover | -5.6 to -1.6 | 4.0 | same outline |
| Bottom cover | -7.6 to -5.6 | 2.0 | same outline |

Top cover

- Window 91.2 x 77.2, x 7.9 to 99.1, y 4.9 to 82.1.
- FPC notch x 45.5 to 61.5, y 82.1 to 83.
- No holes. The glass face sits 0.2 proud of it. How it is held is not in source.

Middle cover holes are d4.75 clearance around the standoffs. Its edge openings all run
the full 4.0 height (z -5.6 to -1.6).

| Opening | x | y | Note |
|---|---|---|---|
| Dial mouth at left edge | -0.15 | 37.763 to 49.272 | 11.509 wide |
| Dial pocket | -0.15 to 8.463 | 36.618 to 50.418 | R7.9 arc |
| EXIT key | -0.15 to 3.7 | 60.9 to 69.1 | |
| MENU key | -0.15 to 3.7 | 17.9 to 26.1 | |
| USB-C | 97.83 to 107.15 | 59.31 to 68.7 | 9.39 wide |
| BAT | 100.95 to 107.15 | 41.25 to 45.75 | 4.5 wide |
| TF mouth at top edge | 20.35 to 35.5 | 84.85 to 87.15 | 15.15 wide |
| TF pocket | 19.35 to 36.7 | 71.35 to 84.85 | |
| Header mouth at bottom edge | 19.2 to 47.8 | -0.15 to 0.85 | 1 x 45 degree chamfers |
| Header pocket | 20.2 to 46.8 | 0.85 to 17.85 | |
| ESP32 module pocket | 77 to 104.3 | 16.8 to 36.9 | |
| Tact switch pockets | about 8.1 x 4.9 | | |

Bottom cover

- d3.0 holes at the six PCB hole centres.
- d2.8 holes for BOOT at (69.805, 5.5) and RST at (83.5, 5.5).
- d1.5 LED window at (57.8, 4.0).
- Header notch, mouth x 19.2 to 47.8, inside x 20.2 to 46.8, running from the edge to y 10.5.

## Standoffs and screws

- Six M2_5X3_5 standoffs, body d4.5, z -5.1 to -1.6 (3.5 tall).
- Boss d3.0 sits in the PCB hole from z -1.6 to -0.4.
- Bore d2.1 (M2.5 thread).
- There is a 0.5 gap from the standoff end (z -5.1) to the bottom cover inner face (z -5.6).
- Screws are not modeled. Length and head type are not in source.

## Controls

All press directions are in the board plane. They are not on the back face.

| Control | Body | Actuator | Past PCB edge | Past shell edge | Press |
|---|---|---|---|---|---|
| Dial XB-TM-2024A (K5) | x -2.449 to 6.55, y 36.918 to 50.082, z -3.8 to -1.0 | wheel d13.4, z -3.5 to -2.15 (1.35 thick), centre (4.9, 43.5) derived | 2.449 (STEP), 2.27 (brd) | 2.299 | rotate about z, press toward +x |
| EXIT key (K4, IO1) | x -1.05 to 2.45, y 61.1 to 68.9, z -5.1 to -1.6 | 1.5 x 3, x -1.05 to 0.45, y 63.5 to 66.5, z -4.092 to -2.592 | 1.05 | 0.9 | +x |
| MENU key (K3, IO2) | x -1.05 to 2.45, y 18.1 to 25.9, z -5.1 to -1.6 | 1.5 x 3, y 20.5 to 23.5, same x and z | 1.05 | 0.9 | +x |
| BOOT tact (K1) | x 67.555 to 72.055, y 3.25 to 7.75, z -8.1 to -1.6 | plunger through d2.8 bottom hole | | 0.5 below bottom cover | +z |
| RST tact (K2) | x 81.25 to 85.75, y 3.25 to 7.75, z -8.1 to -1.6 | plunger through d2.8 bottom hole | | 0.5 below bottom cover | +z |

- The dial wheel axis is normal to the panel, so the wheel edge turns in the left edge plane.
- Dial nets are IO4 DOWN, IO6 UP and IO5 CONF. That the press acts toward +x is presumed from the part type and is not in source.
- Key size 3 x 6 x 3.5 per the part label.

## Connectors

| Connector | Exit edge | Body | Mouth centre | Insert direction |
|---|---|---|---|---|
| USB-C (J2) | right, flush with PCB edge x 107 | x 99.7 to 107, y 59.56 to 68.5, z -4.81 to -0.61 (8.94 x 4.2) | y 64.03, z -2.71 | -x |
| BAT SH1.0 2 pin (B1) | right, flush at x 107 | x 103 to 107, y 41.5 to 45.5, z -4.6 to -1.6 | y 43.5, z -3.1 (cavity 3.4 x 2.4) | -x |
| TF socket (J1) | top, flush at y 87 in STEP, 0.37 over in brd | x 19.75 to 36.15, y 72 to 87, z -3.4 to -1.0 | card slot along the top edge | -y |
| GPIO header (U5) | bottom, face recessed | x 20.7 to 46.3, y 1.0 to 9.5, z -6.6 to -1.6 | see below | +y |
| Panel FPC B (U4, 24 pin 0.5) | internal | x 68.95 to 82.95, y 65.4 to 69.2, z -2.6 to -1.6 | | |
| Panel FPC A | internal, not in brd | x 47.07 to 61.07, y 48.9 to 52.7, z -2.6 to -1.6 | | |

- The USB-C shell reaches 0.99 into the PCB thickness in STEP (z top -0.61 against PCB back -1.6). Neither file has a PCB cutout for it, so it is a mid mount or the STEP is off. Caliper it.
- U4 carries the panel SPI nets IO45 CS, IO46 D/C, IO47 RES, IO48 BUSY, IO12 CLK and IO11 MOSI.

## GPIO header for the SEN63C cable

U5 is a 2 x 10 right angle SMD female socket, 2.54 pitch, part label `2×10PIN_2.54_WOTIE`.

- Housing x 20.7 to 46.3, y 1.0 to 9.5, z -6.6 to -1.6 (5.0 tall).
- The socket face points down (-y) at y 1.0. That is 1.0 inside the PCB edge and 1.15 inside the shell edge.
- Cavities are 0.7 square and 7.3 deep. A male plug inserts toward +y.
- Socket rows are 2.54 apart. The near row (next to the PCB) is at z -2.83. The far row is at z -5.37.
- Column centres are x 22.083 + 2.54 k for k 0 to 9, ending at x 44.943.
- PCB pad rows are at y 11.335 (pins 1 to 10) and y 16.335 (pins 11 to 20). Legs run behind the housing to y 17.

| Column x | 22.083 | 24.623 | 27.163 | 29.703 | 32.243 | 34.783 | 37.323 | 39.863 | 42.403 | 44.943 |
|---|---|---|---|---|---|---|---|---|---|---|
| Near row, z -2.83 | IO3 | IO9 | IO15 | IO17 | IO19 | IO21 | GND | GND | GND | GND |
| Far row, z -5.37 | IO8 | IO14 | IO16 | IO18 | IO20 | IO38 | 3V3 | 3V3 | 3V3 | 3V3 |

For the SEN63C, IO8 (SDA) is far row x 22.083 (pin 20), IO9 (SCL) is near row x 24.623
(pin 2), and 3V3 and GND share columns x 37.323 to 44.943, far row and near row. A plug
spanning columns 1 to 7 (x 22.083 to 37.323) covers all four signals. Plug body length
and cable bend radius are not in source.

## Tallest parts on the back (below PCB back z -1.6)

| Part | Lowest z | Height below PCB |
|---|---|---|
| BOOT and RST tacts | -8.1 | 6.5 |
| GPIO header U5 | -6.6 | 5.0 |
| Keys and standoffs | -5.1 | 3.5 |
| USB-C shell | -4.81 | 3.21 |
| ESP32-S3 shield can | -4.8 | 3.2 |
| BAT SH1.0 | -4.6 | 3.0 |
| Dial | -3.8 | 2.2 |
| TF socket | -3.4 | 1.8 |
| FPC connectors | -2.6 | 1.0 |

The front side holds only the panel and top cover, up to z 1.7.

## Heat sources (all on the back)

Extents are footprint extents from the brd, so heights are not in source unless noted.

| Part | x | y | Note |
|---|---|---|---|
| ESP32-S3 module U3 | 80.54 to 99.90 (pads), STEP board 78.2 to 104 | 17.03 to 36.62 (pads), STEP 17.9 to 35.9 | can x 79.2 to 95.2, y 18.9 to 34.9, z to -4.8. Antenna end x 95.2 to 104, near the right edge |
| U2 RY3420 buck | 68.42 to 71.82 | 43.72 to 46.72 | |
| L1 4.7 uH | 72.06 to 78.06 | 42.78 to 49.17 | |
| U6 4054 Li-ion charger | 97.92 to 101.12 | 50.69 to 54.23 | warm only while charging |
| U1 CH340C | 82.51 to 88.96 | 50.08 to 60.08 | active on USB |
| D2 S2M diode | 89.68 to 96.88 | 39.64 to 43.83 | |
| L2 47 uH (panel boost) | 54.91 to 59.06 | 29.97 to 34.17 | |
| D1 red LED | 56.50 to 59.16 | 3.47 to 4.75 | under the d1.5 bottom window |

The warm cluster sits at x 68 to 104, y 17 to 60, on the right half of the back. The
header and the SEN63C cable exit are at the bottom left (x 20.7 to 46.3). For the sensor
chamber, keep the SEN63C inlet away from the right half and the antenna end.

## Snapshots

Rendered with `cad xbf snapshot` from `step/crowpanel-4.2-epaper.xbf`, display mode parts.

| File | Board view | cad preset |
|---|---|---|
| snapshots/front-display-face.png | display face | back |
| snapshots/back-face.png | component side with shell | front |
| snapshots/back-face-no-shell.png | component side, middle and bottom covers hidden | front |
| snapshots/left-edge-dial.png | dial and key edge | right |
| snapshots/right-edge-usb.png | USB-C and BAT edge | left |
| snapshots/top-edge-tf.png | TF edge | top |
| snapshots/bottom-edge-header.png | header edge | bottom |
| snapshots/iso.png | isometric | iso |

## STEP against brd

| Item | STEP | brd |
|---|---|---|
| Silk revision | none | V1.0, "SKU:DIE07300S", "ESP32 Display-4.2(E) inch" |
| Panel FPC connectors | two (A and B) and two LCD tail variants (`4_2LCD` L shaped to B, `4-2LCD-B` straight to A) | one, U4, which matches B |
| Middle hole centres | (23.5, 43.5), (83.5, 43.5) | (23.493, 43.509), (83.493, 43.509) |
| Corner radius | 3.0 | about 2.97 to 2.99 |
| TF socket over top edge | flush | 0.37 |
| Dial past left edge | 2.449 | 2.27 |
| USB-C shell | overlaps PCB thickness by 0.99 | no PCB cutout |
| Panel to PCB gap | 0.5, nothing modeled in it | not applicable |
| Tact plungers | 0.5 past bottom cover | not applicable |
| Top cover fastening | none modeled | not applicable |

The brd libraries come from a 4.3 inch V2.1 template and the "ESP32 Terminal 3.5 Inch"
pads, which supports that the brd is an early layout.

## v1.0 against v1.2

Elecrow publishes no v1.2 CAD. The brd silk says V1.0. The STEP is dated 2024-11-18 and
both files arrived in the repo's initial commit of 2025-03-25. The repo firmware folder
name mentions PCBA-V1.2A, but its bins report V1.0.

Judgment. The outline, the six hole positions, the edge connectors and the header almost
certainly carry over, because the printed shell in the same STEP fits them. Internal
details may differ on v1.2, notably which panel FPC connector is fitted. Treat every
edge position as good to about 0.2 until the caliper checks below are done, and leave
0.3 to 0.5 clearance around openings.

## Caliper list for the owner's unit

1. Which edge is up in normal use, and confirm the dial and keys are on the left with the display facing you.
2. Total stack thickness, glass face to tallest back part, bare and in the stock shell.
3. Glass protrusion above the top cover (STEP says 0.2) and the panel to PCB gap (STEP says 0.5).
4. Dial protrusion past the PCB edge (2.27 or 2.449) and wheel diameter (13.4).
5. Key actuator protrusion past the PCB edge (1.05) and actuator centre heights.
6. USB-C mouth centre from the PCB back and from the bottom edge (STEP y 64.03, z -2.71), and whether the shell sits in a PCB cutout.
7. TF socket overhang at the top edge (0 or 0.37).
8. Header face recess from the PCB edge (1.0) and the two row heights from the PCB back (1.23 and 3.77).
9. All six hole centres, especially the two middle holes.
10. Screw length and head type of the stock shell screws.
11. Which panel FPC connector is fitted (U4 at x 68.95 to 82.95 or the A position at x 47.07 to 61.07).
12. BOOT and RST plunger height past the PCB back (STEP says 6.5).

## Files

- `step/crowpanel-4.2-epaper.stp` is an unmodified copy of the Elecrow STEP (STEP axes, not the viewer frame).
- `step/crowpanel-4.2-epaper.xbf` is the same assembly converted for `cad`.
- Scripts that produced the numbers are in `../../board-scripts/` (`brd_parse.py`, `list_parts.py`, `faces.py`, `to_viewer.py`), with outputs `viewer_table.md`, `parts.txt`, `faces1.txt` to `faces4.txt` and `brd_u5.txt`.
