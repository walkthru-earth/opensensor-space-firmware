# SPDX-License-Identifier: CC-BY-4.0. Copyright Walkthru.Earth.
"""Assembly, CrowPanel 4.2 and SEN63C desk base, iteration 1.

PLAN-iteration-1.md Task 5. Places the three printed parts, the vendor
CrowPanel, SEN63C and SEN6x foam, and four bought rubber feet in the base frame
(X along the base, Y front to back, Z up, desk at Z 0), all as assembled. The
panel rests at TILT_DEG against the rear wall, the backrest sits in its socket
and the bottom plate is clicked home. The face gasket is the Sensirion
example foam with the strip over the two rigid gasket stops cut out (_gasket).

Tipping (PLAN Review Focus 5). The centre of mass comes from each printed
part's print mass at its solid centroid, the panel at ASSUMED_PANEL_MASS_G at
the centroid of the vendor body, the sensor at SEN_MASS_MAX_G at its vendor
centroid and the feet at an assumed mass. It asserts that the whole Thing
tilts at least 10 degrees forward and backward before it tips, and that a
0.5 N horizontal push at the panel top edge, either way, does not tip it.
The feet act at their centres, which is on the safe side of their 10 mm faces.

Check: pixi run -e cad cad run assembly.py, then
pixi run -e cad cad model interference <id> --fit 'assembly/sen6x_foam' 'assembly/sen63c' \
    --fit 'assembly/crowpanel' 'assembly/crowpanel'
The second fit names the 35 overlaps between solids inside the CrowPanel vendor
STEP itself (shell over PCB and parts, largest 7,354 mm3), which are not ours.
Measured 2026-11-20, exit 0, no printed part shares volume with a vendor body.
"""

import math
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))

import build123d as bd  # noqa: E402
from cad import model  # noqa: E402
from cad.build123d import leaves, mass_properties  # noqa: E402

import params as p  # noqa: E402
import vendor  # noqa: E402
from backrest import backrest  # noqa: E402
from base_body import STOP_GAP, STOP_X0, STOP_X1, base_body  # noqa: E402
from bottom_plate import bottom_plate  # noqa: E402

G = 9.81  # m/s2, standard gravity
PUSH_N = 0.5  # PLAN Review Focus 5, push at the panel top edge
MIN_TIP_DEG = 10.0  # PLAN Review Focus 5, tilt forward and backward before tipping

# Print mass per part in grams, model only with supports removed. Source is the
# Bambu Studio slice of the iter1 3MF files (README Print settings), which
# carries the real walls and shells. The base body is sliced at 40 % infill
# because at 15 % (55.00 g) the Thing tips at 0.464 N about the foot centres,
# under the 0.5 N push. The backrest and plate stay at 15 %. A part with None
# falls back to its solid mass times ASSUMED_FILL_FALLBACK.
PRINT_MASS_G = {
    "base_body": 80.14,  # Bambu Studio 02.09.01.70 slice 2026-11-20, 40 % infill, 93.02 total less 12.88 tree support
    "backrest": 8.05,  # same slice set, no support
    "bottom_plate": 17.07,  # same slice set, no support
}
ASSUMED_FILL_FALLBACK = 0.5  # ASSUMED, used only when a slice number is missing, low side for tipping
ASSUMED_FOOT_MASS_G = 0.5  # ASSUMED, one 10 x 3 rubber foot, too small to matter

COLORS = {
    "base_body": "#d9d4c7",
    "backrest": "#b8c4b0",
    "bottom_plate": "#9fb3c8",
    "crowpanel": "#3a3f45",
    "sen63c": "#5d6b7a",
    "sen6x_foam": "#e0b04a",
    "foot": "#222222",
}


def _solids(shape):
    return [leaf for leaf in leaves(shape) if getattr(leaf, "solids", None) and leaf.solids()]


