# SEN63C mechanical and airflow design-in sheet

Research date 2026-11-19. Target is a printed PLA or PETG desk enclosure. The CrowPanel 4.2" e-paper board sits tilted in a wedge base. The SEN63C sits in its own vented chamber behind a wall that separates it from the ESP32-S3 board.

Every value carries a source tag from the table below. Values tagged **STEP** were measured by us from the Sensirion CAD file and are not dimensioned in the datasheet. Treat them as nominal geometry without tolerance. "not in source" means no Sensirion or JST document we read gives the value.

## Sources

| Tag | Document | Revision | URL | sha256 |
| --- | --- | --- | --- | --- |
| DS | SEN6x Datasheet, 61 pages | v1.0, Oct 2026 | https://sensirion.com/media/documents/FAFC548D/6AC50BC5/PS_DS_SEN6x.pdf | 34b8e5b69c3686c76955abe7781b52841aaff7bd67beed012c4655b4566bbeeb |
| MG | SEN6x Mechanical Design and Assembly Guidelines, 14 pages | v1.1, Nov 2026 | https://sensirion.com/media/documents/EA641247/6AFD5963/sensirion_mechanical_design_and_assembly_guidelines_SEN6x_EN.pdf | babb78420eb0fa721c14ba6d30a3255b9242cea6aeba59c797bdd0389a4989d0 |
| HI | SEN6x Handling Instructions | v1.1, Aug 2026 | https://sensirion.com/media/documents/13034ACB/6A7B333B/PS_AN_SEN6x_Handling_Instructions_D1.pdf | 923ff749673255ad5985e1cee30034b85d55cf081b0bff7a5f7f207e5b5ec58d |
| TE | SEN6x Testing and Evaluation Guide | v1.0, Jul 2026 | https://sensirion.com/media/documents/AE5564E4/6A58BFB5/PS_AN_SEN6x_Testing_And_Evaluation_Guide_D1.pdf | 070dc5fa12eff3e2c97d6b40e1d5ae1406ddeaddafe12878b4320b0ed64a8ae3 |
| TC | SEN6x Temperature Compensation and Acceleration app note, 22 pages | v1.1, Jan 2026 | https://sensirion.com/media/documents/C964FCC8/69709EC3/PS_AN_SEN6x_Temperature_Compensation_and_Acceleration_Application_No.pdf | 925a7c9e538837f5513081f83c6423e75489665902cdcb1d9d62ba9855c12d5b |
| STEP | SEN6x 3D model, internal name "175875-00_Publication 3D File SEN6x.STEP", SolidWorks 2023, AP203 | dated 2025-05-22 | https://sensirion.com/media/documents/0005B7E5/68C12844/PS_CD_SEN6x_D1.STEP | f72f93285def36a74cb6219e817ebbf5b43f1aa73ec37b8804d92918a25e8ada |
| RET | Example retainer STEP, internal name retainer_005 | not stated | https://sensirion.com/media/documents/6E25B2D8/682EF111/SEN6x_Example_Retainer.STEP | 3f35c711f1a4571ffbd94fb830565dd39a513a36705e2d5ce2f0374a1fc0caf5 |
| FOAM | Example retainer sealing foam STEP, internal name sealing_005 | not stated | https://sensirion.com/media/documents/0167F89C/682EF130/SEN6x_Example_Retainer_Sealing_Foam.STEP | f6905cd105c17c0623a879e74ec9908dba35d6b5a2fd80ec5d75379f2fde9557 |
| PP | Sensirion SEN63C product page, read through r.jina.ai | live page, 2026-11-19 | https://sensirion.com/products/catalog/SEN63C | page, no hash |
| JST | JST GH connector catalogue, read as text through r.jina.ai only | not stated | https://www.jst-mfg.com/product/pdf/eng/eGH.pdf | text only, no hash |

The vendor files are not kept in the repo. Fetch them again from the URLs above and check each SHA-256 before use. The STEP measurements were made with build123d and the `cad` CLI.

## Coordinate convention used in this sheet

The datasheet drawing (DS 5.1, Figure 8, p.56) dimensions the plan view from the corner at the inlet end.

- **x** runs along the 55.2 mm length. x = 0 is the short end nearest the two inlets.
- **y** runs across the 25.6 mm width. y = 0 is the long edge nearest the square inlet. The round inlet is near y = 25.6.
- **z** is height. z = 0 is the inlet and outlet face. z grows toward the connector side and ends at 21.3 at the connector-side rim.

The sensor STEP uses another frame. Its height axis is Y, with the inlet face at Y = 21.3 and the connector rim at Y = 0. The mapping is x = X + 27.6, y = Z + 12.8, z = 21.3 - Y.

