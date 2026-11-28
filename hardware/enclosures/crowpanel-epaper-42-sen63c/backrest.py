# SPDX-License-Identifier: CC-BY-4.0. Copyright Walkthru.Earth.
"""Backrest with the top release latch, CrowPanel 4.2 and SEN63C desk base, iteration 1.

DESIGN.md "2. Backrest with top latch", PLAN-iteration-1.md Task 4. A spine
behind the panel from a dovetail tongue below the base deck up to the panel
top edge, with one tapered latch arm that hooks over the shell top edge.

Frames. Built in the panel frame (x along the panel, y up the panel, z out of
the display) and placed with params.panel_to_base(). backrest() is in the base
frame at its assembled position. backrest_print() lies on its back (panel
z -16 on the bed), so the arm flexes along the layers.

The arm runs up the panel (y) on the back plane and bends backward (-z). The
head above the panel top is a lever about 18 mm tall, so the arm rotation
lifts the tooth (+y) off the shell front. Push the thumb pad back to release.

Check: pixi run -e cad cad run backrest.py, then
pixi run -e cad cad model dfm backrest.py:backrest_print --process printing --technology fdm --material pla
"""

import math
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))

import build123d as bd  # noqa: E402
from cad import model  # noqa: E402

import params as p  # noqa: E402

# ---------------------------------------------------------------------------
# Part values, panel frame. Each comes from params.py or is a named builder call.
# ---------------------------------------------------------------------------
SPINE_X0 = 47.0  # DES Backrest bullet, spine at x 47 to 67 (moved 2026-11-19)
SPINE_X1 = 67.0  # DES Backrest bullet
SPINE_XC = (SPINE_X0 + SPINE_X1) / 2  # 57.0, arm centred on the spine
BACK_Z = p.DOVETAIL_BACKREST_BACK_Z  # -16.0, spine back face, the bed in print
FRONT_Z = p.BACKREST_FRONT_Z  # -7.8, 0.2 behind the back cover
LOWER_FRONT_Z = p.BACKREST_LOWER_FRONT_Z  # -11.65, 0.2 behind the cradle rear wall
STEP_Y = p.CRADLE_REAR_WALL_TOP_PANEL_Y + p.FIT_SLIDE  # 12.2, step 0.2 above the rear wall top
SPINE_Y_LOW = p.DOVETAIL_BACKREST_BOTTOM_Y  # builder call, start of the lower block before the deck cut

ARM_X0 = SPINE_XC - p.LATCH_B / 2  # 54.0
ARM_X1 = SPINE_XC + p.LATCH_B / 2  # 60.0
PRINT_GAP = 0.8  # builder call, in-part gap that prints open, cad dfm printing.gap asks for 0.762 (Xometry)
SLOT = PRINT_GAP  # gap each side of the arm, twice the MAT 6.1 0.4 minimum (FIT_LATCH_GAP)
RAIL_Y1 = p.PANEL_SHELL_Y1 + p.FIT_LATCH_GAP - PRINT_GAP  # 86.75, side rails end PRINT_GAP below the head
HEAD_Y0 = p.PANEL_SHELL_Y1 + p.FIT_LATCH_GAP  # 87.55, head underside 0.4 above the shell top
ARM_TIP_Y = HEAD_Y0  # arm ends where the head starts
ARM_ROOT_Y = ARM_TIP_Y - p.LATCH_L  # 69.55, free length LATCH_L 18

