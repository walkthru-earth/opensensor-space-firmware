# Materials, printers and screwless closures

Shared reference for the `hardware-design` skill. It was researched for the CrowPanel desk
base, so its examples name that set, but the filament, printer and closure numbers apply to
any printed part. Add a new filament or printer as its own section with sources.

## Owner's printers and filaments

| Printer | Nozzle | Build volume | Materials the vendor rates ideal | Source |
| --- | --- | --- | --- | --- |
| Bambu Lab A1 combo (AMS lite) | 0.4 mm | 256 x 256 x 256 mm | PLA, PETG, TPU, PVA. ASA and ABS not recommended | section 1.3 |

| Filament | Use | Source |
| --- | --- | --- |
| eSUN PLA+ | desk prototypes | section 1.1 |


Research for the printed wedge base that holds the Elecrow CrowPanel 4.2" e-paper board tilted about 15° back, with the Sensirion SEN63C in its own vented chamber. Printer and material for the first prototype are eSUN PLA+ on a Bambu Lab A1 combo with the stock 0.4 mm nozzle at 0.2 mm layers. Research date 2026-11-19.

Rules used in this file

- Every number carries the link it came from. Numbers that came only from a search result snippet, with the page not fetched, are marked "snippet".
- "Derived" means arithmetic on cited inputs. The inputs and the formula are shown so the result can be checked. A derived number is a starting point for a test coupon, not a validated limit.
- "not in source" means no fetched source gave the value.
- Clearance values are quoted as the source states them. Most sources do not say whether a clearance is per side or total. Where a source says per side, the text says so.

## 1. Baseline for this printer and filament

### 1.1 eSUN PLA+ datasheet values

