<!-- SPDX-License-Identifier: CC-BY-4.0. Copyright Walkthru.Earth. -->

# Waveshare ESP32-S3-ePaper-3.97, mechanical sheet

Board Waveshare ESP32-S3-ePaper-3.97 (SKU 33810), ESP32-S3-WROOM-1-N16R8 with an
800 x 480 SSD1677 e-paper panel. This sheet is reusable by any hardware set that
uses this board. It was first written on 2026-11-26 for the desk stand in
`hardware/enclosures/waveshare-epaper-397-sen63c/`. The vendor files are not kept
in the repo, fetch them again from the source URL and check the SHA-256.

All numbers are in mm. Every number below was read from geometry, as STEP leaf
bounding boxes and planar faces through `cad` and build123d. Nothing was read
from a picture. "not in source" means the vendor files do not hold the value.

Firmware pins, the SEN63C wiring on GPIO43 SDA and GPIO44 SCL and the key GPIOs
are in `docs/boards/waveshare-esp32-s3-epaper-397/README.md` on main. This sheet
covers the mechanics only.

## Sources

Read on 2026-11-26.

| File | Origin | SHA-256 |
|---|---|---|
| `ESP32-S3-EPAPER-3.97.zip`, the Waveshare structure archive | https://files.waveshare.com/wiki/ESP32-S3-ePaper-3.97/ESP32-S3-EPAPER-3.97.zip, linked as "ESP32-S3-ePaper-3.97 Structure" on the Waveshare resources page | b792c9deff606aeaa739dcf0ad5cb8d15734ba88571e179c084bc17528596fc9 |

The zip holds three files, all dated 2026-08-21 inside the archive. They sit
renamed in the git ignored folder
`artifacts/enclosures/waveshare-epaper-397-sen63c/vendor/`, together with the
`cad` conversions and the Sensirion files that the SEN63C sheet already lists.

| File in the vendor folder | Name in the zip or origin | SHA-256 |
|---|---|---|
| `waveshare-epaper-397.stp` | `ESP32-S3-EPAPER-3_97-20260821.stp` | a28e504d7a519a17988f713fbfd917ca8127e2310a577105ac29cf115fe36901 |
| `waveshare-epaper-397.dxf` | `ESP32-S3-EPAPER-3.97-20260821.dxf` | 1c2dbb3787fcee8dd27a630ca45899189bf5606cf2e63d6807a2f4364dffa316 |
| `waveshare-epaper-397-drawing.pdf` | `ESP32-S3-EPAPER-3.97-20260821.pdf`, vector drawing with no text layer | 48b5c61dc3615aec4df61ff95de9b788bd775665d87b9792e2e0abcc31f152f2 |
| `waveshare-epaper-397.xbf` | `cad step export` of the STEP above | f78a9886b825b41320f0620dca12fa5a3addebdc4fe096d3cbe70d30c5f7022b |
| `sen6x.step` | Sensirion SEN6x STEP, see `hardware/parts/sensors/sen63c.md` | f72f93285def36a74cb6219e817ebbf5b43f1aa73ec37b8804d92918a25e8ada |
| `sen6x.xbf` | `cad step export` of `sen6x.step` | 64d6e276f5d47d608d29388ef3fa3a60fa54c1e0cc3be030c7e577b77c6849b3 |
| `sen6x-sealing-foam.step` | Sensirion example sealing foam, see the SEN63C sheet | f6905cd105c17c0623a879e74ec9908dba35d6b5a2fd80ec5d75379f2fde9557 |
| `sen6x-sealing-foam.xbf` | `cad step export` of the foam STEP | f84cf833bb8cf665e5ab32dd9f63f614d3d5c07929216afecc9c6624b2b483f4 |

The `.xbf` hashes depend on the `cad` release that wrote them (0.11.13 here), so a
fresh conversion can hash differently. Check the STEP, DXF and PDF hashes.

The STEP holds 675 solid leaves. It models the PCB, the display module, the EVA
foam under the display, the four studs, the WROOM module with its shield can and
antenna trace, the connectors, the two tact keys, the rocker and most passives.
The TG28 PMIC, the regulators and the charger are not named in the leaf labels,
so their positions and heights are not in source.