The example retainer STEP (RET) and foam (FOAM) use plan coordinates mirrored in y. Their y equals 25.6 minus our y, because they are drawn looking at the inlet face from outside. Their round inlet centre sits at (6.7, 7.4), which is our (6.7, 18.2).

## 1. Outline, tolerances, mass and datum

| Item | Value | Source |
| --- | --- | --- |
| Length | 55.2 +0.2 / -0.4 mm | DS Fig 8, p.56 |
| Width | 25.6 +0.1 / -0.2 mm | DS Fig 8, p.56 |
| Height overall | 21.3 ±0.5 mm | DS Fig 8, p.56 |
| Shoulder height (2x) | 19.3 ±0.3 mm from the inlet face | DS Fig 8, p.56 |
| Connector-side rim | about 2 mm tall (21.3 minus 19.3), inset 1.2 +0.1 / -0.2 mm all round | DS Fig 8, p.56 |
| Side recess floor (4x) | 8 ±0.2 mm from the inlet face, reference dimension (15) | DS Fig 8, p.56 |
| Long-side recess | x = 19 ±0.2 to 29.9 ±0.2 | DS Fig 8, p.56 |
| Short-end recess | y = 14.7 ±0.2 to 19.2 ±0.2 | DS Fig 8, p.56 |
| Flatness of the inlet face | 0.5 mm | DS Fig 8, p.56 |
| Plan corner radii | R2.8 (2x) and R4.3 (2x). Edge radii R0.5 and R0.3 | DS Fig 8, p.56 |
| Bounding box | 55.2 x 25.6 x 21.3 mm, volume 25,125 mm³ | STEP |
| Mass | 18 min, 20 typ, 22 max g | DS Table 1, p.5 |
| Label area | 22 x 8 mm on the connector side | DS Fig 9, p.57 |

**Outline conflict settled.** The current datasheet drawing (DS p.56), the STEP and the product page (PP) all give 55.2 x 25.6 x 21.3 mm. The 55.5 x 25.6 x 21.5 figure matches no current Sensirion drawing we found. Its height is inside the tolerance band (21.3 + 0.5 = 21.8 covers 21.5), but its length is 0.1 past the upper limit (55.2 + 0.2 = 55.4, below 55.5). It reads as a rounded figure, not a drawing value. A pocket sized for 55.4 with 0.3 side clearance still takes a 55.5 sensor. Design the pocket for the maximum material size, 55.4 x 25.7 x 21.8 mm, from the tolerances above.

## 2. Inlets, outlet and fan

All three openings are on the same large face (z = 0). Air enters through the two inlets and leaves through the outlet (MG Fig 1, p.2).

| Opening | Position and size | Source |
| --- | --- | --- |
| Square inlet | x = 3.2 ±0.1 to 10.2 ±0.1, y = 2.8 ±0.1 to 9.8 ±0.1, about 7 x 7 mm at the outer chamfer. Centre line x = 6.7 ±0.1 | DS Fig 8, p.56 |
| Round inlet | Ø6 ±0.2 mm, centre x = 6.7, y = 18.2 ±0.2. STEP radius at the face 3.021 mm | DS Fig 8, p.56, STEP |
| Outlet | Ø20.5 ±0.3 mm, centre x = 42.4 ±0.3, y = 12.8 ±0.2 (width centre). Three arc slits around a hub. STEP radius at the face 10.271 mm | DS Fig 8, p.56, STEP |
| Effective inlet area | 56 mm² for both inlets together | MG Fig 1, p.2 |
| Effective outlet area | 148 mm² | MG Fig 1, p.2 |
| Fan location | under the outlet. The circular fan housing is visible on the connector side | MG Fig 1 rear view, p.2 |
| Fan speed | 4000 RPM, fixed | MG FAQ 4, p.11 |
| Fan blades | 5 | MG FAQ, p.11 |
| Flow rate | about 0.8 to 0.9 l/min | MG FAQ 5, p.11 |
| Fan speed warning | status bit reports a fan speed out of range | DS 4.3.1, p.17 |

The inlets sit at the inlet end (x below about 10.2). The outlet sits at the other end (x from 32.15 to 52.65). The clear gap on the face between the inlet group and the outlet edge is about 22 mm, derived from x = 10.2 and 42.4 minus 10.25.

## 3. Mounting features, fixing method and orientation

**Mounting features.** The drawing shows no screw bosses and no mounting holes (DS Fig 8, p.56). The sensor is retained by its outline, its shoulders and its side recesses.