HEAD_X0 = SPINE_XC - 5.0  # builder call, 10 mm thumb tab, 2 mm wider than the arm each side
HEAD_X1 = SPINE_XC + 5.0
HEAD_Y1 = 97.0  # builder call, 9.45 of thumb tab above the head underside
TOOTH_CLEAR = p.FIT_SLIDE  # builder call, return face passes 0.2 above the shell front corner
TOOTH_TIP_Y = p.PANEL_SHELL_Y1 - p.LATCH_CATCH  # 86.15, catch overlap LATCH_CATCH 1.0
TOOTH_RET_RUN = (HEAD_Y0 - TOOTH_TIP_Y) / math.tan(math.radians(p.LATCH_RETURN_DEG))  # 1.4 at 45 deg
TOOTH_Z0 = p.PANEL_TOP_COVER_Z1 + TOOTH_CLEAR - (HEAD_Y0 - p.PANEL_SHELL_Y1) * math.tan(
    math.radians(p.LATCH_RETURN_DEG)
)  # 1.3, return face low end at the head
TOOTH_LAND = 0.5  # builder call, flat tip land, no knife edge (above PLA_WALL_1 0.45)
TOOTH_TIP_Z0 = TOOTH_Z0 + TOOTH_RET_RUN  # 2.7
TOOTH_TIP_Z1 = TOOTH_TIP_Z0 + TOOTH_LAND  # 3.2
HEAD_Z1 = TOOTH_TIP_Z1 + (HEAD_Y0 - TOOTH_TIP_Y) / math.tan(math.radians(p.LATCH_LEAD_DEG))  # 5.625, 30 deg lead
RIB_H = 0.6  # builder call, thumb grip ribs
RIB_W = 1.0  # builder call, above PLA_MIN_WALL 0.8
RIB_YS = (91.0, 93.0, 95.0)  # builder call, three ribs on the thumb tab front face
OVERLAP = 0.5  # builder call, solids overlap this much where they join

TONGUE_Y0 = p.DOVETAIL_BACKREST_BOTTOM_Y + p.DOVETAIL_BACKREST_FIT  # -14.2, 0.2 short of the socket end
TONGUE_XC = (p.DOVETAIL_BACKREST_X0 + p.DOVETAIL_BACKREST_X1) / 2  # 62.03
DETENT_FLAT = 1.0  # builder call, flat top of the detent ridge
DETENT_RAMP_DEG = 30.0  # builder call, ramp both ways so the tongue can be pulled out
DETENT_X_L = 4.0  # builder call, ridge length across the narrow face (6.19)
DETENT_Y = TONGUE_Y0 + 3.0  # builder call, ridge centre 3 mm above the tongue end, the end detent
BED_CHAMFER = 0.4  # PLAN Task 3 Step 3 bed edge chamfer, takes the elephant foot (MAT 1.3, 0.10)

# Lever model inputs
TAB_LOADS_E = (1.0, 5.0, HEAD_Y1 - HEAD_Y0)  # thumb push points above the arm tip, worst of three
SWEEP_STEP_DEG = 0.1  # sweep resolution for the tipping panel
SWEEP_MAX_DEG = 25.0  # panel tipped forward this far is well clear of the latch
SHELL_SECTION = ((60.0, p.PANEL_BACK_COVER_Z0), (p.PANEL_SHELL_Y1, p.PANEL_BACK_COVER_Z0),
                 (p.PANEL_SHELL_Y1, p.PANEL_TOP_COVER_Z1), (60.0, p.PANEL_TOP_COVER_Z1))  # BRD shell, y-z section


def _box(x0, x1, y0, y1, z0, z1):
    return bd.Pos((x0 + x1) / 2, (y0 + y1) / 2, (z0 + z1) / 2) * bd.Box(x1 - x0, y1 - y0, z1 - z0)


def _prism_yz(pts, x0, x1):
    """Prism along +X from x0 to x1 with a section given as (y, z) points."""
    face = bd.Plane.YZ.offset(x0) * bd.Polygon(*pts, align=None)
    return bd.extrude(face, x1 - x0, dir=(1, 0, 0))  # explicit, a polygon winding can flip the face normal


def _probe(shape, pt):
    return shape.is_inside(bd.Vector(*pt))


def _arm_h(s):
    return p.LATCH_H_ROOT - (p.LATCH_H_ROOT - p.LATCH_H_TIP) * s / p.LATCH_L