Source is the official eSUN PLA+ TDS, Feb 2026, Version 1.0, linked as "TDS Download" from the eSUN PLA+ product page. [eSUN PLA+ TDS PDF](https://www.esun3d.com/media/esun/catalog/certification/product/PLA/PLA_-TDS-en.pdf), [eSUN PLA+ product page](https://www.esun3d.com/pla-pro-product/). The same values appear in the product page property table.

| Property | X-Y | Z | Test method |
| --- | --- | --- | --- |
| Tensile strength | 70.37 MPa | 36.36 MPa | GB/T 1040 |
| Elongation at break | 4.14 % | 2.92 % | GB/T 1040 |
| Flexural strength | 100.3 MPa | 56.4 MPa | GB/T 9341 |
| Flexural modulus | 3259.39 MPa | 2873.94 MPa | GB/T 9341 |
| Izod impact | 7.27 kJ/m² | 2.22 kJ/m² | GB/T 1843 |
| Heat distortion temperature | 53.78 °C at 0.45 MPa | | GB/T 1634 |
| UV resistance | NO | | |
| Glass transition, continuous service temperature | N/A in TDS | | |

Specimen conditions from the same TDS are 220 °C nozzle, 65 °C bed, 2 walls, 3 top and bottom layers, 100 % infill, 100 % fan, printed on a Bambu P1S with a 0.4 mm nozzle in OrcaSlicer 2.1.0 Beta. Recommended print settings are 210 to 230 °C nozzle, 45 to 60 °C bed, 100 % fan, under 300 mm/s, and drying at 50 °C for 8 to 12 h. [eSUN PLA+ TDS](https://www.esun3d.com/media/esun/catalog/certification/product/PLA/PLA_-TDS-en.pdf)

Why this matters. These are printed-specimen values, so they already include FDM layer effects. They are much lower in elongation than two older figures that still circulate.

- A third-party mirror of an older eSUN PLA+ sheet lists 20 % elongation and 1973 MPa flexural modulus. [plahunter mirror](https://plahunter.pages.dev/data-sheets/esun/pla-pla). The Multicomp Pro PLA+ datasheet on Farnell lists the same 20 % and 1973 MPa. [Farnell PDF](https://www.farnell.com/datasheets/4520126.pdf). These look like moulded-specimen values and are superseded by the Feb 2026 TDS for design here.
- An older 2016 eSUN sheet with 29 % elongation and 3642 MPa modulus appeared only in a search snippet and was not fetched.
- Fictiv lists generic "PLA 4-8 %" acceptable strain for snap fits. [Fictiv](https://www.fictiv.com/articles/how-to-design-snap-fit-components). For eSUN PLA+ printed parts this is optimistic, because the printed elongation at break itself is only 4.14 % in X-Y.

eSUN does not publish a snap-fit strain limit, a fatigue curve or a creep curve for PLA+. Those are not in source.

### 1.2 Allowable strain for eSUN PLA+ snaps (derived)

Inputs and rules

- Bayer, single brief snap, "partially crystalline materials may be stressed almost to the yield point, amorphous ones up to about 70% of the yield strain". For materials without a distinct yield point "the permissible strain ... is about half the elongation at break". For "frequent separation and rejoining, use about 60% of these values". [Bayer Snap-Fit Joints for Plastics](http://fab.cba.mit.edu/classes/S62.12/people/vernelle.noel/Plastic_Snap_fit_design.pdf)
- Printed PLA+ shows no published yield point in the eSUN TDS, so the half-elongation rule is applied here by analogy. This is a judgement call, not a Bayer statement about PLA.

| Case | Arithmetic | Result |
| --- | --- | --- |
| Linear elastic strain at tensile strength, X-Y | 70.37 / 3259.39 | 2.16 % |
| Linear elastic strain at tensile strength, Z | 36.36 / 2873.94 | 1.27 % |
| Single snap, X-Y arm, half of elongation at break | 4.14 × 0.5 | 2.07 % |
| Repeated opening, X-Y arm | 2.07 × 0.6 | **1.24 %** |
| Single snap, Z arm | 2.92 × 0.5 | 1.46 % |
| Repeated opening, Z arm | 1.46 × 0.6 | **0.88 %** |

Starting rule for this project. Design every snap that is opened in service to 1.2 % root strain or less, built in X-Y. Treat 0.9 % as the ceiling for anything that cannot avoid Z. Validate with a coupon (section 6.4).

### 1.3 Bambu A1 facts that bear on fits

- Build volume 256 × 256 × 256 mm, stainless steel 0.4 mm nozzle included. PLA, PETG, TPU and PVA are "Ideal". ABS, ASA, PC, PA, PET and fibre-filled grades are "Not Recommended". [Bambu A1 tech specs](https://bambulab.com/en/a1/tech-specs)
- The A1 FAQ says the open frame and lower chamber temperature reduce interlayer strength and raise warping for high-temperature filaments, and that ABS and ASA need ventilation the A1 does not provide. [Bambu A1 FAQ](https://wiki.bambulab.com/en/a1/manual/faq#can-a1-print-abs-and-asa)
- Bambu publishes no general fit or clearance table for the A1. The only official clearance number found is for cut connectors, "adjust the cut tolerance to 0.15–0.3 mm" and recalibrate flow on the A1. [Bambu wiki, splitting large files](https://wiki.bambulab.com/en/bambu-studio/manual/3d-print-large-files)
- Bambu Studio has X-Y hole compensation and X-Y contour compensation. Calibration is per filament, worked in 0.1 mm steps in the wiki example, and hole compensation applies only to closed paths. [Bambu wiki, XY hole and contour compensation](https://wiki.bambulab.com/en/software/bambu-studio/xy-hole-contour-compensation)
- Elephant foot compensation shrinks the first layer contour. The default is fine for general models. The wiki example measured 0.09 and 0.11 mm flare and set 0.10 mm. The value changes with the build plate type. [Bambu wiki, elephant foot](https://wiki.bambulab.com/en/software/bambu-studio/parameter/elephant-foot)
- Community, not official. One forum reply says "as long as 0.2mm tolerance hinges well you are doing pretty good", and the original poster reached 0.15 mm on a print-in-place tolerance test. No slicer setting stores a tolerance. [Bambu forum, print in place hinge tolerances](https://forum.bambulab.com/t/print-in-place-hinge-tolerances/111715)
- Line width defaults for the A1 0.4 mm profile are not in source. Prusa gives 0.45 mm extrusion width for a 0.4 mm nozzle and walls of 0.45, 0.9, 1.35 and 1.8 mm for 1 to 4 perimeters. [Prusa, modeling with 3D printing in mind](https://help.prusa3d.com/en/article/modeling-with-3d-printing-in-mind_164135/). Section 4 uses those wall steps, check them against the A1 profile in Bambu Studio.

### 1.4 General FDM clearance guidance

| Fit | Value | Source |
| --- | --- | --- |
| Loose, covers and adjustable parts | 0.30 to 0.40 mm | [UF Makerspace, Prusa MK4 and XL](https://makerspace.uflib.ufl.edu/services/3dprocess/recommended-software-for-3d-printing/designing-for-3d-printing-tolerances-on-the-mk4-and-xl/) |
| Standard sliding, tabs, rails, enclosures | 0.20 to 0.25 mm | same |
| Snug, locating features | 0.15 to 0.20 mm, "very dependent on surface qualities" | same |
| Print in place | larger end of sliding and loose | same |
| FDM snap-fit connectors | 0.5 mm | [Hubs snap fits](https://www.hubs.com/knowledge-base/how-design-snap-fit-joints-3d-printing/) |
| Snap fins to hole, balance of ease and tightness | 0.4 mm | [OpenAT snap fits](https://makersmakingchange.github.io/OpenAT_Design_Guide/Design_Elements_For_3D_Printed_Parts/Snap_Fits.html) |
| Close-fit snap, tight fit, slide and pivot | 0.3, 0.2, 0.4 mm | [Fictiv](https://www.fictiv.com/articles/how-to-design-snap-fit-components) |
| Movable parts, first try | at least 0.3 mm, printer accurate to at least 0.2 mm | [Prusa](https://help.prusa3d.com/en/article/modeling-with-3d-printing-in-mind_164135/) |
| PCB perimeter to FDM enclosure | 1.5 to 2.0 mm, because FDM may warp | [Formlabs snap-fit enclosures](https://formlabs.com/blog/designing-3d-printed-snap-fit-enclosures/) |
| Snap lid, powder bed | 0.2 mm on each side to start, draft 1 to 2° | [JawsTec](https://www.jawstec.com/snap-fit-lid-design-tips-for-3d-printed-parts/) |
| PLA and PETG snap clearance, ABS and ASA | 0.2 to 0.3 mm, 0.3 to 0.5 mm (snippet) | [Qidi clearance guide](https://qidi3d.com/blogs/print-lab/3d-printed-snap-fit-joints-clearance-guide) |

## 2. Catalogue

Each entry covers mechanism, print orientation, FDM dimensions, cycle life in PLA versus PETG, tool-free release and failure modes.

### 2.1 Cantilever snap, straight and tapered

- Mechanism. A beam with a hook deflects over a lead-in ramp and drops into an undercut. Strain peaks at the root. [Bayer](http://fab.cba.mit.edu/classes/S62.12/people/vernelle.noel/Plastic_Snap_fit_design.pdf)
- Taper. Bayer reports a constant-section arm "uses 17% more material and exhibits 46% higher strain" than the optimal design, and an arm whose thickness falls linearly to half at the hook "increases the permissible deflection by more than 60%". The alternative is width falling to one quarter. Tapered thickness deflection is y = 1.09 · ε · l² / h (Bayer Table 1, design 2). [Bayer](http://fab.cba.mit.edu/classes/S62.12/people/vernelle.noel/Plastic_Snap_fit_design.pdf)
- Constant section. ε = 1.5 · t · Y / (L² · Q), Q is a deflection magnification factor for a non-rigid wall. Deflection force P = b · t² · E · ε / (6 L). Mating force W = P · (μ + tan α) / (1 − μ · tan α). [Fictiv](https://www.fictiv.com/articles/how-to-design-snap-fit-components)
- Orientation. Build the arm in X-Y, never along Z. A Z-built cantilever loses about 50 % elongation and 20 to 30 % tensile strength. [Formlabs](https://formlabs.com/blog/designing-3d-printed-snap-fit-enclosures/), [Fictiv](https://www.fictiv.com/articles/how-to-design-snap-fit-components). The eSUN TDS shows the same direction for PLA+, 4.14 % X-Y versus 2.92 % Z elongation. OpenAT adds that layer lines must not run in plane with the tabs. [OpenAT](https://makersmakingchange.github.io/OpenAT_Design_Guide/Design_Elements_For_3D_Printed_Parts/Snap_Fits.html)
- Dimensions. Root fillet at least 0.5 × base thickness, minimum clip width 5 mm, FDM clearance 0.5 mm, add lugs to carry shear. [Hubs](https://www.hubs.com/knowledge-base/how-design-snap-fit-joints-3d-printing/). Formlabs example uses a 1.2 mm protrusion on a 20 mm arm, says "2 mm or more would make it much more tightly secured", and uses 3 mm lugs to stop sliding. [Formlabs](https://formlabs.com/blog/designing-3d-printed-snap-fit-enclosures/). Bayer worked example l = 19 mm, b = 9.5 mm, y = 2.4 mm, α = 30°, h = 3.28 mm, P = 32.5 N, W = 58.5 N with PC on PC μ = 0.6. [Bayer](http://fab.cba.mit.edu/classes/S62.12/people/vernelle.noel/Plastic_Snap_fit_design.pdf)
- Release. A return angle equal to the lead angle gives the same force both ways. A 90° return angle "can never be disassembled, except by using an external window to release the snap". [Fictiv](https://www.fictiv.com/articles/how-to-design-snap-fit-components). A double-angled tab is removable, a barb is permanent. [OpenAT](https://makersmakingchange.github.io/OpenAT_Design_Guide/Design_Elements_For_3D_Printed_Parts/Snap_Fits.html)
- Cycle life. Fatigue "typically happens at high loading frequencies (hundreds, if not thousands, of cycles)". [Fictiv](https://www.fictiv.com/articles/how-to-design-snap-fit-components). Hubs and Hackaday both call PLA less suitable or brittle for repeated snapping and prefer ABS, PETG and nylon. [Hubs](https://www.hubs.com/knowledge-base/how-design-snap-fit-joints-3d-printing/), [Hackaday 2022](https://hackaday.com/2022/03/14/3d-printing-snap-fit-joints/). A cycle count for PLA or PETG printed snaps is not in source.
- Failure modes. Root crack at a sharp corner, layer split on a Z-built arm, creep that relaxes a preloaded hook, wear of the hook edge.

### 2.2 Cantilever with release slot or push button

- Mechanism. The hook locks with a steep return face, and a slit or window in the housing lets a finger or fingernail push the hook clear. Bayer's traffic light door "can be opened again by pressing the hook through the slit in the housing". [Bayer](http://fab.cba.mit.edu/classes/S62.12/people/vernelle.noel/Plastic_Snap_fit_design.pdf)
- Why use it. The joint resists accidental opening (steep return) but needs no tool. The hook strain on release is the same as on closing, so the 1.2 % rule in section 1.2 still applies.
- Dimensions for the window and button are not in source. Size the window from the hook width plus the clearance in section 1.4.
- Failure modes. Users pry with a screwdriver if the button is not obvious, which overstrains the arm. Mark the push point.

### 2.3 Annular snap

- Mechanism. A circular bead in a groove, the whole ring deflects. Bayer treats it with multiaxial stress, and a joint counts as "remote" from the end of the tube at 1.8 · √(d · t) or more, with remote forces about three times the near ones. [Bayer](http://fab.cba.mit.edu/classes/S62.12/people/vernelle.noel/Plastic_Snap_fit_design.pdf)
- FDM note. A round part printed upright puts the deflecting ring wall in Z layers, the weak direction in section 1.1. Bead sizes for FDM are not in source.
- Fit for this device. Poor, nothing in the base is round except a possible inlet ring.

### 2.4 Bayonet and quarter-turn twist lock

- Mechanism. Two to four tabs drop into slots, a turn locks them, and a small bump and divot hold the turned position. [OpenAT twist lock](https://makersmakingchange.github.io/OpenAT_Design_Guide/Design_Elements_For_3D_Printed_Parts/Twist-Lock.html)
- Dimensions. Tab at least 3 mm wide and 1.2 mm tall, "larger than this is better". Locking bump 0.4 to 1 mm is "a good starting range". Bumps on the side of the tab last longer than on top, because the side is usually printed parallel to the bed. Surfaces may need 45° faces to print without sag. [OpenAT twist lock](https://makersmakingchange.github.io/OpenAT_Design_Guide/Design_Elements_For_3D_Printed_Parts/Twist-Lock.html)
- Community figures (snippets, pages not fetched). Lip 2 to 3 mm and bayonet depth about 0.5 mm. [MyMiniFactory bayonet container](https://www.myminifactory.com/object/3d-print-customizable-bayonet-container-18162). One bayonet box listing says PETG holds lugs better than PLA for daily opening.
- Release. Turn back by hand. OpenAT warns users may stop at the first resistance and not seat the bump, so the parts "will easily twist apart".
- Failure modes. Bump wear, which is the fatigue point in PLA+. Bayer uses a bayonet catch for the traffic light sun visor. [Bayer](http://fab.cba.mit.edu/classes/S62.12/people/vernelle.noel/Plastic_Snap_fit_design.pdf)

### 2.5 Quarter-turn cam lock

- Mechanism. A printed knob with a cam pulls a panel tight as it turns. Same physics as the twist lock with a ramp in the slot.
- FDM numbers are not in source beyond the twist lock figures in 2.4.

### 2.6 Sliding dovetail lid and rails

- Mechanism. A lid slides along two angled rails and stops on an end wall. A small bump or a single short cantilever at the end holds it closed. Load normal to the lid is carried by the rail flanges in bearing, not by a bending arm.
- Clearance. UF Makerspace lists 0.20 to 0.25 mm for "tabs, rails, and enclosures". [UF Makerspace](https://makerspace.uflib.ufl.edu/services/3dprocess/recommended-software-for-3d-printing/designing-for-3d-printing-tolerances-on-the-mk4-and-xl/). Fictiv lists 0.4 mm for slide fits. [Fictiv](https://www.fictiv.com/articles/how-to-design-snap-fit-components). Community dovetail figures 0.12 to 0.15 mm (MakerWorld parametric) and 0.20 to 0.30 mm per side (vendor) are snippets only.
- Orientation. Print the lid flat with the rails in X-Y so the dovetail flanges are not Z-layer cantilevers. The 45° overhang rule applies to the dovetail undercut. [OpenAT](https://makersmakingchange.github.io/OpenAT_Design_Guide/Design_Elements_For_3D_Printed_Parts/Snap_Fits.html)
- Elephant foot. A rail edge that sits on the bed flares. Use the Bambu elephant foot compensation or chamfer the bottom edge. [Bambu wiki](https://wiki.bambulab.com/en/software/bambu-studio/parameter/elephant-foot)
- Cycle life. Wear on the rail is sliding, not bending, so PLA+ fatigue barely enters. The end detent is the only fatigue point.
- Failure modes. Binding from warp along a long rail, dust in the rail, lid backing out if the detent is too small.

### 2.7 Slide-in PCB grooves and card guides

- Mechanism. The board edge slides into a U groove on two or three sides. A stop or a single latch holds the open end.
- Clearance. Formlabs recommends 1.5 to 2.0 mm between PCB perimeter and an FDM enclosure because of warp, and 2.0 mm around port openings. [Formlabs](https://formlabs.com/blog/designing-3d-printed-snap-fit-enclosures/). For the groove width itself use the sliding class, 0.20 to 0.25 mm. [UF Makerspace](https://makerspace.uflib.ufl.edu/services/3dprocess/recommended-software-for-3d-printing/designing-for-3d-printing-tolerances-on-the-mk4-and-xl/). Lead-in chamfers and locating ribs help. [JawsTec](https://www.jawstec.com/snap-fit-lid-design-tips-for-3d-printed-parts/)
- Groove depth and wall for a 100 × 80 mm class board are not in source.
- Cycle life. Very high, nothing bends.
- Failure modes. Rattle if the groove is wide, which section 3.4 handles with foam or TPU.

### 2.8 Flexure latches and living hinge clips

- FDM living hinge 0.4 to 0.6 mm thick, at least two layers, orient so the width is built layer by layer. An FDM example reached only 25 cycles before failure. Recommended FDM materials are Nylon 12 or TPU in multi-material. SLS hinges 0.3 to 0.8 mm thick, at least 5 mm long, last 30 to 50 cycles. [Hubs living hinges](https://www.hubs.com/knowledge-base/how-design-living-hinges-3d-printing/)
- Protolabs gives hinge length L = π · R and splitting one 0.8 in hinge into three 0.2 in hinges with 0.1 in gaps. No guaranteed cycle count. [Protolabs hinges](https://www.protolabs.com/resources/design-tips/how-to-design-3d-printed-hinges/)
- Verdict for PLA+. Avoid for any part opened for maintenance. 4.14 % elongation at break leaves no margin for the strain a thin hinge sees.

### 2.9 Print-in-place hinge with latch

- Mechanism. Pin and knuckle printed together with a clearance gap. Print in place needs the larger end of sliding or loose clearance. [UF Makerspace](https://makerspace.uflib.ufl.edu/services/3dprocess/recommended-software-for-3d-printing/designing-for-3d-printing-tolerances-on-the-mk4-and-xl/). A1 community reports 0.15 to 0.2 mm working gaps. [Bambu forum](https://forum.bambulab.com/t/print-in-place-hinge-tolerances/111715)
- Cycle life. Knuckle wear only, no bending. Good for a flip-down door.
- Failure modes. Fused hinge from wet filament or over-extrusion, which the forum thread lists as the usual cause.

### 2.10 Magnets

- Install. Friction-fit magnets loosen from thermal creep, cylinders grip better than flat discs, side slots avoid pausing the print, a lip that the magnet is pressed past stops it falling out, and spheres self-orient. [Hackaday 2026, Slant 3D video](https://hackaday.com/2026/07/19/seven-ways-to-install-magnets-into-your-3d-prints/)
- The printed wall between magnet and target weakens the pull. Same source.
- One magnet plus a steel disc or ball on the other side avoids polarity mistakes and fragile magnet pairs. Same source.
- Magnet size, grade and pull force for a given wall are not in source. Pick by test.
- Cycle life. Unlimited for the closure itself. Creep matters only for how the magnet is held, so prefer a captured pocket over a press fit in PLA+ (HDT 53.78 °C, section 1.1).

### 2.11 Friction and press fit with crush ribs

- Crush ribs are small standoffs that deform, more forgiving than a full press fit, and a 0.1 mm change noticeably changes the fit. [Hackaday 2020 crush ribs](https://hackaday.com/2020/10/15/adding-crush-ribs-to-3d-printed-parts-for-a-better-press-fit/). Comments warn about lost bearing surface and heat.
- Crush ribs are one-time use and grip fins are reusable (Digi-Key, snippet, page blocked). An injection moulding rib of 2.5 mm diameter with 0.25 mm interference (RapidDirect, snippet).
- Bambu official 0.15 to 0.3 mm for cut connectors, with sanding for tight pegs. [Bambu wiki](https://wiki.bambulab.com/en/bambu-studio/manual/3d-print-large-files)
- Verdict. Fine for a one-time locating peg, poor for a part opened often in PLA+ because the rib does not spring back.

### 2.12 Wedge and tapered locks

- A wedge pushed in by gravity or a slide tightens as it moves. The 15° tilt of this base helps a gravity wedge at the panel's lower edge. FDM numbers are not in source. Draft of 1 to 2° on snap lids is the closest sourced figure. [JawsTec](https://www.jawstec.com/snap-fit-lid-design-tips-for-3d-printed-parts/)

### 2.13 Printed threads and threaded rings

- PLA+ (AnkerMake) at 0.2 mm layers was tested. Comment data for 1/4"-20 pull-out are 48.38 lbf at 10 % infill, 67.54 at 25 %, 89.54 at 50 %, and more than 110 at 100 % (instrument limit). [Hackaday 2023 threads](https://hackaday.com/2023/11/10/putting-3d-printed-screw-threads-to-the-test/)
- Snapmaker calls PLA brittle for threads and suggests heat-set inserts for repeated use (snippet). [Snapmaker threads](https://www.snapmaker.com/blog/3d-printing-threads)
- Verdict. A large threaded ring would work for a round inlet cover but nothing on this base is round. Thread wear in PLA+ over many cycles is not in source.

### 2.14 Keyed tabs with one locking clip (lug plus hook)

- Bayer calls "rigid lugs on one side in combination with snap-fitting hooks" an economical and reliable join, and the traffic light door hinges on a fulcrum and locks with one hook. [Bayer](http://fab.cba.mit.edu/classes/S62.12/people/vernelle.noel/Plastic_Snap_fit_design.pdf)
- Hubs and Formlabs both add lugs to carry shear so the snap arm only resists opening. [Hubs](https://www.hubs.com/knowledge-base/how-design-snap-fit-joints-3d-printing/), [Formlabs](https://formlabs.com/blog/designing-3d-printed-snap-fit-enclosures/) (3 mm lugs)
- Why it suits PLA+. Only one arm bends, the lugs never flex, and the cover is located by rigid geometry.

### 2.15 Captive pins

- A printed or steel pin through aligned knuckles, held by a clip or a snap head. FDM dimensions are not in source.

### 2.16 Gravity and nesting

- The part sits in a pocket and weight keeps it there. For the panel at 15° back tilt, a ledge under the lower edge plus side walls hold it with no latch. Tipping the base forward is the failure mode, so add one small latch or a magnet as backup. Numbers are not in source.

### 2.17 Elastic bands and O-rings

- An O-ring in a groove over two hooks, or around a lid, gives a spring that does not creep the plastic. O-ring groove sizes for FDM are not in source.

### 2.18 Community tricks

- Slant 3D videos on snap-fit clearance ([video](https://www.youtube.com/watch?v=RTQjvYENR7w), source of OpenAT's 0.4 mm), lid snaps ([video](https://www.youtube.com/watch?v=hrpYulJLnQY)) and magnets ([video](https://www.youtube.com/watch?v=BwUzOJ8B_H0)). Videos were not watched, numbers come from the written pages that cite them.
- Bambu Studio cut tool can add Plug, Dowel and Snap connectors at a cut. [Bambu wiki](https://wiki.bambulab.com/en/bambu-studio/manual/3d-print-large-files)
- Scale a lid to 100.4 % or 99.6 % to tune fit (MakerWorld bayonet box, snippet).

## 3. Electronics tricks

### 3.1 Holding the panel without screws

- Slide-in edge groove on two sides plus a ledge at the bottom edge (section 2.7), using the sliding clearance from section 1.4.
- Leave 1.5 to 2.0 mm from the board outline to enclosure walls that are not grooves, and 2.0 mm around ports such as USB and the TF slot. [Formlabs](https://formlabs.com/blog/designing-3d-printed-snap-fit-enclosures/)
- The CrowPanel sits in its own stock shell. Elecrow's v1.0 STEP models it as three printed covers, see `hardware/parts/boards/crowpanel-epaper-42.md`. Confirm the shell type and outline on the unit before setting groove widths.

### 3.2 Cable strain relief

- A U clip or a zig-zag channel moulded into the base, so the cable bends around two posts before the header. No fetched source gives dimensions. Keep the SEN6x I²C cable at 0.5 m or less. [Sensirion SEN6x mechanical guide](https://sensirion.com/media/documents/EA641247/6AFD5963/sensirion_mechanical_design_and_assembly_guidelines_SEN6x_EN.pdf)

### 3.3 Keeping the SEN63C sealed and decoupled

All from the [Sensirion SEN6x mechanical design and assembly guidelines v1.1](https://sensirion.com/media/documents/EA641247/6AFD5963/sensirion_mechanical_design_and_assembly_guidelines_SEN6x_EN.pdf).

- Openings no smaller than the sensor openings, Ain ≥ 56 mm² and Aout ≥ 148 mm². Enlarge them if mesh is used.
- Seal inlets from the outlet, and both from the rest of the device volume. Keep internal pressure equal to ambient.
- Openings should not face up or down. Isolate from external flow faster than 1 m/s.
- Sensor over-temperature should stay below 5 K. Place it below heat sources and out of sunlight. Do not thermally insulate the sensor itself.
- Use a firm fixture with flexible foam decoupling. "thick and soft foam and/or very soft rubber/silicone retainer constructions".
- Sensirion publishes an example snap-in retainer and seal as a STEP file on the SEN6x product page, for prototyping, to be adjusted "according to sealing ductility & thickness".
- Fan 4000 RPM, dominant resonance 1000 Hz, blade pass 333.34 Hz, a bracing mode near 200 Hz. Avoid loose parts that rattle, use soft rubber feet. Airflow 0.8 to 0.9 l/min.
- Gasket compression percentage is not in source.

### 3.4 Anti-rattle

- A TPU strip or a foam pad pressed between panel and base. TPU is "Ideal" on the A1. [Bambu A1 tech specs](https://bambulab.com/en/a1/tech-specs). Durometer and thickness are not in source.
- Sensirion asks for no loose parts and soft decoupling, see 3.3.

### 3.5 Vents that double as grips or latch windows

- The SEN6x chamber needs side-facing inlet and outlet openings (3.3). A row of slots on the side wall can serve as the finger grip for a sliding lid or as the release window for a cantilever (2.2). Keep the inlet and outlet sealed from each other inside, so a grip slot must not open into the inlet path. No source gives slot sizes beyond the area minimums.

## 4. Design numbers table

| Item | Value | Status | Source |
| --- | --- | --- | --- |
| eSUN PLA+ elongation at break | 4.14 % X-Y, 2.92 % Z | sourced | [eSUN TDS](https://www.esun3d.com/media/esun/catalog/certification/product/PLA/PLA_-TDS-en.pdf) |
| eSUN PLA+ flexural modulus | 3259.39 MPa X-Y, 2873.94 MPa Z | sourced | same |
| eSUN PLA+ tensile strength | 70.37 MPa X-Y, 36.36 MPa Z | sourced | same |
| eSUN PLA+ HDT | 53.78 °C at 0.45 MPa | sourced | same |
| eSUN PLA+ UV resistance | NO | sourced | same |
| Repeat-use derating | about 60 % of single-join strain | sourced | [Bayer](http://fab.cba.mit.edu/classes/S62.12/people/vernelle.noel/Plastic_Snap_fit_design.pdf) |
| PLA+ repeat-use strain ceiling | 1.24 % X-Y, 0.88 % Z | derived, section 1.2 | eSUN TDS and Bayer |
| Generic PLA snap strain | 4 to 8 % | sourced, optimistic for eSUN PLA+ | [Fictiv](https://www.fictiv.com/articles/how-to-design-snap-fit-components) |
| Tapered arm deflection | y = 1.09 · ε · l² / h | sourced | [Bayer](http://fab.cba.mit.edu/classes/S62.12/people/vernelle.noel/Plastic_Snap_fit_design.pdf) |
| Root fillet | at least 0.5 × root thickness | sourced | [Hubs](https://www.hubs.com/knowledge-base/how-design-snap-fit-joints-3d-printing/) |
| Clip width | at least 5 mm | sourced | same |
| Snap clearance FDM | 0.5 mm (Hubs), 0.4 mm (OpenAT) | sourced | [Hubs](https://www.hubs.com/knowledge-base/how-design-snap-fit-joints-3d-printing/), [OpenAT](https://makersmakingchange.github.io/OpenAT_Design_Guide/Design_Elements_For_3D_Printed_Parts/Snap_Fits.html) |
| Sliding fit, rails and grooves | 0.20 to 0.25 mm | sourced, Prusa MK4 and XL | [UF Makerspace](https://makerspace.uflib.ufl.edu/services/3dprocess/recommended-software-for-3d-printing/designing-for-3d-printing-tolerances-on-the-mk4-and-xl/) |
| Loose fit, covers | 0.30 to 0.40 mm | sourced, Prusa MK4 and XL | same |
| Bambu official connector tolerance | 0.15 to 0.3 mm | sourced, A1 wiki | [Bambu wiki](https://wiki.bambulab.com/en/bambu-studio/manual/3d-print-large-files) |
| A1 print-in-place gap that works | 0.15 to 0.2 mm | community | [Bambu forum](https://forum.bambulab.com/t/print-in-place-hinge-tolerances/111715) |
| Elephant foot example | 0.10 mm compensation | sourced, plate dependent | [Bambu wiki](https://wiki.bambulab.com/en/software/bambu-studio/parameter/elephant-foot) |
| PCB to FDM wall | 1.5 to 2.0 mm, 2.0 mm at ports | sourced | [Formlabs](https://formlabs.com/blog/designing-3d-printed-snap-fit-enclosures/) |
| Snap protrusion example | 1.2 mm on a 20 mm arm | sourced | same |
| Shear lugs | 3 mm long | sourced | same |
| Twist-lock tab | at least 3 mm wide, 1.2 mm tall | sourced | [OpenAT twist lock](https://makersmakingchange.github.io/OpenAT_Design_Guide/Design_Elements_For_3D_Printed_Parts/Twist-Lock.html) |
| Twist-lock bump | 0.4 to 1 mm | sourced | same |
| Overhang for snap faces | 45° or steeper | sourced | [OpenAT](https://makersmakingchange.github.io/OpenAT_Design_Guide/Design_Elements_For_3D_Printed_Parts/Snap_Fits.html) |
| Wall widths, 0.4 mm nozzle | 0.45, 0.9, 1.35, 1.8 mm for 1 to 4 perimeters | sourced for Prusa, A1 not in source | [Prusa](https://help.prusa3d.com/en/article/modeling-with-3d-printing-in-mind_164135/) |
| FDM living hinge | 0.4 to 0.6 mm, 25 cycles in the FDM example | sourced | [Hubs living hinges](https://www.hubs.com/knowledge-base/how-design-living-hinges-3d-printing/) |
| SEN6x inlet and outlet area | Ain ≥ 56 mm², Aout ≥ 148 mm² | sourced | [Sensirion SEN6x guide](https://sensirion.com/media/documents/EA641247/6AFD5963/sensirion_mechanical_design_and_assembly_guidelines_SEN6x_EN.pdf) |
| SEN6x over-temperature | below 5 K | sourced | same |
| SEN6x I²C cable | 0.5 m max | sourced | same |
| Gasket compression | not in source | | |
| Magnet size and pull through PLA wall | not in source | | |
| A1 default line width | not in source | | |

### 4.1 Worked snap arm for eSUN PLA+ (derived)

Tapered arm, root thickness h tapering to h/2 at the hook, width b = 6 mm (above the 5 mm Hubs minimum), E = 3259.39 MPa (TDS flexural modulus, X-Y, an upper bound on the secant modulus), friction μ = 0.6 borrowed from Bayer's PC on PC example because a PLA on PLA value is not in source.

Root strain from ε = y · h / (1.09 · l²), rearranged from Bayer.

| l (mm) | h root (mm) | undercut y (mm) | root strain | Below 1.24 % X-Y |
| --- | --- | --- | --- | --- |
| 12 | 1.8 | 1.2 | 1.38 % | no |
| 12 | 1.8 | 1.0 | 1.15 % | yes, thin margin |
| 15 | 1.8 | 1.2 | 0.88 % | yes |
| 15 | 1.8 | 1.0 | 0.73 % | yes |
| 18 | 1.8 | 1.2 | 0.61 % | yes |
| 20 | 1.8 | 1.2 | 0.50 % | yes |
| 15 | 1.35 | 1.2 | 0.66 % | yes |

For comparison a constant-section arm with t = 1.8, Y = 1.2, L = 15 and Q = 1 gives 1.5 × 1.8 × 1.2 / 225 = 1.44 % by the Fictiv formula, above the ceiling. The taper is what makes PLA+ workable.

Forces, P = b · h² · E · ε / (6 l) and W = P · (μ + tan α) / (1 − μ · tan α).

| l | h | y | P | W at 30° lead | W at 45° return |
| --- | --- | --- | --- | --- | --- |
| 15 | 1.8 | 1.0 | 5.2 N | 9.3 N | 20.7 N |
| 15 | 1.8 | 1.2 | 6.2 N | 11.2 N | 24.8 N |
| 20 | 1.8 | 1.2 | 2.6 N | 4.7 N | 10.5 N |

These are hand-assembly forces. The 45° return roughly doubles the opening force over closing, so a cover stays shut on the desk but still opens by hand.

## 5. Where eSUN PLA+ creep and fatigue bite, and what PETG or ASA would change

### 5.1 PLA+ pain points on a cover opened many times

1. Any arm that stays bent after closing creeps. Hubs and Fictiv both say a snap must return to a neutral, unstressed position after assembly. [Hubs](https://www.hubs.com/knowledge-base/how-design-snap-fit-joints-3d-printing/), [Fictiv](https://www.fictiv.com/articles/how-to-design-snap-fit-components). On this device the risk is the sensor chamber cover, because it must hold foam compression on the SEN63C seal for months. If a snap arm carries that load it relaxes and the seal leaks. Carry the foam preload on rigid lugs, rails or a hard stop, and let the snap only stop the cover from opening.
2. Low elongation. 4.14 % X-Y and 2.92 % Z at break (eSUN TDS) leave about a 1.2 % repeat-use budget. Short stubby arms and every Z-built latch are where PLA+ cracks first.
3. Heat. HDT 53.78 °C. A sunny window, a car or the ESP32 side of the base can approach that range. Magnets in friction pockets and press-fit pegs loosen first, as Hackaday notes for thermal creep. [Hackaday 2026](https://hackaday.com/2026/07/19/seven-ways-to-install-magnets-into-your-3d-prints/)
4. Wear points. The twist-lock bump, the end detent of a sliding lid and the hook edge round off with use. A cycle count for PLA+ is not in source, so plan a cycle test (section 6.4).
5. Living hinges. 25 cycles in the Hubs FDM example, too few for a maintenance door.

### 5.2 PETG, short

eSUN PETG product page lists HDT 64 °C, tensile 34.77 MPa X-Y, elongation at break 5.15 % X-Y and 3.91 % Z, flexural modulus 1228.33 MPa X-Y and 1276.45 MPa Z. [eSUN PETG](https://www.esun3d.com/petg-product/). Effects on the numbers here, all derived.

- Repeat-use strain ceiling rises to about 1.5 % X-Y and 1.2 % Z by the same method as 1.2.
- Snap force for the same geometry drops to about 38 % of PLA+ (1228 / 3259), so arms can be shorter or the undercut deeper.
- Hubs prefers PETG over PLA for snap fits. [Hubs](https://www.hubs.com/knowledge-base/how-design-snap-fit-joints-3d-printing/). PETG is "Ideal" on the A1.
- Clearance stays in the 0.2 to 0.3 mm class for PLA and PETG (Qidi, snippet).

### 5.3 ASA for an outdoor version, short

- eSUN PLA+ TDS states UV resistance "NO". eSUN ASA+ lists HDT 96.4 °C, elongation at break 5.39 % X-Y and 2.73 % Z, flexural modulus 1849.41 MPa X-Y and 1384.64 MPa Z, and claims UV and weather resistance. [eSUN ASA+](https://www.esun3d.com/asa-pro-product)
- Derived effect. X-Y strain ceiling about 1.6 %, Z about 0.8 %, so Z-built latches stay just as weak. Same-geometry force about 57 % of PLA+.
- Clearance for ABS and ASA 0.3 to 0.5 mm (Qidi, snippet).
- Printer. ASA is "Not Recommended" on the A1, with reduced interlayer strength and warping on the open frame, and it needs ventilation. [Bambu A1 tech specs](https://bambulab.com/en/a1/tech-specs), [Bambu A1 FAQ](https://wiki.bambulab.com/en/a1/manual/faq#can-a1-print-abs-and-asa). An outdoor ASA run needs an enclosed printer or a service bureau. Outdoor sensor placement also changes the SEN6x flow rule (isolate from flow faster than 1 m/s), so the outdoor version is a new chamber design, not only a new material.

## 6. Ranked recommendation

All starting dimensions assume eSUN PLA+, A1, 0.4 mm nozzle, 0.2 mm layers, every latch arm built in X-Y. Print the coupon in 6.4 before the full base.

### 6.1 (a) Panel retention in the base

1. **Slide-in cradle with gravity ledge and one release latch.** U grooves on the two side edges, a ledge under the lower edge, so the 15° back tilt and gravity seat the panel. One tapered cantilever with a push tab at the top edge stops it sliding out if the base tips.
   - Groove width = measured shell thickness plus 0.20 to 0.25 mm sliding fit ([UF Makerspace](https://makerspace.uflib.ufl.edu/services/3dprocess/recommended-software-for-3d-printing/designing-for-3d-printing-tolerances-on-the-mk4-and-xl/)). Lead-in chamfer at the groove mouth ([JawsTec](https://www.jawstec.com/snap-fit-lid-design-tips-for-3d-printed-parts/)).
   - Latch arm l = 15 to 20 mm, h root 1.8 mm tapering to 0.9 mm, b = 6 mm, undercut 1.0 mm, lead 30°, return 45°, root fillet 0.9 mm, 0.4 mm gap around the arm. Root strain 0.50 to 0.73 % (4.1).
   - 2.0 mm clear around the USB and TF slot openings ([Formlabs](https://formlabs.com/blog/designing-3d-printed-snap-fit-enclosures/)).
   - A TPU or foam strip in one groove takes up rattle (3.4).
   - Why first. Nothing bends during normal use, the panel comes out in one move, and the latch carries no load while closed, so PLA+ creep does not apply.
2. **Rigid lugs at the bottom edge plus one top hook with release slot.** Bayer's lug plus hook pattern. Lugs 3 mm ([Formlabs](https://formlabs.com/blog/designing-3d-printed-snap-fit-enclosures/)), hook as above. Slightly more positive than option 1 but the panel must tilt in, which needs extra clearance above it.
3. **Magnets with steel discs.** Two or four cylindrical magnets captured in side-slot pockets in the base, steel discs on a printed frame or on the shell. Fully tool-free and no wear. Sizes and pull are not in source, so test. Keep magnets away from the SEN63C airflow path and check that the CrowPanel has nothing magnetically sensitive near the contact points.

### 6.2 (b) Sensor chamber access cover

1. **Sliding dovetail lid with an end detent, foam preload carried by the rails.** The SEN63C sits on a foam gasket against the inlet and outlet wall. The lid slides on two dovetail rails whose flanges set the gasket compression as a fixed gap, and a 0.4 to 1 mm bump ([OpenAT twist lock](https://makersmakingchange.github.io/OpenAT_Design_Guide/Design_Elements_For_3D_Printed_Parts/Twist-Lock.html)) at the end holds it shut. Rail clearance 0.20 to 0.25 mm ([UF Makerspace](https://makerspace.uflib.ufl.edu/services/3dprocess/recommended-software-for-3d-printing/designing-for-3d-printing-tolerances-on-the-mk4-and-xl/)). Side-wall slots can be the finger grip if sealed from the inlet path.
   - Why first. Gasket load is carried in bearing by rigid rails, so PLA+ creep only lowers detent force, not seal force. Sliding wear does not fatigue.
   - Gasket thickness and compression are not in source. Start from the Sensirion example retainer STEP and adjust for the foam you buy.
2. **Hinge hooks plus one cantilever with release slot.** Two rigid hooks at one edge act as the fulcrum (Bayer traffic light door). One tapered cantilever at the opposite edge with a push window, dimensions as in 6.1. A rigid rib on the cover, not the snap, sets gasket compression against a hard stop.
3. **Quarter-turn twist-lock cap.** Tabs 3 mm wide or more and 1.2 mm tall, bump 0.4 to 1 mm on the side of the tab, 45° faces for printing ([OpenAT](https://makersmakingchange.github.io/OpenAT_Design_Guide/Design_Elements_For_3D_Printed_Parts/Twist-Lock.html)). Works if the chamber access is round. Turning a cap on a foam gasket drags the foam, so add a slip washer or keep the gasket on the sensor side.

Whatever the closure, keep the Sensirion rules. Openings face sideways, Ain ≥ 56 mm² and Aout ≥ 148 mm², inlets sealed from outlet and from the ESP32 volume, sensor below the heat source, foam decoupled. [Sensirion SEN6x guide](https://sensirion.com/media/documents/EA641247/6AFD5963/sensirion_mechanical_design_and_assembly_guidelines_SEN6x_EN.pdf)

### 6.3 (c) Base bottom or back cover

1. **Rigid lugs on one edge plus two tapered cantilevers with push-release windows.** Bayer's "economical and reliable" pattern. Lugs 3 mm, arms l = 15 to 20 mm, h 1.8 to 0.9 mm, b = 6 mm, undercut 1.0 to 1.2 mm, return 45°, root fillet 0.9 mm, 0.4 to 0.5 mm gap ([OpenAT](https://makersmakingchange.github.io/OpenAT_Design_Guide/Design_Elements_For_3D_Printed_Parts/Snap_Fits.html), [Hubs](https://www.hubs.com/knowledge-base/how-design-snap-fit-joints-3d-printing/)). Root strain 0.50 to 0.88 %, opening force about 10 to 25 N per arm (4.1). Cover clearance 0.30 to 0.40 mm on the perimeter ([UF Makerspace](https://makerspace.uflib.ufl.edu/services/3dprocess/recommended-software-for-3d-printing/designing-for-3d-printing-tolerances-on-the-mk4-and-xl/)).
2. **Sliding bottom plate on dovetail rails** with an end detent. Same numbers as 6.2 option 1. Good if the cover is long and the base has room for the slide.
3. **Magnet cover or a separate magnet TF card door.** If the TF card is swapped often, a small door held by one captured magnet and a steel disc avoids cycling any snap. Sizes not in source.

Avoid for every opened part. Living hinges (25 cycles FDM, [Hubs](https://www.hubs.com/knowledge-base/how-design-living-hinges-3d-printing/)), crush ribs (one-time, snippet), Z-built arms, and any arm that stays bent when closed.

### 6.4 First coupon to print on the A1

One plate, eSUN PLA+, 0.2 mm layers.

- Clearance ladder of a slide rail and a peg at 0.15, 0.20, 0.25, 0.30 and 0.40 mm, to replace the Prusa-derived classes with A1 values. Calibrate X-Y hole compensation for this filament first ([Bambu wiki](https://wiki.bambulab.com/en/software/bambu-studio/xy-hole-contour-compensation)).
- Three tapered arms at l = 12, 15 and 20 mm with h 1.8 to 0.9 mm and 1.2 mm undercut. Cycle each by hand and count cycles to whitening or crack. Expect the 12 mm arm to fail first, since it sits above the 1.24 % ceiling.
- One arm of each length left latched in a 50 °C oven or a sunny window for a day, then check the latch still holds, to see creep.
- A twist-lock bump at 0.4, 0.7 and 1.0 mm.

## 7. Sources

Fetched and used

- eSUN PLA+ TDS Feb 2026 v1.0, https://www.esun3d.com/media/esun/catalog/certification/product/PLA/PLA_-TDS-en.pdf
- eSUN PLA+ product page, https://www.esun3d.com/pla-pro-product/
- eSUN PETG product page, https://www.esun3d.com/petg-product/
- eSUN ASA+ product page, https://www.esun3d.com/asa-pro-product
- Bambu A1 tech specs, https://bambulab.com/en/a1/tech-specs
- Bambu A1 FAQ, https://wiki.bambulab.com/en/a1/manual/faq
- Bambu wiki, splitting large files, https://wiki.bambulab.com/en/bambu-studio/manual/3d-print-large-files
- Bambu wiki, XY hole and contour compensation, https://wiki.bambulab.com/en/software/bambu-studio/xy-hole-contour-compensation
- Bambu wiki, elephant foot, https://wiki.bambulab.com/en/software/bambu-studio/parameter/elephant-foot
- Bambu forum, print in place hinge tolerances, https://forum.bambulab.com/t/print-in-place-hinge-tolerances/111715
- Bayer Snap-Fit Joints for Plastics, http://fab.cba.mit.edu/classes/S62.12/people/vernelle.noel/Plastic_Snap_fit_design.pdf
- Fictiv, https://www.fictiv.com/articles/how-to-design-snap-fit-components
- Hubs snap fits, https://www.hubs.com/knowledge-base/how-design-snap-fit-joints-3d-printing/
- Hubs living hinges, https://www.hubs.com/knowledge-base/how-design-living-hinges-3d-printing/
- Formlabs snap-fit enclosures, https://formlabs.com/blog/designing-3d-printed-snap-fit-enclosures/
- OpenAT snap fits, https://makersmakingchange.github.io/OpenAT_Design_Guide/Design_Elements_For_3D_Printed_Parts/Snap_Fits.html
- OpenAT twist lock, https://makersmakingchange.github.io/OpenAT_Design_Guide/Design_Elements_For_3D_Printed_Parts/Twist-Lock.html
- UF Makerspace tolerances, https://makerspace.uflib.ufl.edu/services/3dprocess/recommended-software-for-3d-printing/designing-for-3d-printing-tolerances-on-the-mk4-and-xl/
- Prusa, modeling with 3D printing in mind, https://help.prusa3d.com/en/article/modeling-with-3d-printing-in-mind_164135/
- JawsTec snap lids, https://www.jawstec.com/snap-fit-lid-design-tips-for-3d-printed-parts/
- Protolabs hinges, https://www.protolabs.com/resources/design-tips/how-to-design-3d-printed-hinges/
- Hackaday crush ribs, https://hackaday.com/2020/10/15/adding-crush-ribs-to-3d-printed-parts-for-a-better-press-fit/
- Hackaday magnets, https://hackaday.com/2026/07/19/seven-ways-to-install-magnets-into-your-3d-prints/
- Hackaday threads, https://hackaday.com/2023/11/10/putting-3d-printed-screw-threads-to-the-test/
- Hackaday snap fits, https://hackaday.com/2022/03/14/3d-printing-snap-fit-joints/
- Sensirion SEN6x mechanical guide v1.1, https://sensirion.com/media/documents/EA641247/6AFD5963/sensirion_mechanical_design_and_assembly_guidelines_SEN6x_EN.pdf
- plahunter mirror and Farnell Multicomp PLA+ sheet, used only to show the superseded 20 % figure

Snippet only, page not fetched or blocked

- Qidi clearance guide, MyMiniFactory bayonet container, MakerWorld bayonet and dovetail models, Digi-Key crush rib and grip fin tips (CAPTCHA), RapidDirect crush rib, Snapmaker threads, Martins et al. 2024 PLA versus PETG fatigue (https://www.mdpi.com/2073-4360/16/13/1868), PLA creep paper PMC7764475, older 2016 eSUN PLA+ sheet.

Not accessible or not found

- Old eSUN Refilament TDS URL returned 404 (the current TDS above replaced it).
- Protolabs and Xometry snap-fit pages (cookie wall, tracker redirect or 404), Slant 3D blog, Fusion 360 blog, Printables 151102, UAL CCI wiki clearance page (404), University of Michigan A1 Mini dataset (empty response).
- No CNC Kitchen, Maker's Muse, Bambu blog or Prusa blog snap-fit article with numbers was found.
- YouTube videos were not watched.
- No source gives FDM PLA or PLA+ snap cycle counts, magnet pull through a printed wall, foam gasket compression for the SEN6x, or the A1 default line width.