**Fixing method.** Use a firm fixture with flexible foam decoupling, so no structure-borne sound reaches the housing. Acoustic foam or thin rubber is allowed (MG 3, p.9). Sensirion publishes an example snap retainer and sealing foam (MG Figs 10 and 11, p.9). MG states the example is for prototyping only and must be adjusted for seal ductility, thickness, material and process.

**Example retainer geometry (RET and FOAM, measured by us, mirrored y as explained above).**

| Feature | Value | Source |
| --- | --- | --- |
| Retainer bounding box | 75.5 x 45.9 x 17 mm, volume 7,329 mm³ | RET |
| Plate | 2 mm thick, 10 mm border around the 55.2 x 25.6 outline | RET |
| Plate opening over the round inlet | slot 7.06 mm wide, round end R3.53 centred on the inlet, running to x = 18.22. Area about 101 mm² | RET |
| Plate opening over the square inlet | rectangle x = 3.08 to 18.22, 8.14 mm wide. Area about 123 mm² | RET |
| Plate opening over the outlet | circle R10.37 centred on the outlet. Area about 338 mm² | RET |
| Snap hooks | 4 arms, 1 per side. Long-side arms 8 mm wide at x = 19.5 to 27.5. Short-end arms 3.5 mm wide, centred on the short-end recess. Inner face 0.3 mm clear of the outline. Hook tip reaches 1.2 mm inward | RET |
| Hook catch | 8.8 mm below the plate underside. This lands on the 8 ±0.2 mm recess floor and leaves about 0.8 mm for the foam (our reading) | RET, DS p.56 |
| Guide ribs | 4 ribs, 6 mm wide, 0.3 mm clear of the outline, 15 mm long with a lead-in at the tip | RET |
| Foam gasket | 55 x 25.4 x 0.9 mm, inset 0.1 mm from the outline, cut-outs that match the plate openings above. Volume 741.5 mm³ | FOAM |
| Foam material | not in source | |
| Foam compression | not in source | |

The example widens each inlet into a slot that runs inward along x, and keeps the inlet passages and the outlet passage apart by a foam web. The opening areas above are computed by us from RET. They all exceed the 56 and 148 mm² minima.

**Housing snap windows (STEP, not dimensioned in DS).** Six small windows sit on the side walls, two per long side and one per short end. Each frame is about 3.6 x 3.6 mm with an opening of about 2.4 x 3.0 mm. They span z = 11.6 to 15.2. These belong to the sensor's own housing. Do not rely on them for retention, the example retainer uses the 8 mm side recesses instead.

**Orientation (MG 2.2, Figs 4 to 6, pp.4 to 5).**

| Opening face points | Rating | Reason given |
| --- | --- | --- |
| Up | not OK | large dust particles can fall in |
| Down | marginal | dust accumulation and accelerated aging |
| Sideways, either rotation | OK | |
| Sensor lying on its side, both views | OK | |

Storage follows the same idea. Inlet and outlet must not face up while stored (HI 3, p.2).

## 4. Connector

| Item | Value | Source |
| --- | --- | --- |
| Sensor-side connector | ACES 51468-0064N-001 | DS Table 16, p.15 |
| Cable-side plug | ACES 51452-006H0H0-001 or compatible (JST GHR-06V-S) | DS Table 16, p.15 |
| Wire | AWG26 or thicker, at least 0.128 mm² | DS Table 16, p.15 |
| Cable length | 50 cm or less. Keep it as short as possible, under 10 cm, and/or well shielded | DS Table 16, p.15 and DS 3.1. MG FAQ 1, p.11 gives 0.5 m maximum |
| Pinout | 1 VDD, 2 GND, 3 SDA, 4 SCL, 5 GND or NC, 6 VDD or NC | DS Table 17, p.15 |
| Location | "at the side of the sensor adjacent to the air outlet" | DS Fig 1, p.15 |
| Pin 1 | toward the inlet end. Pin 6 toward the outlet end | DS Fig 1, p.15 |
| Face | on the connector side (z = 21.3), inside a recessed pocket in the hollow underside | STEP |
| Pocket depth | pocket face 6.35 mm below the rim. Connector front face about 6.44 mm below the rim. Recess floor 7.75 mm below the rim | STEP |
| Connector mouth | about 10.75 x 4.25 mm. Chamfered pocket about 12.0 x 5.45 mm | STEP |
| Connector body in plan | x about 17.8 to 28.5, y about 16.9 to 21.15. Near the round-inlet long edge, roughly mid-length | STEP |
| Mating direction | plug pushes straight in toward the inlet face, perpendicular to the connector side | STEP |
| Cable exit | out through the open underside, perpendicular to the connector side, then a bend | STEP |
| GHR-06V-S housing | pitch 1.25 mm. Dimension A 6.25 mm and B 8.75 mm for 6 circuits. Drawing labels 5.7, 4.15 and 4.25 mm read from text, check the PDF before use | JST p.4 (text only) |
| GH wire range | AWG30 to AWG26, insulation OD 0.76 to 1.0 mm | JST p.2 (text only) |
| GH temperature range | -40 to +105 °C | JST p.2 (text only) |
| Plug height above the rim when mated | not in source | |
| Minimum cable bend radius | not in source | |
| Clearance behind the connector side for plug and bend | not in source | |