# ---------------------------------------------------------------------------
# Lever model, pure Python. Euler-Bernoulli, tapered arm, small strain.
# ---------------------------------------------------------------------------
def _lever(moment, axial=0.0, n=720):
    """Integrate the arm from root to tip.

    moment(y, zn) gives the bending moment (N mm) that bends the arm toward -z.
    Returns tip rotation (rad), tip displacement (dy, dz) and peak strain (%).
    """
    ds = p.LATCH_L / n
    theta = dy = dz = eps_max = 0.0
    for i in range(n):
        s = (i + 0.5) * ds
        h = _arm_h(s)
        m = moment(ARM_ROOT_Y + s, BACK_Z + h / 2)
        k = 12.0 * m / (p.PLA_E_XY_MPA * p.LATCH_B * h**3)
        eps = 6.0 * m / (p.PLA_E_XY_MPA * p.LATCH_B * h**2) + abs(axial) / (p.PLA_E_XY_MPA * p.LATCH_B * h)
        tm = theta + k * ds / 2
        dy += (math.cos(tm) - 1.0) * ds
        dz -= math.sin(tm) * ds
        theta += k * ds
        eps_max = max(eps_max, eps)
    return theta, (dy, dz), 100.0 * eps_max


def _deform(pts, theta, disp):
    """Head points carried rigidly by the arm tip."""
    ty, tz = ARM_TIP_Y, BACK_Z + p.LATCH_H_TIP / 2
    c, s = math.cos(theta), math.sin(theta)
    out = []
    for y, z in pts:
        ry, rz = y - ty, z - tz
        out.append((ty + disp[0] + ry * c + rz * s, tz + disp[1] - ry * s + rz * c))
    return out


def _tilt(pts, deg):
    """Panel points tipped forward about the cradle pivot (top moves +z)."""
    py, pz = p.CRADLE_PIVOT_PANEL
    c, s = math.cos(math.radians(deg)), math.sin(math.radians(deg))
    return [(py + (y - py) * c - (z - pz) * s, pz + (y - py) * s + (z - pz) * c) for y, z in pts]


def _hit(a, b):
    """Separating axis test for two convex polygons. True when they overlap."""
    for poly in (a, b):
        for i in range(len(poly)):
            (y1, z1), (y2, z2) = poly[i], poly[(i + 1) % len(poly)]
            ny, nz = z2 - z1, y1 - y2
            pa = [ny * y + nz * z for y, z in a]
            pb = [ny * y + nz * z for y, z in b]
            if max(pa) <= min(pb) + 1e-9 or max(pb) <= min(pa) + 1e-9:
                return False
    return True


_PV = p.CRADLE_PIVOT_PANEL
SWEEP_Y_MAX = _PV[0] + math.hypot(p.PANEL_SHELL_Y1 - _PV[0], p.PANEL_TOP_COVER_Z1 - p.PANEL_BACK_COVER_Z0)  # 87.62
FULL_TRAVEL = SWEEP_Y_MAX - TOOTH_TIP_Y + p.FIT_LATCH_GAP  # 1.87, tooth rise at full tab travel

TOOTH_PTS = ((HEAD_Y0, TOOTH_Z0), (TOOTH_TIP_Y, TOOTH_TIP_Z0), (TOOTH_TIP_Y, TOOTH_TIP_Z1), (HEAD_Y0, HEAD_Z1))
HEAD_PTS = ((HEAD_Y0, BACK_Z), (HEAD_Y1, BACK_Z), (HEAD_Y1, HEAD_Z1), (HEAD_Y0, HEAD_Z1))
RAIL_PTS = ((ARM_ROOT_Y, BACK_Z), (RAIL_Y1, BACK_Z), (RAIL_Y1, FRONT_Z), (ARM_ROOT_Y, FRONT_Z))


def _sweep_hits(theta, disp):
    """Does the shell, tipped 0 to SWEEP_MAX_DEG, touch the deflected latch or the rails."""
    latch = [_deform(TOOTH_PTS, theta, disp), _deform(HEAD_PTS, theta, disp), list(RAIL_PTS)]
    n = int(round(SWEEP_MAX_DEG / SWEEP_STEP_DEG))
    for i in range(n + 1):
        shell = _tilt(SHELL_SECTION, i * SWEEP_STEP_DEG)
        if any(_hit(shell, part) for part in latch):
            return i * SWEEP_STEP_DEG
    return None