## Frame

The vendor STEP as it is, with no transform.

- bx runs along the PCB, 0 to 99.5.
- by runs up the PCB, 0 to 60.
- bz points out of the display. The PCB front face (display side) is bz 0 and
  the back face (component side) is bz -1.6.

On this frame USB-C and the TF slot are on the left edge (bx 0), the BOOT and PWR
keys on the bottom edge (by 0), the rocker on the top edge (by 60) and the
module antenna at the right edge (bx 99.5). The desk stand uses this frame as
it is, keys down and rocker up. Which edge Waveshare means as up is not in
source, and the firmware turns the picture with its Auto rotation.

## Envelope

| Item | bx | by | bz | Size |
|---|---|---|---|---|
| Whole STEP | -0.01 to 99.51 | -0.69 to 62.41 | -5.6 to 1.35 | 99.52 x 63.10 x 6.95 |

The by minimum is the two key plungers. The by maximum is the rocker lever. The
bz minimum is the four stud ends. The bz maximum is the active display face.

## Outline

| Item | Value |
|---|---|
| PCB | 99.5 x 60, bx 0 to 99.5, by 0 to 60 (STEP box -0.01 to 99.51 and -0.01 to 60.01) |
| Thickness | 1.6, bz -1.6 to 0 |
| Corner radius | not measured |
| Mass | not in source |
| Board revision of the STEP against the owner's unit | not in source. The zip files are dated 2026-08-21 and no revision is marked |

## Display

| Item | bx | by | bz | Size |
|---|---|---|---|---|
| Module bounding box | 0.32 to 98.32 | 1.99 to 58.23 | -2.5 to 1.35 | 98.00 x 56.24 |
| Module back face (flat) | 1.7 to 98.32 | 1.99 to 58.23 | 0.35 | |
| Border top face | 1.7 to 98.32 | 1.99 to 58.23 | 1.30 | |
| Active face (glass top) | 9.72 to 96.12 | 4.19 to 56.03 | 1.35 | 86.40 x 51.84 |
| EVA foam under the module | 2.29 to 97.84 | 2.49 to 57.73 | 0.05 to 0.35 | two solids |

- Pixel pitch from the active face is 0.108 for 800 x 480.
- Border from the module edge to the active face is 9.40 on the left, 2.20 on the
  right, 2.20 at the bottom and 2.20 at the top. The wide border is on the left.
- A small step at bx 0.72 to 1.7, by 19.61 to 40.61 sits at bz 1.1 and 1.2 on the
  left edge of the module.
- The module solid reaches bz -2.5 behind the PCB. That is its FPC tail running
  to the ZIF connector on the back. The tail route was not traced.
- The glass sits on EVA foam 0.3 thick, so nothing hard lies between the glass
  and the PCB front. Any part that presses the glass presses it into foam.

## Studs

| Item | Value |
|---|---|
| Part | four `STUDS-M2_5XH4`, M2.5 x 4 |
| Centres | (2.7, 2.7), (96.8, 2.7), (2.7, 57.3), (96.8, 57.3), from box centres |
| Outer diameter | 4.5, box 0.45 to 4.95 around each centre |
| bz | -5.6 to -0.14. The back end at bz -5.6 is the deepest point of the board |
| Thread and screws | not measured, no screws are modelled |

## USB-C

| Item | Value |
|---|---|
| Receptacle | bx 0.15 to 7.68, by 25.21 to 34.79, bz -4.85 to -0.69 (9.58 x 4.16) |
| Inner tongue | bx 0.45 to 7.5, by 25.83 to 34.17, bz -4.55 to -0.99 |
| Mouth centre | by 30.0, bz -2.77, derived from the box |
| Edge | left, mouth 0.15 inside the PCB edge |
| Insert direction | +bx |

The receptacle top at bz -0.69 is 0.91 into the PCB thickness (back face bz
-1.6). That reads as a mid mount in a PCB cutout, but the PCB solid was not
checked for the cutout. Caliper it.

## TF slot