## 5. Enclosure guidance from the sources

**Coupling to ambient (MG 2.1, p.2, Figs 2 and 3, pp.3 to 4).**
- The ideal mount connects the openings directly to ambient with no channel.
- If a channel is needed, keep its length d short. A maximum length is not in source.
- Channel opening areas must be Ain ≥ 56 mm² and Aout ≥ 148 mm².
- Increase both areas when a fine mesh is used. The mesh open-area ratio or the size increase is not in source.
- Seal the inlets from the outlet, and seal both from the device interior. The figures show a soft sealing material.
- The device interior must sit at ambient pressure (Pint = Pamb).
- Any airflow restriction proportionally reduces the PM count rate.

**Recirculation.** The sources require the inlet and outlet channels to be sealed from each other (MG 2.1). A minimum spacing between outside inlet and outlet grilles, or a rule against exhaust re-entry outside the housing, is not in source.

**External airflow (MG 2.3, Fig 7, p.6).** If external flow exceeds 1 m/s, isolate the sensor. A duct-air mount is recommended in that case.

**Thermal (MG 2.4, Fig 8, p.7, and MG 3, p.9).**
- Over-temperature at the sensor should stay below 5 K.
- Place the sensor as far as possible from heat sources and below them.
- Do not thermally insulate the sensor.
- The module compensates its own self-heating. Heat from the rest of the device needs the TC procedure, and a residual uncertainty remains.
- TC 2.1.3, p.8 says to tune the device in its final orientation and normal mode, sitting on a surface like the real one. For an indoor air quality monitor it suggests a wooden plate to simulate a desk.
- TC Table 1, p.10 gives acceleration presets. Light (IAQM) is T1 100, T2 300, K 20, P 20. Middle is 100, 600, 50, 20. Strong (air purifier) is 250, 550, 150, 20.
- The SHT4x heater draws 200 mW for 1 s when used (MG FAQ 6, p.11).

**Sunlight (MG 2.5, Fig 9, p.8).** Shade the sensor. Direct light entering the inlets or outlet affects PM readings.

**Materials and chemicals (HI, pp.1 to 2).** Avoid cleaning agents, solvents and outgassing materials near the openings. HI names plastics, rubber seals and conformal coatings as possible sources. Mount the sensor after other assembly materials have cured, if possible. Do not put stickers on the sensor inside packaging.

**Vibration and acoustics.**
- Avoid strong vibration, as it may affect performance (TE, p.2).
- A numeric vibration limit is not in source.
- The fan base frequency is about 66.67 Hz. Blade pass is 333.34 Hz with harmonics at 666.67 and 1000 Hz. Braces give about 200 Hz. The dominant frequency is 1000 Hz (MG 4, p.10, FAQ 2 and 3, p.11).
- Avoid loose parts, stiffen panels, decouple with soft foam or PU rubber and use soft rubber feet (MG 4, p.10).
- Acoustic emission is at most 24 dB(A) at 0.2 m (DS Table 1, p.5).

**Operating environment (DS Table 10, p.11 and Table 8, p.9).** Recommended 10 to 40 °C and 20 to 80 %RH. Short-term limits -10 to 50 °C and 0 to 90 %RH. Maximum dew point 35 °C for SEN63C. Never condensing. Pressure 85 to 110 kPa without compensation.

**Sensor response (DS Table 3, p.6).** Temperature accuracy ±0.45 typ, ±0.7 max °C. RH accuracy ±4.5 typ, ±6 max %RH. Response time under 60 s for temperature and under 20 s for RH.

## 6. Supply

| Item | Value | Source |
| --- | --- | --- |
| VDD | 3.15 min, 3.3 typ, 3.6 max V | DS Table 12, p.13 |
| Ripple | 100 mV max | DS Table 12, p.13 |
| Idle current, averaged over 5 s | 1.7 mA typ | DS Table 12, p.13 |
| Measurement mode current after the first 60 s, averaged over 5 s | 75 mA typ, 100 mA max | DS Table 12, p.13 |
| Peak current, 2 ms pulse | 140 mA typ, 200 mA max | DS Table 12, p.13 |
| Product page average | 80,000 µA | PP |
| Current during the first 60 s | not in source as a separate figure | |