def _solve(case, target):
    """Smallest load (N) for which target(theta, disp, eps) holds.

    The bracket grows from 0.01 N by doubling, so it stays in the small
    rotation range where the tooth rise grows with the load, then bisects.
    """
    lo, hi = 0.0, 0.01
    while not target(*case(hi)):
        lo, hi = hi, hi * 2.0
        assert hi < 1000.0, "no load reaches the target"
    for _ in range(50):
        mid = (lo + hi) / 2
        if target(*case(mid)):
            hi = mid
        else:
            lo = mid
    return hi


def _solve_strain(case):
    """Load (N) at which the peak strain reaches PLA_STRAIN_LIMIT_XY."""
    return _solve(case, lambda t, d, e: e >= p.PLA_STRAIN_LIMIT_XY)


def _tab_case(e):
    y_tab = ARM_TIP_Y + e
    return lambda f: _lever(lambda y, zn: f * (y_tab - y))


def _insert_case(yc, zc):
    # Contact on the 30 degree lead face, force normal to it, into the tooth.
    fy = math.cos(math.radians(p.LATCH_LEAD_DEG))  # 0.866, lifts the tooth
    fz = math.sin(math.radians(p.LATCH_LEAD_DEG))  # 0.5, pushes it back
    return lambda f: _lever(lambda y, zn: f * fz * (yc - y) + f * fy * (zc - zn), axial=f * fy)


def _rise(theta, disp):
    return _deform([(TOOTH_TIP_Y, TOOTH_TIP_Z0)], theta, disp)[0][0] - TOOTH_TIP_Y