| Item | Value |
|---|---|
| Socket | bx 1.51 to 16.52, by 6.28 to 22.38, bz -3.45 to -1.0 (15.01 x 16.10) |
| Edge | left, card goes in at bx 1.51 |
| Insert direction | +bx |
| Push push or hinge type | not in source |

## Keys

Two `SWITCH-TS24CA` tact keys on the bottom edge.

| Key | bx | by | bz |
|---|---|---|---|
| Left key | 71.2 to 76.7 | -0.69 to 2.96 | -3.4 to -1.2 |
| Right key | 81.2 to 86.7 | -0.69 to 2.96 | -3.4 to -1.2 |

- The plungers point down (-by) and end at by -0.69, 0.69 past the PCB edge.
  They press toward +by.
- The board README names them BOOT (GPIO0, also the settings Menu key) and PWR
  (on the TG28 PMIC). Which of the two positions is BOOT is not in source. Read
  the silk on the unit.

## Rocker

| Item | Value |
|---|---|
| Part | `PRT0001` |
| Box | bx 72.03 to 86.03, by 50.41 to 62.41, bz -4.2 to -1.6 (14.0 x 12.0 x 2.6) |
| Lever end | by 62.41, 2.41 past the top edge |
| Function | not in the STEP. The board README lists Up, Down and Press on GPIO 4, 6 and 5, and this is the only other control, so it is presumed to carry them |
| Travel and press force | not in source |

## Heat

All on the back, on the right half of the board.

| Part | bx | by | bz | Note |
|---|---|---|---|---|
| WROOM module PCB | 73.68 to 99.18 | 21.0 to 39.0 | -2.36 to -1.63 | 25.5 x 18.0 |
| Shield can | 74.73 to 92.33 | 22.1 to 37.9 | -4.7 to -2.4 | leaf named `ESP32-WROOM-32E_SHIELDING_CASE_` |
| ESP32-S3 chip | 82.05 to 89.09 | 25.12 to 32.16 | -3.25 to -2.4 | 7 x 7 inside the can outline |
| SOP8 | 75.42 to 80.42 | 28.4 to 34.6 | -4.01 to -2.4 | inside the can outline |
| Crystal 3225 | 84.85 to 88.05 | 34.49 to 36.99 | -3.11 to -2.41 | inside the can outline |
| PMIC, regulators, charger | not in source | | | not named in the STEP |

The warm area is bx 74 to 93, by 22 to 38. A sensor that needs room air belongs
below by 22 and toward the left edge.

## Antenna

| Item | Value |
|---|---|
| PCB antenna trace (`TRACE_1_SUBMODEL-MINI-ANT-TYPEB`) | bx 93.1 to 98.75, by 22.25 to 36.89, bz -2.4 to -2.36 |
| Module PCB end | bx 99.18, 0.32 inside the board edge |

Keep plastic near the antenna thin and keep metal, magnets and foil away from it.
Leave the area past the right edge open where the design allows.

## Back side parts

Lowest point below the PCB back (bz -1.6) for every part that matters to a case.

| Part | bx | by | Lowest bz | Height below PCB |
|---|---|---|---|---|
| Studs | four, see Studs | | -5.6 | 4.0 |
| MX1.25 header (`_12_AF0`), left | 31.18 to 35.43 | 2.93 to 7.13 | -5.0 | 3.4 |
| MX1.25 header, middle | 42.68 to 46.93 | 2.93 to 7.13 | -5.0 | 3.4 |
| MX1.25 header, right | 54.18 to 58.43 | 2.93 to 7.13 | -5.0 | 3.4 |
| USB-C | 0.15 to 7.68 | 25.21 to 34.79 | -4.85 | 3.25 |
| Shield can | 74.73 to 92.33 | 22.1 to 37.9 | -4.7 | 3.1 |
| Rocker | 72.03 to 86.03 | 50.41 to 62.41 | -4.2 | 2.6 |
| FPC ZIF, two leaves | 28.91 to 35.41 | 43.2 to 59.3 | -3.6 | 2.0 |
| Part `EXTRUDED-560428` | 46.15 to 50.55 | 48.2 to 52.2 | -3.6 | 2.0 |
| TF socket | 1.51 to 16.52 | 6.28 to 22.38 | -3.45 | 1.85 |
| Keys | 71.2 to 86.7 | -0.69 to 2.96 | -3.4 | 1.8 |
| `D1-2-3` | 45.11 to 50.78 | 43.03 to 46.75 | -3.0 | 1.4 |
| Part `EXTRUDED-559526` | 1.5 to 4.1 | 41.95 to 45.35 | -2.7 | 1.1 |
| SOD323 diodes, two | 65.58 to 68.37 | 45.0 to 47.7 | -2.7 | 1.1 |
| Part `EXTRUDED-406969` | 63.65 to 66.95 | 19.3 to 22.1 | -2.5 | 0.9 |
| 0402 and 0603 passives | spread over the back | | -2.43 | 0.83 |