## 7. Rules for our chamber

Each rule cites its source. Where the rule is our design choice built on a source, it says so.

1. Model the sensor pocket at 55.2 x 25.6 x 21.3 nominal and clear the maximum material size 55.4 x 25.7 x 21.8. (DS Fig 8, p.56)
2. Mount the sensor so the inlet and outlet face is vertical and faces out through a side or rear wall of the chamber. Never face it up. Avoid facing it down. (MG 2.2, Figs 4 to 6, pp.4 to 5)
3. Put the opening face directly against the chamber wall, with grilles over each opening and no long duct. (MG 2.1, p.2)
4. Give the inlet grille at least 56 mm² of open area and the outlet grille at least 148 mm². Add more area when a fine mesh or a printed grille with thin bars covers them, because the sources ask for an increase but do not size it. (MG 2.1, Figs 2 and 3, pp.3 to 4)
5. Seal the inlet pair from the outlet with a soft gasket on the opening face. A foam web between the inlet group and the outlet, as in the example foam, does this. (MG 2.1, Figs 2 and 3, MG 3 p.9, FOAM)
6. Seal both openings from the chamber interior, so the fan only draws outside air. (MG 2.1, pp.2 to 4)
7. Vent the sealed interior so its pressure matches ambient. A small opening that sits away from the grilles works for this, and its size is not in source. (MG 2.1, Pint = Pamb)
8. Keep the outside inlet grille and outlet grille separated so the exhaust does not blow straight back into the inlets. The minimum spacing is not in source. The sensor face itself spaces them about 22 mm apart. (MG 2.1, DS p.56, derived)
9. Place the sensor chamber below the ESP32-S3 board and as far from it as the layout allows, never above it. The dividing wall must not trap heat around the sensor. (MG 2.4, Fig 8, p.7)
10. Do not wrap the sensor in insulation or close foam around its sides. Foam belongs only at the sealing face and the decoupling points. (MG 3, p.9)
11. Hold the sensor firmly through soft foam or thin rubber. Do not make hard plastic to plastic contact that can carry fan vibration. (MG 3, p.9 and MG 4, p.10)
12. Retain the sensor on its 8 mm side recesses, the way the example retainer does, and leave about 0.3 mm side clearance to the outline. Adjust the catch height for the gasket thickness. The example values are for prototyping only, so tune them for PLA or PETG on the bench. (DS p.56, RET, MG 3 p.9)
13. Stiffen thin printed panels near the sensor and avoid loose parts, because the dominant fan tone is 1000 Hz. Use soft rubber feet on the enclosure. (MG 4, p.10)
14. Shade the grilles from direct light. No line of sight from a window or lamp into the openings. (MG 2.5, p.8)
15. If the desk unit could sit in airflow faster than 1 m/s, such as under a fan or vent, shield the grilles. (MG 2.3, p.6)
16. Route the cable from the open underside with the plug pushed in toward the inlet face. Pin 1 is toward the inlet end. Keep the cable under 10 cm and use AWG26. (DS Table 16 and Fig 1, p.15)
17. Leave free space behind the connector side for the GHR-06V-S plug and the cable bend. The plug height above the rim and the minimum bend radius are not in source, so check the clearance with a real cable on the bench.
18. Print the sensor chamber in a material that does not outgas solvents, and let the print and any glue cure fully before the sensor goes in. (HI pp.1 to 2)
19. After assembly, tune the temperature offset with the device in its final tilted position on a desk-like surface. Start from the Light (IAQM) acceleration preset. (TC 2.1.3, p.8 and Table 1, p.10)
20. Budget the 3.3 V rail for 100 mA average and 200 mA peak for the sensor alone. (DS Table 12, p.13)

## Open items not covered by the sources

- Tolerances on the STEP-only features (connector pocket, connector position, housing snap windows).
- Plug height when mated, cable bend radius and clearance behind the connector side.
- Maximum channel length d, and the mesh open-area factor.
- Minimum outside spacing between inlet and outlet grilles to avoid recirculation.
- Size of the pressure equalisation vent.
- Foam material, hardness and compression for the gasket.
- A numeric vibration or shock limit.
- Any guidance for FDM printed parts, PLA or PETG outgassing, or wall thickness.
- The JST GH drawing values were read only as text through r.jina.ai. The labels 5.7, 4.15 and 4.25 mm need a check against the PDF drawing.