def check_latch(report):
    """PLAN Task 4 latch asserts. Returns nothing, appends to report."""
    eps_t = p.latch_strain_tapered(p.LATCH_L)
    eps_c = p.latch_strain_constant(p.LATCH_L)
    report.append(f"latch formula strain, tapered {eps_t:.3f} %, constant section {eps_c:.3f} %")
    assert eps_t <= p.PLA_STRAIN_LIMIT_XY, eps_t

    overlap = p.PANEL_SHELL_Y1 - TOOTH_TIP_Y
    report.append(f"catch overlap of the shell front {overlap:.3f} mm")
    assert overlap <= 1.2 + 1e-9 and abs(overlap - p.LATCH_CATCH) < 1e-9
    assert abs(HEAD_Y0 - p.PANEL_SHELL_Y1 - p.FIT_LATCH_GAP) < 1e-9, "head underside 0.4 over the shell top"
    lead = math.degrees(math.atan2(HEAD_Y0 - TOOTH_TIP_Y, HEAD_Z1 - TOOTH_TIP_Z1))
    ret = math.degrees(math.atan2(HEAD_Y0 - TOOTH_TIP_Y, TOOTH_TIP_Z0 - TOOTH_Z0))
    assert abs(lead - p.LATCH_LEAD_DEG) < 1e-9 and abs(ret - p.LATCH_RETURN_DEG) < 1e-9, (lead, ret)
    assert ret >= p.PLA_OVERHANG_DEG, "return face is the print overhang"

    # At rest the shell is clear, and with no tab push the latch stops a forward tip.
    rest = _sweep_hits(0.0, (0.0, 0.0))
    assert rest is not None and rest > 0.0, rest
    report.append(f"latch at rest, the panel meets the tooth after {rest:.1f} deg of forward tip")

    # Release. Full tab travel lifts the tooth tip FIT_LATCH_GAP past the highest
    # point the shell top back corner reaches as the panel tips (SWEEP_Y_MAX).
    travel = FULL_TRAVEL
    report.append(f"shell corner sweep reaches panel y {SWEEP_Y_MAX:.3f}, full tab travel lifts the tooth {travel:.3f}")
    assert travel >= p.LATCH_CATCH + p.FIT_LATCH_GAP
    worst = 0.0
    for e in TAB_LOADS_E:
        case = _tab_case(e)
        f_full = _solve(case, lambda t, d, _e: _rise(t, d) >= travel)
        th, d, eps = case(f_full)
        assert _sweep_hits(th, d) is None, f"tab at e {e}, the tipping shell still meets the latch"
        f_clear = _solve(case, lambda t, d, _e: _sweep_hits(t, d) is None)
        th_c, d_c, eps_c2 = case(f_clear)
        th_l, d_l, _ = case(_solve_strain(case))
        report.append(
            f"tab push {e:.2f} above the tip: full travel rise {_rise(th, d):.3f} at {f_full:.2f} N, "
            f"strain {eps:.3f} %, thumb point moves back {HEAD_Z1 - _deform([(ARM_TIP_Y + e, HEAD_Z1)], th, d)[0][1]:.3f} mm; clears the tipping shell from rise "
            f"{_rise(th_c, d_c):.3f} (strain {eps_c2:.3f} %); 1.2 % reached at rise {_rise(th_l, d_l):.3f}"
        )
        worst = max(worst, eps)
        assert eps <= p.PLA_STRAIN_LIMIT_XY, (e, eps)
        assert _rise(th_c, d_c) <= travel, "full travel clears the tipping shell"
    report.append(f"release worst root strain {worst:.3f} % at full tab travel, limit {p.PLA_STRAIN_LIMIT_XY} %")

    # Insertion. The tipping shell back corner cams the tooth up on the lead face.
    worst = 0.0
    for yc, zc in ((TOOTH_TIP_Y, TOOTH_TIP_Z1), ((TOOTH_TIP_Y + HEAD_Y0) / 2, (TOOTH_TIP_Z1 + HEAD_Z1) / 2)):
        case = _insert_case(yc, zc)
        f = _solve(case, lambda t, d, _e: _sweep_hits(t, d) is None)
        th, d, eps = case(f)
        report.append(
            f"insert, contact y {yc:.2f} z {zc:.2f}: clears at rise {_rise(th, d):.3f}, {f:.2f} N normal, strain {eps:.3f} %"
        )
        worst = max(worst, eps)
        assert eps <= p.PLA_STRAIN_LIMIT_XY, (yc, eps)
    report.append(f"insert worst root strain {worst:.3f} % (no friction), limit {p.PLA_STRAIN_LIMIT_XY} %")


# ---------------------------------------------------------------------------
# Geometry
# ---------------------------------------------------------------------------
def _spine_panel():
    """Spine body without the tongue, panel frame, not yet cut at the deck."""
    lower = _box(SPINE_X0, SPINE_X1, SPINE_Y_LOW, STEP_Y + OVERLAP, BACK_Z, LOWER_FRONT_Z)
    upper = _box(SPINE_X0, SPINE_X1, STEP_Y, ARM_ROOT_Y, BACK_Z, FRONT_Z)
    rail_l = _box(SPINE_X0, ARM_X0 - SLOT, ARM_ROOT_Y - OVERLAP, RAIL_Y1, BACK_Z, FRONT_Z)
    rail_r = _box(ARM_X1 + SLOT, SPINE_X1, ARM_ROOT_Y - OVERLAP, RAIL_Y1, BACK_Z, FRONT_Z)
    return lower + upper + rail_l + rail_r