- Each MX1.25 header holds two Molex 1.25 pins at by 1.94 to 6.14, and its
  plug goes in along -by, from the bottom edge side. Which header carries
  what is in Headers and battery below. None of them brings out GPIO43 and
  GPIO44, see the board README.
- The front side holds only the display module, up to bz 1.35.

## Headers and battery

These facts are not geometry. They come from the Waveshare docs, the product
page and the schematic, read on 2026-11-26, and the owner's own measurement.

| Header | bx | Schematic | Pin 1 | Pin 2 |
|---|---|---|---|---|
| BAT, left in the board frame, right seen from the back | 31.18 to 35.43 | H3, lithium battery, PH1.25-2P | VBAT | GND |
| RTC, middle | 42.68 to 46.93 | H4, RTC backup battery, PH1.25-2P, reserved and likely left off in later batches | VBACKUP of the TG28 through R61 0R | GND |
| SPK, right in the board frame, left seen from the back | 54.18 to 58.43 | H2, speaker, PH1.25-2P | speaker | AGND |

- Order. The Waveshare docs onboard resources image shows the silk SPK, RTC,
  BAT from left to right seen from the back, callouts 10, 11 and 12
  (https://docs.waveshare.com/ESP32-S3-ePaper-3.97). Check the silk on the
  owner's unit.
- UART and I2C come out on an unpopulated pad row to the right of the
  headers seen from the back, labelled GP1 GP0 SCL SDA TX RX 3V3 GND 5V.
- Pack. The "With Lithium Batt" and Kit versions ship a 3.7 V 1500 mAh
  lithium polymer pack, label 454261, 5.55 Wh, 4.2 V charge limit, MX1.25 2
  pin plug, red + and black - (Waveshare product page,
  https://www.waveshare.com/esp32-s3-epaper-3.97.htm). The owner measured
  61.5 x 41 x 4.0 and a lead of about 55 from the pack edge to the plug
  (2026-11-26).
- RTC coin. No coin ships with any version. The TG28 backup pin can trickle
  charge a coin at 100 uA with a termination of 2.6 to 3.3 V, default 2.9 V,
  when register 0x18 bit 2 is set (TG28 datasheet section 6.12.2,
  https://files.waveshare.com/wiki/common/TG28_Datasheet_V1.0_en.pdf). So a
  rechargeable 3 V ML2032 suits it and a CR2032 must not be used. The
  PCF85063 RTC runs from the TG28 RTCLDO, not straight from the coin.

## Caliper list for the owner's unit

1. PCB outline, 99.5 x 60, and the corner radius.
2. Stud back end from the PCB back (STEP says 4.0, bz -5.6) and the four stud
   centres.
3. USB-C mouth centre height from the PCB back (STEP says 1.17, bz -2.77) and
   from the bottom edge (by 30.0), and whether it sits in a PCB cutout.
4. Rocker lever end past the top edge (STEP says 2.41) and its height from the
   PCB back.
5. Display glass face above the PCB front (STEP says 1.35) and the active area
   edges.
6. Board mass on a kitchen scale.
7. Which tact key is BOOT and which is PWR, from the silk.
8. Key plunger tip past the bottom edge (STEP says 0.69).
9. Which MX1.25 header or pad carries GPIO43 and GPIO44.
10. The SPK, RTC and BAT silk order, and whether the RTC header is fitted.
11. Where the pack lead leaves the pack (thickness measured 4.0 on 2026-11-26).
