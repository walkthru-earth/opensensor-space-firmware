# SPDX-License-Identifier: CC-BY-4.0. Copyright Walkthru.Earth.
"""Assembly, Waveshare ePaper-3.97 and SEN63C desk stand.

Places the three printed parts (shell, bezel, sensor bar), the LiPo pack on
the shell battery shelf and the RTC coin in its slot as plain bodies, the vendor board
and SEN63C, the cut foam (face gaskets, two bar pads, the strip under the
sensor) and four stick-on rubber feet in the case frame, all as assembled.
The foam is drawn at its compressed thickness. The feet sit under the floor,
so the desk is at Z = -ASSUMED_FOOT_H.

Tipping. Each printed part counts as its sliced mass (params SLICE_MASS_G) at
its solid centroid, the board as ASSUMED_BOARD_MASS_G at its vendor centroid and
the sensor as SEN_MASS_MAX_G at its vendor centroid, the pack and coin as
their ASSUMED masses at their box centroids. It asserts the Thing
tilts at least MIN_TIP_DEG forward and backward before it tips, about the
foot centre lines, and reports the push at the top edge that tips it.

Check: pixi run -e cad cad run assembly.py, then
pixi run -e cad cad model interference <id> --fit 'assembly/waveshare_397' 'assembly/waveshare_397' \
    --fit 'assembly/sen63c' 'assembly/sen63c'
The two fits name the overlaps between solids inside each vendor STEP, which
are not ours.
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
from bezel import bezel  # noqa: E402
from sensor_bar import sensor_bar  # noqa: E402
from shell import shell  # noqa: E402
from shapes import box, coin_body, pack_body  # noqa: E402

PLA_DENSITY_G_CM3 = 1.24  # NatureWorks Ingeo 4043D datasheet, typical PLA filament
G = 9.81  # m/s2, standard gravity
MIN_TIP_DEG = 10.0  # call, same rule as the CrowPanel stand
PUSH_N = 0.5  # call, light touch on the top edge, reported only
GASKET_RING = 2.5  # call, foam ring width around each grille
STANDOFF_FOAM_GAP = 0.3  # call, hand cut clearance around each stand-off
ASSUMED_FOOT_MASS_G = 0.5  # ASSUMED, one 10 x 3 rubber foot

COLORS = {
    "shell": "#d9d4c7",
    "bezel": "#3a3f45",
    "sensor_bar": "#9fb3c8",
    "battery": "#6a4c93",
    "waveshare_397": "#2f6b3a",
    "sen63c": "#5d6b7a",
    "foam": "#e0b04a",
    "foot": "#222222",
}


def _solids(shape):
    return [leaf for leaf in leaves(shape) if getattr(leaf, "solids", None) and leaf.solids()]


def _group(shape, label, color):
    kids = []
    for i, s in enumerate(_solids(shape)):
        s.label = f"{label}_{i}"
        s.color = color
        kids.append(s)
    return bd.Compound(children=kids, label=label)


def _sheet_box(x0, x1, y0, y1, z0, z1):
    return vendor.sensor_to_case() * box(x0, x1, y0, y1, z0, z1)


def _gaskets():
    """Face gaskets, one ring round the two inlets and one round the outlet, 2.0 compressed."""
    g = p.GRILLE_GROW
    t = p.ASSUMED_GASKET_COMPRESSED_T
    sq = p.SEN_INLET_SQ
    holes = [bd.Pos((sq[0] + sq[1]) / 2, (sq[2] + sq[3]) / 2) * bd.Rectangle(sq[1] - sq[0] + 2 * g, sq[3] - sq[2] + 2 * g)]
    holes += [bd.Pos(x, y) * bd.Circle(d / 2 + g) for x, y, d in (p.SEN_INLET_ROUND, p.SEN_OUTLET)]
    inlet = holes[0] + holes[1]
    outlet = holes[2]
    clip = bd.Pos(p.SEN_L / 2, p.SEN_W / 2) * bd.Rectangle(p.SEN_L, p.SEN_W)
    stops = [bd.Pos((a + b) / 2, (c + d) / 2) * bd.Rectangle(b - a + 2 * STANDOFF_FOAM_GAP, d - c + 2 * STANDOFF_FOAM_GAP)
             for a, b, c, d in p.STANDOFFS]
    rings = []
    for hole in (inlet, outlet):
        ring = (bd.offset(hole, GASKET_RING) & clip) - hole
        for s in stops:
            ring = ring - s
        rings.append(bd.extrude(ring, t))
    # Sheet z -t to 0 lies between the opening face and the back wall.
    return [vendor.sensor_to_case() * bd.Pos(0, 0, -t) * r for r in rings]


def _pads():
    return [box(p.SEN_X0 + a, p.SEN_X0 + b, p.BAR_BACK_Y, p.SEN_RIM_Y, p.BAR_Z0, p.BAR_Z1) for a, b in p.PADS]


def _strip():
    return box(p.SEN_X0, p.SEN_X0 + p.SEN_L, p.SEN_RIM_Y + 1.0, p.SEN_FACE_Y - 1.0, p.FLOOR_TOP, p.SEN_Z0)


def _feet():
    feet = []
    r = p.ASSUMED_FOOT_D / 2
    for i, (x, y) in enumerate(p.FOOT_CENTRES, start=1):
        f = bd.Solid.make_cylinder(r, p.ASSUMED_FOOT_H).moved(bd.Location((x, y, -p.ASSUMED_FOOT_H)))
        f.label = f"foot_{i}"
        f.color = COLORS["foot"]
        feet.append(f)
    return feet


def _centroid(shape):
    mp = mass_properties([(sol, 1.0) for leaf in _solids(shape) for sol in leaf.solids()])
    return mp.center_of_mass, mp.volume


def tipping(parts, board, sensor, cells, top_z, report):
    masses = []
    for name, solid in parts.items():
        com, vol = _centroid(solid)
        solid_g = vol / 1000 * PLA_DENSITY_G_CM3
        g = p.SLICE_MASS_G[name]
        report.append(f"mass {name} {g:.1f} g (slice, solid {solid_g:.1f} g), "
                      f"centroid ({com.X:.1f}, {com.Y:.1f}, {com.Z:.1f})")
        masses.append((g, com))
    bc, _ = _centroid(board)
    masses.append((p.ASSUMED_BOARD_MASS_G, bc))
    report.append(f"mass board {p.ASSUMED_BOARD_MASS_G:.0f} g ASSUMED at ({bc.X:.1f}, {bc.Y:.1f}, {bc.Z:.1f})")
    sc, _ = _centroid(sensor)
    masses.append((p.SEN_MASS_MAX_G, sc))
    report.append(f"mass sen63c {p.SEN_MASS_MAX_G:.0f} g (datasheet max) at ({sc.X:.1f}, {sc.Y:.1f}, {sc.Z:.1f})")
    for name, g, cell in cells:
        cc, _ = _centroid(cell)
        masses.append((g, cc))
        report.append(f"mass {name} {g:.0f} g ASSUMED at ({cc.X:.1f}, {cc.Y:.1f}, {cc.Z:.1f})")
    for x, y in p.FOOT_CENTRES:
        masses.append((ASSUMED_FOOT_MASS_G, bd.Vector(x, y, -p.ASSUMED_FOOT_H / 2)))
    m = sum(g for g, _ in masses)
    com = sum((c * g for g, c in masses), bd.Vector(0, 0, 0)) / m
    h = com.Z + p.ASSUMED_FOOT_H
    report.append(f"total {m:.1f} g, centre of mass ({com.X:.2f}, {com.Y:.2f}, {com.Z:.2f}), {h:.2f} over the desk")
    xs = sorted({x for x, _ in p.FOOT_CENTRES})
    ys = sorted({y for _, y in p.FOOT_CENTRES})
    assert xs[0] < com.X < xs[-1] and ys[0] < com.Y < ys[-1], ("centre of mass outside the feet", com)
    d = {"forward": com.Y - ys[0], "backward": ys[-1] - com.Y, "left": com.X - xs[0], "right": xs[-1] - com.X}
    tip = {k: math.degrees(math.atan2(v, h)) for k, v in d.items()}
    report.append("tilt before tipping, about the foot centre lines, " +
                  ", ".join(f"{k} {v:.1f} deg" for k, v in tip.items()))
    assert tip["forward"] >= MIN_TIP_DEG and tip["backward"] >= MIN_TIP_DEG, tip
    w = m / 1000 * G
    arm = top_z + p.ASSUMED_FOOT_H
    push = {k: w * d[k] / arm for k in ("forward", "backward")}
    report.append(f"push at the top edge to tip it, forward {push['forward']:.2f} N, backward "
                  f"{push['backward']:.2f} N, against a {PUSH_N} N touch")
    return com, tip, push


@model
def assembly():
    report = []
    printed = {"shell": shell().shape, "bezel": bezel().shape, "sensor_bar": sensor_bar().shape}
    for name, s in printed.items():
        s.label = name
        s.color = COLORS[name]
    board = _group(vendor.board_body(), "waveshare_397", COLORS["waveshare_397"])
    sensor = _group(vendor.sensor_body(), "sen63c", COLORS["sen63c"])
    foam = []
    for i, f in enumerate([*_gaskets(), *_pads(), _strip()]):
        f.label = f"foam_{i}"
        f.color = COLORS["foam"]
        foam.append(f)
    foam = bd.Compound(children=foam, label="foam")
    pack = pack_body()
    coin = coin_body()
    for s, label in ((pack, "lipo_pack"), (coin, "rtc_coin")):
        s.label = label
        s.color = COLORS["battery"]
    feet = _feet()
    top_z = printed["shell"].bounding_box().max.Z
    try:
        tipping(printed, board, sensor, (("lipo pack", p.ASSUMED_BAT_MASS_G, pack), ("rtc coin", p.ASSUMED_RTC_MASS_G, coin)), top_z, report)
    finally:
        for line in report:
            print("REPORT", line)
    everything = [*printed.values(), board, sensor, foam, pack, coin, *feet]
    bb = bd.Compound(children=everything).bounding_box()
    print("REPORT", f"assembled bbox X {bb.min.X:.2f} to {bb.max.X:.2f}, Y {bb.min.Y:.2f} to {bb.max.Y:.2f}, "
                    f"Z {bb.min.Z:.2f} to {bb.max.Z:.2f}")
    assert abs(bb.min.Z + p.ASSUMED_FOOT_H) < 1e-6, bb.min.Z
    return bd.Compound(children=everything, label="assembly")


if __name__ == "__main__":
    assembly()