def _latch_panel():
    """Arm, root fillet, head, tooth and thumb ribs, panel frame."""
    s0, s1 = -OVERLAP, p.LATCH_L + OVERLAP
    arm = _prism_yz(
        [(ARM_ROOT_Y + s0, BACK_Z), (ARM_ROOT_Y + s1, BACK_Z),
         (ARM_ROOT_Y + s1, BACK_Z + _arm_h(s1)), (ARM_ROOT_Y + s0, BACK_Z + _arm_h(s0))],
        ARM_X0, ARM_X1,
    )
    r = p.LATCH_ROOT_FILLET
    zf = BACK_Z + p.LATCH_H_ROOT
    sq = _prism_yz([(ARM_ROOT_Y - OVERLAP, zf - OVERLAP), (ARM_ROOT_Y + r, zf - OVERLAP), (ARM_ROOT_Y + r, zf + r),
                    (ARM_ROOT_Y - OVERLAP, zf + r)], ARM_X0, ARM_X1)
    cut = bd.Pos(ARM_X0 - 1.0, ARM_ROOT_Y + r, zf + r) * bd.Rot(0, 90, 0) * bd.Cylinder(
        r, ARM_X1 - ARM_X0 + 2.0, align=(bd.Align.CENTER, bd.Align.CENTER, bd.Align.MIN))
    fillet = sq - cut
    head = _box(HEAD_X0, HEAD_X1, HEAD_Y0, HEAD_Y1, BACK_Z, HEAD_Z1)
    tooth = _prism_yz([(HEAD_Y0 + OVERLAP, TOOTH_Z0), *TOOTH_PTS, (HEAD_Y0 + OVERLAP, HEAD_Z1)], HEAD_X0, HEAD_X1)
    ribs = [_box(HEAD_X0, HEAD_X1, y - RIB_W / 2, y + RIB_W / 2, HEAD_Z1 - OVERLAP, HEAD_Z1 + RIB_H) for y in RIB_YS]
    out = arm + fillet + head + tooth
    for rib in ribs:
        out += rib
    return out


def _tongue_panel():
    """Dovetail tongue, wide face at the back, with the end detent ridge on the narrow face."""
    x0, x1 = p.DOVETAIL_BACKREST_X0, p.DOVETAIL_BACKREST_X1
    hn = p.DOVETAIL_BACKREST_NARROW / 2
    y1 = STEP_Y
    sec = bd.Polygon((x0, BACK_Z), (x1, BACK_Z), (TONGUE_XC + hn, p.DOVETAIL_BACKREST_FRONT_Z),
                     (TONGUE_XC - hn, p.DOVETAIL_BACKREST_FRONT_Z), align=None)
    # Section lies in panel XZ, extruded along +y.
    face = bd.Plane.XZ.offset(-TONGUE_Y0) * sec
    tongue = bd.extrude(face, y1 - TONGUE_Y0, dir=(0, 1, 0))
    d = p.DOVETAIL_BACKREST_DETENT
    run = d / math.tan(math.radians(DETENT_RAMP_DEG))
    zf = p.DOVETAIL_BACKREST_FRONT_Z
    half = DETENT_FLAT / 2
    ridge = _prism_yz(
        [(DETENT_Y - half - run, zf - OVERLAP), (DETENT_Y + half + run, zf - OVERLAP), (DETENT_Y + half + run, zf),
         (DETENT_Y + half, zf + d), (DETENT_Y - half, zf + d), (DETENT_Y - half - run, zf)],
        TONGUE_XC - DETENT_X_L / 2, TONGUE_XC + DETENT_X_L / 2,
    )
    return tongue + ridge


def _build_base():
    loc = p.panel_to_base()
    spine = (_spine_panel() + _latch_panel()).moved(loc)
    keep = _box(-50.0, 200.0, -100.0, 300.0, p.BASE_DECK_Z, 300.0)
    spine = spine & keep
    return spine + _tongue_panel().moved(loc)


def _to_print():
    """Base frame to print frame. Panel z -16 to Z 0, panel x 47 to X 0, tongue end to Y 0."""
    return bd.Location((-SPINE_X0, -TONGUE_Y0, -BACK_Z)) * p.panel_to_base().inverse()


def _chamfer_bed(shape):
    """0.4 chamfer on the outer bed edges, not on the arm, so the lever model holds."""
    def on_arm(e):
        c = e.center()
        return (ARM_X0 - SLOT - SPINE_X0 - 0.01 <= c.X <= ARM_X1 + SLOT - SPINE_X0 + 0.01
                and ARM_ROOT_Y - TONGUE_Y0 - 0.01 <= c.Y <= HEAD_Y0 - TONGUE_Y0 + 0.01)
    edges = [e for e in shape.edges() if abs(e.bounding_box().max.Z) < 1e-6 and not on_arm(e)]
    assert edges, "no bed edges to chamfer"
    # No fallback. A refused chamfer fails the build, so the part never ships
    # without its elephant foot relief.
    return bd.chamfer(edges, BED_CHAMFER), len(edges)