def _group(shape, label, color):
    """A vendor compound as one labelled child, its solids kept as they are."""
    kids = []
    for i, s in enumerate(_solids(shape)):
        s.label = f"{label}_{i}"
        s.color = color
        kids.append(s)
    return bd.Compound(children=kids, label=label)


def _feet():
    feet = []
    r = p.ASSUMED_FOOT_D / 2
    for i, (x, y) in enumerate(p.FOOT_CENTRES, start=1):
        f = bd.Solid.make_cylinder(r, p.ASSUMED_FOOT_H).moved(bd.Location((x, y, 0.0)))
        f.label = f"foot_{i}"
        f.color = COLORS["foot"]
        feet.append(f)
    return feet


def _gasket():
    """The face gasket as fitted, the Sensirion example foam with the stop strip cut out.

    The example foam covers the whole opening face, so laid on as it is it sits
    on the two rigid gasket stops (63 mm3 measured). base_body.py places the
    stops in the strip between the inlet and outlet gasket lands with STOP_GAP
    clear each side, so the hand cut gasket stops short of them. That strip,
    STOP_X0 - STOP_GAP to STOP_X1 + STOP_GAP across the full face, is cut here.
    The foam stays at its vendor 0.9 thickness, uncompressed, as a reference.
    """
    x0, x1 = STOP_X0 - STOP_GAP, STOP_X1 + STOP_GAP
    strip = bd.Solid.make_box(x1 - x0, 10.0, 60.0).moved(bd.Location((x0, p.SEN_FACE_Y - 5.0, 0.0)))
    kids = []
    for leaf in _solids(vendor.foam_body()):
        for sol in leaf.solids():
            kids.extend(sol.cut(strip).solids())
    return bd.Compound(children=kids)


def _centroid(shape):
    mp = mass_properties([(sol, 1.0) for leaf in _solids(shape) for sol in leaf.solids()])
    return mp.center_of_mass, mp.volume


