# SPDX-License-Identifier: CC-BY-4.0. Copyright Walkthru.Earth.
"""Bottom plate, CrowPanel 4.2 and SEN63C desk base, iteration 1.

DESIGN.md "Bottom plate", PLAN-iteration-1.md Task 4 and Review Focus 3. The
plate closes the open bottom of the base. It slides in from the right (+X) on
the two dovetail rails until its left end meets the left wall, and a ridge on
its top snaps past the right wall to hold it there. It carries the four
adhesive feet, the ambient vent slots under the harness bay, and the foam pad
seat under the sensor. The thumb grip sits on the underside at the +X end,
used with the base turned over.

Frames. bottom_plate() is in the base frame at its assembled position (X along
the base, Y front to back, Z up, desk at Z 0). bottom_plate_print() lies with
its underside on the bed, the way params says the dovetail key prints.

Check: pixi run -e cad cad run bottom_plate.py, then
pixi run -e cad cad model dfm bottom_plate.py:bottom_plate_print --process printing --technology fdm --material pla
"""

import math
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))

import build123d as bd  # noqa: E402
from cad import model  # noqa: E402

import params as p  # noqa: E402

FEATURES = True  # False builds the bare dovetail blank, the first run that saw the probes fail

# ---------------------------------------------------------------------------
# Part values, base frame. Each comes from params.py or is a named builder call.
# ---------------------------------------------------------------------------
X0 = p.DOVETAIL_PLATE_STOP_X  # -0.35, left end, the end stop against the left wall inner face
X1 = p.DOVETAIL_PLATE_EXIT_X  # 109.15, right end, flush with the right wall outer face
Z0 = p.BASE_Z0  # 2.0, underside
Z1 = p.PLATE_TOP_Z  # 5.0, top
Y0N = p.DOVETAIL_PLATE_Y0  # 3.3, underside edge at the front, key narrow face (test_plate dove_key)
Y1N = p.DOVETAIL_PLATE_Y1  # 66.8, underside edge at the back
Y0W = Y0N - p.DOVETAIL_PLATE_RUN  # 1.568, top edge at the front, key wide face
Y1W = Y1N + p.DOVETAIL_PLATE_RUN  # 68.532
YC = (Y0N + Y1N) / 2  # 35.05
BED_CHAMFER = 0.4  # PLAN Task 3 Step 3 bed edge chamfer, takes the elephant foot (MAT 1.3, 0.10)

# Feet. The params FOOT_CENTRES put the left pair 4.2 from the plate end, so a
# 10 mm foot breaks out of it (reported). The inset here is from the plate edges.
FOOT_FIT = p.FIT_SLIDE  # builder call, 0.2 per side round the 10 mm foot
FOOT_R = p.ASSUMED_FOOT_D / 2 + FOOT_FIT  # 5.2
FOOT_INSET = math.ceil((FOOT_R + p.PLA_MIN_WALL + BED_CHAMFER) * 2) / 2  # 6.5, builder call, rounded up to 0.5
FOOT_CENTRES = tuple(
    (x, y) for x in (X0 + FOOT_INSET, X1 - FOOT_INSET) for y in (Y0N + FOOT_INSET, Y1N - FOOT_INSET)
)