def _part():
    """Returns (print frame solid, report lines). The assembled part is the same solid moved back."""
    report = []
    check_latch(report)
    raw = _build_base().moved(_to_print())
    raw = raw.solids()[0] if len(raw.solids()) == 1 else raw
    solid, n = _chamfer_bed(raw)
    assert SLOT >= p.FIT_LATCH_GAP and HEAD_Y0 - RAIL_Y1 >= p.FIT_LATCH_GAP
    report.append(f"bed chamfer {BED_CHAMFER} on {n} edges")
    return solid, report


def _pp(x, y, z):
    return p.panel_point_to_base(x, y, z)


def check_geometry(part, report):
    """PLAN Task 4 backrest asserts on the base frame solid."""
    assert len(part.solids()) == 1, len(part.solids())
    assert p.PANEL_TF_X1 < SPINE_X0 and SPINE_X1 < p.PANEL_HEAT_X0, "spine clear of the TF slot and the heat zone"
    assert SPINE_X1 < p.PANEL_BOOT_CENTRE[0] - p.PANEL_BUTTON_HOLE_D / 2, "spine clear of the BOOT pin path"
    bb = part.bounding_box()
    report.append(f"assembled X {bb.min.X:.3f} to {bb.max.X:.3f}, Y {bb.min.Y:.2f} to {bb.max.Y:.2f}, "
                  f"Z {bb.min.Z:.2f} to {bb.max.Z:.2f}")
    assert SPINE_X0 - 1e-6 <= bb.min.X and bb.max.X <= SPINE_X1 + 1e-6, (bb.min.X, bb.max.X)

    def inside(x, y, z):
        return _probe(part, _pp(x, y, z))

    # Spine faces and gaps
    assert abs(p.PANEL_BACK_COVER_Z0 - FRONT_Z - p.FIT_SLIDE) < 1e-9, "0.2 behind the back cover"
    assert inside(50.0, 40.0, FRONT_Z - 0.03) and not inside(50.0, 40.0, FRONT_Z + 0.03)
    assert inside(50.0, 40.0, BACK_Z + 0.03) and not inside(50.0, 40.0, BACK_Z - 0.03)
    assert abs(p.CRADLE_REAR_OUT_Z - LOWER_FRONT_Z - p.FIT_SLIDE) < 1e-9, "0.2 behind the rear wall"
    assert inside(50.0, 9.0, LOWER_FRONT_Z - 0.03) and not inside(50.0, 9.0, LOWER_FRONT_Z + 0.03)
    assert inside(50.0, RAIL_Y1 - 0.03, -10.0) and not inside(50.0, RAIL_Y1 + 0.03, -10.0), "rails end PRINT_GAP below the head"
    # Spine sits on the deck, only the tongue goes below it
    for z in (BACK_Z + 0.5, LOWER_FRONT_Z - 0.1):  # 0.5 above the bed chamfer
        y_deck = p.panel_y_at_base_z(z, p.BASE_DECK_Z)
        assert inside(50.0, y_deck + 0.05, z) and not inside(50.0, y_deck - 0.05, z), z
    # Tongue section and fit
    fit = p.DOVETAIL_BACKREST_FIT
    assert abs(p.DOVETAIL_BACKREST_X0 - p.DOVETAIL_BACKREST_SOCKET_X0 - fit) < 1e-9
    assert abs(p.DOVETAIL_BACKREST_SOCKET_X1 - p.DOVETAIL_BACKREST_X1 - fit) < 1e-9
    assert abs(p.DOVETAIL_BACKREST_SOCKET_FRONT_Z - p.DOVETAIL_BACKREST_FRONT_Z - fit) < 1e-9
    assert abs(p.DOVETAIL_BACKREST_X0 - 57.2) < 1e-9 and abs(p.DOVETAIL_BACKREST_X1 - 66.855) < 1e-9
    yb = 0.0  # below the deck at every z of the tongue
    for z in (BACK_Z + 0.5, p.DOVETAIL_BACKREST_FRONT_Z - 0.05):  # 0.5 above the bed chamfer
        assert _pp(TONGUE_XC, yb, z)[2] < p.BASE_DECK_Z - 1.0
        f = (z - BACK_Z) / p.DOVETAIL_BACKREST_DEPTH
        hw = p.DOVETAIL_BACKREST_WIDE / 2 - f * (p.DOVETAIL_BACKREST_WIDE - p.DOVETAIL_BACKREST_NARROW) / 2
        for sgn in (-1, 1):
            assert inside(TONGUE_XC + sgn * (hw - 0.03), yb, z) and not inside(TONGUE_XC + sgn * (hw + 0.03), yb, z), z
    assert inside(TONGUE_XC, TONGUE_Y0 + 0.03, -14.0) and not inside(TONGUE_XC, TONGUE_Y0 - 0.03, -14.0)
    assert not inside(TONGUE_XC, 0.0, p.DOVETAIL_BACKREST_FRONT_Z + 0.03), "no spine in front of the tongue below the deck"
    zf, d = p.DOVETAIL_BACKREST_FRONT_Z, p.DOVETAIL_BACKREST_DETENT
    assert inside(TONGUE_XC, DETENT_Y, zf + d - 0.03) and not inside(TONGUE_XC, DETENT_Y, zf + d + 0.03)
    report.append(f"tongue x {p.DOVETAIL_BACKREST_X0} to {p.DOVETAIL_BACKREST_X1}, fit {fit} per side and front, "
                  f"detent {d} at panel y {DETENT_Y:.3f}, base {tuple(round(v, 3) for v in _pp(TONGUE_XC, DETENT_Y, zf))}")
    # Arm, slots, head, tooth
    for s in (2.0, 9.0, 16.0):
        h = _arm_h(s)
        assert inside(SPINE_XC, ARM_ROOT_Y + s, BACK_Z + h - 0.03) and not inside(SPINE_XC, ARM_ROOT_Y + s, BACK_Z + h + 0.03), s
        assert not inside(ARM_X0 - SLOT / 2, ARM_ROOT_Y + s, BACK_Z + 0.5) and not inside(ARM_X1 + SLOT / 2, ARM_ROOT_Y + s, BACK_Z + 0.5)
    assert not inside(SPINE_XC, HEAD_Y0 - 0.03, -10.0) and inside(SPINE_XC, HEAD_Y0 + 0.03, -10.0), "head underside"
    assert inside(SPINE_XC, TOOTH_TIP_Y + 0.03, TOOTH_TIP_Z0 + 0.25) and not inside(SPINE_XC, TOOTH_TIP_Y - 0.03, TOOTH_TIP_Z0 + 0.25)
    assert inside(SPINE_XC, HEAD_Y1 - 0.5, HEAD_Z1 - 0.03) and not inside(SPINE_XC, HEAD_Y1 - 0.5, HEAD_Z1 + 0.03)
    assert inside(SPINE_XC, RIB_YS[0], HEAD_Z1 + RIB_H - 0.03), "thumb rib"


@model
def backrest():
    solid, report = _part()
    part = solid.moved(_to_print().inverse())
    check_geometry(part, report)
    for line in report:
        print("REPORT", line)
    part.label = "backrest"
    return part


@model
def backrest_print():
    solid, report = _part()
    bb = solid.bounding_box()
    report.append(f"print size {bb.size.X:.2f} x {bb.size.Y:.2f} x {bb.size.Z:.2f} mm")
    assert abs(bb.min.Z) < 1e-6 and max(bb.size.X, bb.size.Y, bb.size.Z) <= p.PRINT_BED, bb.size
    for line in report:
        print("REPORT", line)
    solid.label = "backrest_print"
    return solid


if __name__ == "__main__":
    backrest()
    backrest_print()