def tipping(parts, panel, sensor, report):
    """Centre of mass and the Review Focus 5 checks. parts maps a label to its solid."""
    masses = []
    for name, solid in parts.items():
        com, vol = _centroid(solid)
        solid_g = vol / 1000 * p.PLA_DENSITY_G_CM3
        g = PRINT_MASS_G[name]
        src = "slice"
        if g is None:
            g = solid_g * ASSUMED_FILL_FALLBACK
            src = f"solid x {ASSUMED_FILL_FALLBACK} ASSUMED"
        report.append(f"mass {name} {g:.1f} g ({src}), solid {solid_g:.1f} g, fill {g / solid_g:.2f}, "
                      f"centroid ({com.X:.1f}, {com.Y:.1f}, {com.Z:.1f})")
        masses.append((g, com))
    pc, _ = _centroid(panel)
    panel_i = len(masses)
    masses.append((p.ASSUMED_PANEL_MASS_G, pc))
    report.append(f"mass crowpanel {p.ASSUMED_PANEL_MASS_G:.0f} g ASSUMED at its vendor centroid "
                  f"({pc.X:.1f}, {pc.Y:.1f}, {pc.Z:.1f})")
    sc, _ = _centroid(sensor)
    masses.append((p.SEN_MASS_MAX_G, sc))
    report.append(f"mass sen63c {p.SEN_MASS_MAX_G:.0f} g (datasheet max) at its vendor centroid ({sc.X:.1f}, {sc.Y:.1f}, {sc.Z:.1f})")
    for x, y in p.FOOT_CENTRES:
        masses.append((ASSUMED_FOOT_MASS_G, bd.Vector(x, y, p.ASSUMED_FOOT_H / 2)))
    m = sum(g for g, _ in masses)
    com = sum((c * g for g, c in masses), bd.Vector(0, 0, 0)) / m
    report.append(f"total {m:.1f} g, centre of mass ({com.X:.2f}, {com.Y:.2f}, {com.Z:.2f})")

    xs = sorted({x for x, _ in p.FOOT_CENTRES})
    ys = sorted({y for _, y in p.FOOT_CENTRES})
    assert xs[0] < com.X < xs[-1] and ys[0] < com.Y < ys[-1], ("centre of mass outside the feet", com, xs, ys)
    d_front, d_back = com.Y - ys[0], ys[-1] - com.Y
    d_left, d_right = com.X - xs[0], xs[-1] - com.X
    tip = {k: math.degrees(math.atan2(d, com.Z)) for k, d in
           (("forward", d_front), ("backward", d_back), ("left", d_left), ("right", d_right))}
    report.append("tilt before tipping, about the foot centre lines, " +
                  ", ".join(f"{k} {v:.1f} deg" for k, v in tip.items()))
    assert tip["forward"] >= MIN_TIP_DEG and tip["backward"] >= MIN_TIP_DEG, tip

    top_z = panel.bounding_box().max.Z
    w = m / 1000 * G
    push = {"backward": w * d_back / top_z, "forward": w * d_front / top_z}
    report.append(f"panel top edge Z {top_z:.2f}, weight {w:.3f} N, push to tip at the top edge about the foot "
                  f"centres backward {push['backward']:.3f} N, forward {push['forward']:.3f} N, "
                  f"margin {min(push.values()) / PUSH_N:.2f} x the {PUSH_N} N push")
    r = p.ASSUMED_FOOT_D / 2
    edge = {"backward": w * (d_back + r) / top_z, "forward": w * (d_front + r) / top_z}
    report.append(f"if the feet are flat and tip about their outer edges, backward {edge['backward']:.3f} N, "
                  f"forward {edge['forward']:.3f} N, margin {min(edge.values()) / PUSH_N:.2f} x (not asserted)")
    # A horizontal push is resisted by each mass times its horizontal arm to the tip line,
    # so the lightest panel that still holds the push about the foot centres is:
    others = [(g, c) for i, (g, c) in enumerate(masses) if i != panel_i]

    def panel_floor(line_y, sign):
        rest = sum(g * sign * (line_y - c.Y) for g, c in others)
        return (PUSH_N * top_z / G * 1000 - rest) / (sign * (line_y - pc.Y))
    floor_g = max(panel_floor(ys[-1], 1.0), panel_floor(ys[0], -1.0))
    report.append(f"the push check about the foot centres holds while the panel weighs at least {floor_g:.0f} g "
                  f"(ASSUMED {p.ASSUMED_PANEL_MASS_G:.0f} g)")
    assert min(push.values()) >= PUSH_N, push
    return com, tip, push


@model
def assembly():
    report = []
    base = base_body().shape
    rest = backrest().shape
    plate = bottom_plate().shape
    for s, name in ((base, "base_body"), (rest, "backrest"), (plate, "bottom_plate")):
        s.label = name
        s.color = COLORS[name]
    panel = _group(vendor.panel_body(), "crowpanel", COLORS["crowpanel"])
    sensor = _group(vendor.sensor_body(), "sen63c", COLORS["sen63c"])
    foam = _group(_gasket(), "sen6x_foam", COLORS["sen6x_foam"])
    feet = _feet()

    try:
        tipping({"base_body": base, "backrest": rest, "bottom_plate": plate}, panel, sensor, report)
    finally:
        for line in report:
            print("REPORT", line)
        report.clear()

    bb = bd.Compound(children=[base, rest, plate, panel, *feet]).bounding_box()
    report.append(f"assembled bbox X {bb.min.X:.2f} to {bb.max.X:.2f}, Y {bb.min.Y:.2f} to {bb.max.Y:.2f}, "
                  f"Z {bb.min.Z:.2f} to {bb.max.Z:.2f}")
    assert abs(bb.min.Z) < 1e-6, ("feet must stand on the desk at Z 0", bb.min.Z)
    for line in report:
        print("REPORT", line)
    return bd.Compound(children=[base, rest, plate, panel, sensor, foam, *feet], label="assembly")


if __name__ == "__main__":
    assembly()