# Ambient vent slots, the grille slot and bar of params reused
VENT_SLOT_W = p.GRILLE_SLOT_W  # 2.5
VENT_BAR_W = p.GRILLE_BAR_W  # 1.35
VENT_PITCH = VENT_SLOT_W + VENT_BAR_W
VENT_N = int((p.VENT_X1 - p.VENT_X0 + VENT_BAR_W + 1e-9) // VENT_PITCH)  # 7, fills the 25.6 exactly
VENT_SPAN = VENT_N * VENT_SLOT_W + (VENT_N - 1) * VENT_BAR_W
VENT_SLOT_X0 = (p.VENT_X0 + p.VENT_X1) / 2 - VENT_SPAN / 2

# Pad seat under the sensor. Pad cut to the SEN63C footprint 55.2 x 21.3 (builder call).
SEAT_FIT = p.FIT_COVER  # 0.3 per side, loose fit for a hand cut pad
SEAT_X0 = X0  # builder call, runs out to the plate end, the left wall closes it. SEN_X0 - SEAT_FIT
# (-0.25) would leave a 0.1 lip that cad dfm calls an unprintable wall.
SEAT_X1 = p.SEN_X0 + p.SEN_L + SEAT_FIT
SEAT_Y0 = p.SEN_RIM_Y - SEAT_FIT
SEAT_Y1 = p.SEN_FACE_Y + SEAT_FIT
SEAT_Z = Z1 - p.PAD_SEAT_D  # 4.5

# End detent. A ridge on the top snaps past the right wall inner face. The
# socket of test_plate adds the fit across (in Y), so on 60 degree flanks the
# plate drops DOVETAIL_PLATE_FIT * tan(60) before it bends. The ridge stands
# that much above the detent so the plate still bends by the full 0.4.
RIGHT_WALL_IN_X = p.CRADLE_SIDE_IN_X1  # 107.35, right wall inner face (BASE_X1 - WALL)
DOVE_DROP = p.DOVETAIL_PLATE_FIT * math.tan(math.radians(p.DOVETAIL_PLATE_FLANK_DEG))  # 0.346
RIDGE_H = p.DOVETAIL_PLATE_DETENT + DOVE_DROP  # 0.746
RIDGE_DEG = p.PLA_OVERHANG_DEG  # MAT 4 snap faces 45 degrees, both ways so it lets go on a pull
RIDGE_RUN = RIDGE_H / math.tan(math.radians(RIDGE_DEG))
RIDGE_FLAT = 0.4  # builder call, flat crest
RIDGE_GAP = p.FIT_SLIDE  # builder call, seated gap to the right wall inner face
RIDGE_X1 = RIGHT_WALL_IN_X - RIDGE_GAP  # 107.15, outer toe
RIDGE_X0 = RIDGE_X1 - 2 * RIDGE_RUN - RIDGE_FLAT
RIDGE_L = 20.0  # builder call, along Y, centred where the plate bends most
RIDGE_Y0 = YC - RIDGE_L / 2
RIDGE_Y1 = YC + RIDGE_L / 2

# Thumb grip, three V grooves across the underside at the +X end
GRIP_D = p.PLATE_T / 2  # builder call, 1.5 deep, 1.5 left above
GRIP_DEG = p.DOVETAIL_PLATE_FLANK_DEG  # 60, same overhang as the rail flanks
GRIP_HALF = GRIP_D / math.tan(math.radians(GRIP_DEG))  # 0.866
GRIP_PITCH = 3.0  # builder call
GRIP_N = 3  # builder call
GRIP_XS = tuple(RIDGE_X0 - GRIP_HALF - i * GRIP_PITCH for i in range(GRIP_N))  # last groove edge under the ridge toe
GRIP_L = 30.0  # builder call, along Y
GRIP_Y0 = YC - GRIP_L / 2
GRIP_Y1 = YC + GRIP_L / 2


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------
def _box(x0, x1, y0, y1, z0, z1):
    return bd.Pos((x0 + x1) / 2, (y0 + y1) / 2, (z0 + z1) / 2) * bd.Box(x1 - x0, y1 - y0, z1 - z0)


def _prism_yz(pts, x0, x1):
    """Prism along +X from x0 to x1 with the (y, z) polygon as its section."""
    face = bd.Plane.YZ.offset(x0) * bd.Polygon(*pts, align=None)
    return bd.extrude(face, x1 - x0, dir=(1, 0, 0))


def _prism_xz(pts, y0, y1):
    """Prism along +Y from y0 to y1 with the (x, z) polygon as its section."""
    face = bd.Plane.XZ.offset(-y0) * bd.Polygon(*pts, align=None)
    return bd.extrude(face, y1 - y0, dir=(0, 1, 0))


def _probe(shape, pt):
    return shape.is_inside(bd.Vector(*pt))


def _to_print():
    """Base frame to print frame, underside on the bed, left end at X 0."""
    return bd.Location((-X0, 0.0, -Z0))


# ---------------------------------------------------------------------------
# Checks
# ---------------------------------------------------------------------------
def _thickness_at(x, y):
    for cx, cy in FOOT_CENTRES:
        dx = x - cx
        if abs(dx) < FOOT_R and abs(y - cy) <= math.sqrt(FOOT_R**2 - dx**2):
            return p.PLATE_T - p.FOOT_RECESS
    return p.PLATE_T


def detent_flex(report):
    """Strain where the plate bends under the ridge, as a beam across Y between the rails.

    Euler-Bernoulli, central point load, span from rail edge to rail edge at
    the underside (the shorter span, so the higher strain). Simply supported
    and clamped ends bound the dovetail grip. The section is thinner over the
    foot recesses. Width cancels from the strain.
    """
    span = Y1N - Y0N
    xs = (RIDGE_X0 + RIDGE_X1) / 2
    n = 4000
    dy = span / n
    e = p.PLA_E_XY_MPA
    ys = [(i + 0.5) * dy for i in range(n)]
    ts = [_thickness_at(xs, Y0N + y) for y in ys]
    ei = [e * t**3 / 12 for t in ts]  # per mm of width
    m0 = [min(y, span - y) / 2 for y in ys]  # moment from a unit centre load, simply supported
    d = p.DOVETAIL_PLATE_DETENT
    # Simply supported
    flex = sum(m * m / k for m, k in zip(m0, ei)) * dy
    f = d / flex
    eps_ss = max(f * m * (t / 2) / k for m, t, k in zip(m0, ts, ei)) * 100
    # Clamped both ends, symmetric, slope zero at the ends and the centre
    half = n // 2
    a = sum(1 / ei[i] for i in range(half))
    b = sum(ys[i] / 2 / ei[i] for i in range(half))
    m_end = -b / a  # per unit centre load
    mu = [m_end + y / 2 if y <= span / 2 else m_end + (span - y) / 2 for y in ys]
    flex_c = sum(m * m0i / k for m, m0i, k in zip(mu, m0, ei)) * dy
    fc = d / flex_c
    eps_cc = max(abs(fc * m) * (t / 2) / k for m, t, k in zip(mu, ts, ei)) * 100
    report.append(
        f"detent flex {d} at plate x {xs:.2f}, span {span:.2f}, strain {eps_ss:.3f} % simply supported, "
        f"{eps_cc:.3f} % clamped, limit {p.PLA_STRAIN_LIMIT_XY} %"
    )
    # The beam spans Y, so its load is per mm of plate width along X. The
    # ridge runs RIDGE_L along that span, not across it, so no total force is
    # given here. The owner slide test measures it.
    report.append(
        f"detent load per mm of plate width along X {f:.3f} N simply supported, {fc:.3f} N clamped, "
        f"friction not modelled"
    )
    assert max(eps_ss, eps_cc) <= p.PLA_STRAIN_LIMIT_XY, (eps_ss, eps_cc)


def check_values(report):
    """Number checks from params, before any geometry."""
    assert abs(Z0 - (p.ASSUMED_FOOT_H - p.FOOT_RECESS)) < 1e-9, Z0
    report.append(f"BASE_Z0 {Z0} = foot {p.ASSUMED_FOOT_H} - recess {p.FOOT_RECESS}")
    pad_gap = p.SEN_Z0 - SEAT_Z
    assert abs(pad_gap - (p.PAD_UNDER_T - p.PAD_UNDER_COMPRESSION)) < 1e-9, pad_gap
    report.append(f"pad gap {pad_gap} = pad {p.PAD_UNDER_T} - compression {p.PAD_UNDER_COMPRESSION}")
    assert p.VENT_GRILLE_DIST >= p.VENT_MIN_GRILLE_DIST, p.VENT_GRILLE_DIST
    report.append(f"vent to nearest grille slot {p.VENT_GRILLE_DIST:.2f}, at least {p.VENT_MIN_GRILLE_DIST}")
    assert VENT_N >= 1 and VENT_SPAN <= p.VENT_X1 - p.VENT_X0 + 1e-9 and VENT_BAR_W >= p.PLA_MIN_WALL
    assert p.VENT_Y0 - Y0N >= p.PLA_MIN_WALL and p.VENT_Y1 <= Y1N
    report.append(f"vent {VENT_N} slots {VENT_SLOT_W} x {p.VENT_Y1 - p.VENT_Y0:.1f}, bars {VENT_BAR_W}, "
                  f"open area {VENT_N * VENT_SLOT_W * (p.VENT_Y1 - p.VENT_Y0):.1f} mm2")
    # End stop, rails and plate size
    assert abs(X0 - p.POCKET_X0) < 1e-9 and abs(X1 - p.BASE_X1) < 1e-9
    assert abs(math.degrees(math.atan2(p.PLATE_T, p.DOVETAIL_PLATE_RUN)) - p.DOVETAIL_PLATE_FLANK_DEG) < 1e-9
    assert p.DOVETAIL_PLATE_FLANK_DEG >= p.PLA_OVERHANG_DEG
    # Feet inside the plate with a wall left past the bed chamfer
    for cx, cy in FOOT_CENTRES:
        wall = min(cx - X0, X1 - cx, cy - Y0N, Y1N - cy) - FOOT_R - BED_CHAMFER
        assert wall >= p.PLA_MIN_WALL, (cx, cy, wall)
    report.append(f"feet at {FOOT_CENTRES}, recess d {2 * FOOT_R} x {p.FOOT_RECESS} deep, inset {FOOT_INSET}")
    params_walls = [min(cx - X0, X1 - cx, cy - Y0N, Y1N - cy) - FOOT_R for cx, cy in p.FOOT_CENTRES]
    report.append(f"params FOOT_CENTRES would leave {min(params_walls):.2f} at the plate edge (needs "
                  f"{p.PLA_MIN_WALL} past the {BED_CHAMFER} chamfer), part-local centres used")
    # Seat inside the open pocket floor, so the walls bear on the plate top round it
    assert p.POCKET_X0 <= SEAT_X0 and SEAT_X1 <= p.POCKET_X1, (SEAT_X0, SEAT_X1)
    assert SEAT_X0 <= p.SEN_X0 - SEAT_FIT and p.SEN_X0 - SEAT_FIT - X0 < p.PLA_MIN_WALL, "lip would not print"
    assert p.POCKET_Y0 <= SEAT_Y0 and SEAT_Y1 <= p.POCKET_Y1, (SEAT_Y0, SEAT_Y1)
    report.append(f"pad seat X {SEAT_X0:.2f} to {SEAT_X1:.2f}, Y {SEAT_Y0:.2f} to {SEAT_Y1:.2f}, "
                  f"floor Z {SEAT_Z}, opens up so no bridge")
    # Detent
    assert RIDGE_X1 < RIGHT_WALL_IN_X and RIDGE_X0 > X1 - p.PLATE_T * 10, RIDGE_X0
    assert RIDGE_DEG >= p.PLA_OVERHANG_DEG
    report.append(f"ridge X {RIDGE_X0:.3f} to {RIDGE_X1:.3f}, Y {RIDGE_Y0:.2f} to {RIDGE_Y1:.2f}, "
                  f"height {RIDGE_H:.3f} = detent {p.DOVETAIL_PLATE_DETENT} + rail drop {DOVE_DROP:.3f}, "
                  f"{RIDGE_DEG} degree faces, seated gap {RIDGE_GAP} to the wall at X {RIGHT_WALL_IN_X}")
    detent_flex(report)
    # Grip, V grooves with no flat ceiling
    assert GRIP_DEG >= p.PLA_OVERHANG_DEG and p.PLATE_T - GRIP_D >= p.SKIN
    bars = GRIP_PITCH - 2 * GRIP_HALF
    assert bars >= p.PLA_MIN_WALL, bars
    assert GRIP_XS[0] + GRIP_HALF <= RIDGE_X0 + 1e-9
    for cx, cy in FOOT_CENTRES:
        ylo, yhi = cy - FOOT_R, cy + FOOT_R
        assert yhi + p.PLA_MIN_WALL <= GRIP_Y0 or ylo - p.PLA_MIN_WALL >= GRIP_Y1, (cx, cy)
    report.append(f"grip {GRIP_N} grooves {GRIP_D} deep, {2 * GRIP_HALF:.3f} wide, {GRIP_DEG} degree faces, "
                  f"centres X {', '.join(f'{x:.2f}' for x in GRIP_XS)}, Y {GRIP_Y0:.2f} to {GRIP_Y1:.2f}")
    # Bridges in the print orientation (underside on the bed)
    report.append(f"bridges: foot recess ceilings span {2 * FOOT_R} at {p.FOOT_RECESS} above the bed, "
                  f"over PLA_MAX_BRIDGE {p.PLA_MAX_BRIDGE} (reported, not hidden), vents run through, "
                  f"grip grooves are {GRIP_DEG} degree V, seat opens up")


def check_geometry(part, report):
    """Probes on the assembled part, base frame."""
    def inside(x, y, z):
        return _probe(part, (x, y, z))

    tan_f = math.tan(math.radians(p.DOVETAIL_PLATE_FLANK_DEG))
    xm = 60.0
    for z in (Z0 + 1.0, Z1 - 0.05):
        yf = Y0N - (z - Z0) / tan_f
        yb = Y1N + (z - Z0) / tan_f
        assert inside(xm, yf + 0.03, z) and not inside(xm, yf - 0.03, z), ("front flank", z)
        assert inside(xm, yb - 0.03, z) and not inside(xm, yb + 0.03, z), ("back flank", z)
    assert inside(X0 + 0.03, 30.0, 3.5) and not inside(X0 - 0.03, 30.0, 3.5), "left end stop"
    assert inside(X1 - 0.03, 30.0, 3.5) and not inside(X1 + 0.03, 30.0, 3.5), "right end"
    assert inside(30.0, 30.0, Z1 - 0.03) and not inside(30.0, 30.0, Z1 + 0.03), "top"
    assert inside(30.0, 30.0, Z0 + 0.03) and not inside(30.0, 30.0, Z0 - 0.03), "underside"
    # Feet
    for cx, cy in FOOT_CENTRES:
        zr = Z0 + p.FOOT_RECESS
        assert not inside(cx, cy, zr - 0.03) and inside(cx, cy, zr + 0.03), ("foot depth", cx, cy)
        assert not inside(cx + FOOT_R - 0.03, cy, Z0 + 0.5) and inside(cx + FOOT_R + 0.03, cy, Z0 + 0.5), ("foot r", cx)
    # Vents
    for i in range(VENT_N):
        sx = VENT_SLOT_X0 + i * VENT_PITCH
        yc = (p.VENT_Y0 + p.VENT_Y1) / 2
        assert not inside(sx + VENT_SLOT_W / 2, yc, 3.5), ("vent", i)
        assert inside(sx - 0.03, yc, 3.5) and inside(sx + VENT_SLOT_W + 0.03, yc, 3.5), ("vent bars", i)
        assert inside(sx + 1.0, p.VENT_Y0 - 0.03, 3.5) and inside(sx + 1.0, p.VENT_Y1 + 0.03, 3.5), ("vent ends", i)
    # Pad seat
    sx, sy = (SEAT_X0 + SEAT_X1) / 2, (SEAT_Y0 + SEAT_Y1) / 2
    assert not inside(sx, sy, SEAT_Z + 0.03) and inside(sx, sy, SEAT_Z - 0.03), "seat floor"
    assert not inside(X0 + 0.03, sy, Z1 - 0.2) and inside(X0 + 0.03, sy, SEAT_Z - 0.03), "seat open at the left end"
    assert inside(SEAT_X1 + 0.03, sy, Z1 - 0.2) and not inside(SEAT_X1 - 0.03, sy, Z1 - 0.2), "seat right"
    assert inside(sx, SEAT_Y0 - 0.03, Z1 - 0.2) and not inside(sx, SEAT_Y0 + 0.03, Z1 - 0.2), "seat front"
    assert inside(sx, SEAT_Y1 + 0.03, Z1 - 0.2) and not inside(sx, SEAT_Y1 - 0.03, Z1 - 0.2), "seat back"
    # Ridge
    xc = (RIDGE_X0 + RIDGE_X1) / 2
    assert inside(xc, YC, Z1 + RIDGE_H - 0.03) and not inside(xc, YC, Z1 + RIDGE_H + 0.03), "ridge crest"
    assert inside(RIDGE_X1 - 0.06, YC, Z1 + 0.03) and not inside(RIDGE_X1 + 0.03, YC, Z1 + 0.03), "ridge toe"
    assert inside(RIDGE_X0 + 0.06, YC, Z1 + 0.03) and not inside(RIDGE_X0 - 0.03, YC, Z1 + 0.03), "ridge heel"
    assert not inside(xc, RIDGE_Y1 + 0.03, Z1 + 0.1) and inside(xc, RIDGE_Y1 - 0.03, Z1 + 0.1), "ridge end"
    assert not inside(RIGHT_WALL_IN_X + 0.03, YC, Z1 + 0.03), "ridge clear of the wall when seated"
    # Grip
    for gx in GRIP_XS:
        assert not inside(gx, YC, Z0 + GRIP_D - 0.03) and inside(gx, YC, Z0 + GRIP_D + 0.03), ("grip depth", gx)
        assert inside(gx, GRIP_Y1 + 0.03, Z0 + 0.5) and not inside(gx, GRIP_Y1 - 0.03, Z0 + 0.5), ("grip end", gx)
    bb = part.bounding_box()
    report.append(f"assembled X {bb.min.X:.3f} to {bb.max.X:.3f}, Y {bb.min.Y:.3f} to {bb.max.Y:.3f}, "
                  f"Z {bb.min.Z:.3f} to {bb.max.Z:.3f}")


# ---------------------------------------------------------------------------
# Model
# ---------------------------------------------------------------------------
def _blank():
    return _prism_yz([(Y0N, Z0), (Y1N, Z0), (Y1W, Z1), (Y0W, Z1)], X0, X1)


def _build_base():
    plate = _blank()
    if not FEATURES:
        return plate
    # Feet
    for cx, cy in FOOT_CENTRES:
        plate -= bd.Pos(cx, cy, Z0 - 0.01) * bd.Cylinder(
            FOOT_R, p.FOOT_RECESS + 0.01, align=(bd.Align.CENTER, bd.Align.CENTER, bd.Align.MIN)
        )
    # Vents
    for i in range(VENT_N):
        sx = VENT_SLOT_X0 + i * VENT_PITCH
        plate -= _box(sx, sx + VENT_SLOT_W, p.VENT_Y0, p.VENT_Y1, Z0 - 0.01, Z1 + 0.01)
    # Pad seat
    plate -= _box(SEAT_X0, SEAT_X1, SEAT_Y0, SEAT_Y1, SEAT_Z, Z1 + 0.01)
    # Grip grooves
    for gx in GRIP_XS:
        plate -= _prism_xz(
            [(gx - GRIP_HALF - 0.01 / math.tan(math.radians(GRIP_DEG)), Z0 - 0.01),
             (gx + GRIP_HALF + 0.01 / math.tan(math.radians(GRIP_DEG)), Z0 - 0.01),
             (gx, Z0 + GRIP_D)],
            GRIP_Y0, GRIP_Y1,
        )
    # Detent ridge
    plate += _prism_xz(
        [(RIDGE_X0, Z1 - 0.01), (RIDGE_X1, Z1 - 0.01), (RIDGE_X1 - RIDGE_RUN, Z1 + RIDGE_H),
         (RIDGE_X0 + RIDGE_RUN, Z1 + RIDGE_H)],
        RIDGE_Y0, RIDGE_Y1,
    )
    return plate


def _chamfer_bed(shape):
    """Bed chamfer on the outer underside edges only, so the walls round the foot recesses keep their width."""
    bb = shape.bounding_box()

    def outer(e):
        c = e.center()
        return (abs(c.X - bb.min.X) < 1e-6 or abs(c.X - bb.max.X) < 1e-6
                or abs(c.Y - Y0N) < 1e-6 or abs(c.Y - Y1N) < 1e-6)

    edges = [e for e in shape.edges() if abs(e.bounding_box().max.Z) < 1e-6 and outer(e)]
    out = bd.chamfer(edges, BED_CHAMFER)
    return out, len(edges)


def _part():
    """Returns (print frame solid, report lines). The assembled part is the same solid moved back."""
    report = []
    check_values(report)
    raw = _build_base().moved(_to_print())
    raw = raw.solids()[0] if len(raw.solids()) == 1 else raw
    solid, n = _chamfer_bed(raw)
    report.append(f"bed chamfer {BED_CHAMFER} on {n} outer edges")
    return solid, report


@model
def bottom_plate():
    solid, report = _part()
    part = solid.moved(_to_print().inverse())
    check_geometry(part, report)
    for line in report:
        print("REPORT", line)
    part.label = "bottom_plate"
    return part


@model
def bottom_plate_print():
    solid, report = _part()
    bb = solid.bounding_box()
    report.append(f"print size {bb.size.X:.2f} x {bb.size.Y:.2f} x {bb.size.Z:.3f} mm, underside on the bed")
    assert abs(bb.min.Z) < 1e-6 and max(bb.size.X, bb.size.Y, bb.size.Z) <= p.PRINT_BED, bb.size
    for line in report:
        print("REPORT", line)
    solid.label = "bottom_plate_print"
    return solid


if __name__ == "__main__":
    bottom_plate()
    bottom_plate_print()
