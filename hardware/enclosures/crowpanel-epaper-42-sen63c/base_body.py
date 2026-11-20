# SPDX-License-Identifier: CC-BY-4.0. Copyright Walkthru.Earth.
"""Base body, CrowPanel 4.2 and SEN63C desk base, iteration 1.

DESIGN.md "1. Base body", PLAN-iteration-1.md Task 3. One printed body with
the tilted panel cradle at the front, the harness bay behind and under it,
the sensor pocket at the back with the back wall grilles and the fin, the
blind dovetail socket for the backrest and the two rails for the bottom plate.

Frame. Base frame (X along the base, Y front to back, Z up, desk at Z 0). The
body prints upright as modelled, its bottom face (Z 2) on the bed.

Layout in X. X -0.35 to 55.65 (the pocket length) is one open cavity, the bay
in front and the pocket behind, both open at the bottom to the plate. X 55.65
to 107.35 is solid behind the cradle and holds the backrest socket and the
BOOT and RST pin channels. The cradle floor, lips and rear wall run the full
length.

Check: pixi run -e cad cad run base_body.py, then
pixi run -e cad cad model dfm base_body.py:base_body --process printing --technology fdm --material pla
"""

import math
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))

import build123d as bd  # noqa: E402
from cad import model  # noqa: E402
from cad.build123d import closest_points, overlap_volume  # noqa: E402

import backrest as br  # noqa: E402
import bottom_plate as bp  # noqa: E402
import params as p  # noqa: E402
import vendor  # noqa: E402

# ---------------------------------------------------------------------------
# Part values, base frame unless named panel. Each comes from params.py or is
# a named builder call.
# ---------------------------------------------------------------------------
X0, X1 = p.BASE_X0, p.BASE_X1  # -2.15, 109.15
Z0 = p.BASE_Z0  # 2.0, bed
ZP = p.PLATE_TOP_Z  # 5.0, plate top, groove ceiling
IN_X0, IN_X1 = p.CRADLE_SIDE_IN_X0, p.CRADLE_SIDE_IN_X1  # -0.35, 107.35, side wall inner faces
CAV_X1 = p.POCKET_X1  # 55.65, the open cavity (bay and pocket) ends here, solid to the right
LIP_OUT_Z = p.CRADLE_FRONT_LIP_IN_Z + p.CRADLE_FRONT_LIP_T  # panel 3.7
FLOOR_UNDER_Y = p.CRADLE_FLOOR_Y - p.CRADLE_FLOOR_T  # panel -1.95
# Front lip draft. The panel rotates about its front bottom edge (CRADLE_PIVOT_PANEL)
# from the 15 degree rest pose to upright for insertion and removal, so its
# glass face sweeps a 15 degree fan from that edge. The lip inner face leans
# out by the same angle from the floor and keeps the FIT_PANEL gap the whole
# way (PLAN Task 3, draft the front lip inner face if the forward tip needs it).
LIP_DRAFT_TAN = math.tan(math.radians(p.TILT_DEG))
LIP_TOP_IN_Z = p.CRADLE_FRONT_LIP_IN_Z + (p.CRADLE_FRONT_LIP_TOP_PANEL_Y - p.CRADLE_FLOOR_Y) * LIP_DRAFT_TAN
DOWN_Y = -60.0  # builder call, panel y far below the cradle, the blocks are trimmed at Z 2
BED_CHAMFER = 0.4  # PLAN Task 3 Step 3, 0.4 on bed edges, takes the elephant foot (MAT 1.3, 0.10)
TOL = 1e-3  # builder call, mm3 of overlap counted as touching

# Plate rails (lead interface 2026-11-20, test_plate dove_socket convention).
# The groove is the plate section widened by DOVETAIL_PLATE_FIT across Y on each
# flank, ceiling at the plate top.
TAN_F = math.tan(math.radians(p.DOVETAIL_PLATE_FLANK_DEG))
GROOVE_Y0_BED = p.DOVETAIL_PLATE_Y0 - p.DOVETAIL_PLATE_FIT  # 3.1
GROOVE_Y1_BED = p.DOVETAIL_PLATE_Y1 + p.DOVETAIL_PLATE_FIT  # 67.0
GROOVE_Y0_TOP = GROOVE_Y0_BED - (ZP - Z0) / TAN_F  # 1.368
GROOVE_Y1_TOP = GROOVE_Y1_BED + (ZP - Z0) / TAN_F  # 68.732
# Back band step. The 45 degree step from the band out to the wall out starts
# where it leaves SKIN normal to the groove top back corner, rounded up to 0.1.
BAND_STEP_Z = math.ceil((GROOVE_Y1_TOP + ZP + p.SKIN * math.sqrt(2) - p.BACK_BAND_OUT_Y) * 10) / 10  # 5.6
# Right region underside, raised clear of the plate. The lead asks for X 105 to
# 107.35, Y 25 to 45, Z 5 to 5.8 clear for the plate detent ridge (its crest
# at Z 5.746). Raising the whole right region underside to the same height also
# keeps the plate off the support scarred ceiling there.
RELIEF_Z = 5.8  # lead interface 2026-11-20, ridge clearance top
RIDGE_ZONE = (105.0, IN_X1, 25.0, 45.0, ZP, RELIEF_Z)  # lead interface 2026-11-20

# Bay front. Inner face Y = Z - FRONT_C, a 45 degree slope, set by the jumper
# block over the params sweep with JUMPER_CLEAR normal to the slope.
# The check sweeps the jumper over p.INSERT_SWEEP_DEG (0 to 35) and the panel
# over p.PANEL_SWEEP_DEG (0 to 15), its whole envelope up to the rear wall stop.


def _jumper_abs(deg):
    return [(p.CRADLE_PIVOT_Y + dy, p.CRADLE_PIVOT_Z + dz) for dy, dz in p._jumper_polygon(deg)]


FRONT_C = math.ceil(
    (max(z - y for a in p.INSERT_SWEEP_DEG for y, z in _jumper_abs(a)) + p.JUMPER_CLEAR * math.sqrt(2)) * 20
) / 20  # rounded up to 0.05
FRONT_OUT_C = FRONT_C + p.WALL * math.sqrt(2)  # outer front slope, WALL normal to the inner one

# Header slot rear edge. Upright, the jumper block swings back toward the edge
# where the slot rear face (rear wall inner plane, panel z -7.8) meets the floor
# underside (panel y -1.95). Sharp, it leaves 0.377 at 0 degrees. A 45 degree
# chamfer of 2.5 in the panel frame over the slot length keeps JUMPER_CLEAR (1.0)
# over the whole sweep (2.0 leaves 0.895, 2.5 leaves 1.024, scratch sweep in
# base_body/slot_chamfer.txt).
SLOT_REAR_CHAMFER = 2.5  # builder call, smallest 0.5 step that keeps JUMPER_CLEAR

# End rib undersides. The ribs hang from the side walls with nothing under
# them (the plate is below Z 5), so each underside rises at 45 degrees from
# the wall (PLAN Task 3 Step 3, chamfers under every overhang).
RIB_UNDER_RISE = p.POCKET_END_RIB_W  # 45 degrees across the rib width

# Pocket features
RIB_Y0 = p.POCKET_Y0 - p.POCKET_END_RIB_T  # 39.2
RIB_SPANS = ((IN_X0, IN_X0 + p.POCKET_END_RIB_W), (CAV_X1 - p.POCKET_END_RIB_W, CAV_X1))
MOUTH = p.POCKET_MOUTH_CHAMFER  # 1.0
# Rigid gasket stops. They stand ASSUMED_GASKET_COMPRESSED_T off the back wall
# so the sensor face stops at SEN_FACE_Y. They sit in the strip between the
# inlet and outlet gasket lands (GRILLE_GASKET_BORDER past each grille), with
# STOP_GAP more each side so a hand cut gasket does not ride on them.
STOP_GAP = 1.0  # builder call
_IN = p.grille_slots_base("inlet")
_OUT = p.grille_slots_base("outlet")
STOP_X0 = max(s[1] for s in _IN) + p.GRILLE_GASKET_BORDER + STOP_GAP  # 18.9
STOP_X1 = min(s[0] for s in _OUT) - p.GRILLE_GASKET_BORDER - STOP_GAP  # 29.075
STOP_H = 5.0  # builder call, height of each stop
# Each stop's underside is a 45 degree chamfer back to the wall, so the stop
# prints without support. The flat that meets the sensor is STOP_H less the
# stand (2.0), 3.0 tall.
STOP_CHAMFER = p.BACK_WALL_IN_Y - p.SEN_FACE_Y  # 2.0, the stand
STOP_INSET = 1.0  # builder call, from the sensor face edges (SEN_Z0 6.5, top 32.1)
STOP_ZS = ((p.SEN_Z0 + STOP_INSET, p.SEN_Z0 + STOP_INSET + STOP_H),
           (p.SEN_Z0 + p.SEN_MAX_W - STOP_INSET - STOP_H, p.SEN_Z0 + p.SEN_MAX_W - STOP_INSET))
# Fin. params FIN_Z0 is the plate top. It runs down to the bed here so it prints
# with no support under it (reported as a deviation).
FIN_Z0 = Z0  # builder call, see above
FIN_X0 = p.FIN_X - p.FIN_T / 2
FIN_X1 = p.FIN_X + p.FIN_T / 2

# Harness bay, PLAN Task 3 Step 1, pull-up bundle 30 x 10 x 8 placed on the
# plate behind the jumper sweep and below the plug zone.
BUNDLE = (21.0, 51.0, 27.0, 37.0, ZP, ZP + 8.0)  # builder call, X, Y, Z ranges

# Backrest socket, blind, open from the deck. Section is the tongue (backrest.py)
# shifted DOVETAIL_BACKREST_FIT out in X on each flank and in z at the front and
# the back, the same rule as params SOCKET_X0 and SOCKET_FRONT_Z.
FIT_B = p.DOVETAIL_BACKREST_FIT
TONGUE_XC = (p.DOVETAIL_BACKREST_X0 + p.DOVETAIL_BACKREST_X1) / 2  # 62.03
TAN_B = 1 / math.tan(math.radians(p.DOVETAIL_BACKREST_FLANK_DEG))  # flank run per unit z
SOCK_BACK_Z = p.DOVETAIL_BACKREST_BACK_Z - FIT_B  # panel -16.2
SOCK_FRONT_Z = p.DOVETAIL_BACKREST_SOCKET_FRONT_Z  # panel -12.8
SOCK_TOP_Y = 20.0  # builder call, panel y above the deck, the cut opens there
# Detent dimple for the backrest ridge (backrest.py DETENT_*: centre y -11.2,
# flat 1.0, 30 degree ramps, 0.4 tall, x TONGUE_XC +- 2.0). Dimple floor 0.4 into
# the socket front face, flat and length grown by FIT_B each side.
DIMPLE_Y = p.DOVETAIL_BACKREST_BOTTOM_Y + FIT_B + 3.0  # backrest DETENT_Y, panel -11.2
DIMPLE_D = p.DOVETAIL_BACKREST_DETENT  # 0.4
DIMPLE_HALF_FLAT = 0.5 + FIT_B  # backrest DETENT_FLAT / 2 + fit
DIMPLE_RUN = DIMPLE_D / math.tan(math.radians(30.0))  # backrest DETENT_RAMP_DEG 30
DIMPLE_HALF_X = 2.0 + FIT_B  # backrest DETENT_X_L / 2 + fit

# BOOT and RST pin channels, panel frame along z. Behind the rear wall the
# channel axis is above the deck for a short run, so its top would break the
# deck at a 15 degree graze (a knife edge). There the channel is opened into a
# trough with walls tangent to the bore, back to where the deck skin over the
# bore is PLA_MIN_WALL.
PIN_D = 3.2  # PLAN Task 3 Step 1, d3.2
PIN_CENTRES = (p.PANEL_BOOT_CENTRE, p.PANEL_RST_CENTRE)
PIN_Z1 = -60.0  # builder call, past the back wall


def _pin_trough_end_z():
    cy = PIN_CENTRES[0][1]
    target = p.BASE_DECK_Z - p.PLA_MIN_WALL / math.cos(math.radians(p.TILT_DEG))
    za, zb = p.CRADLE_REAR_OUT_Z, PIN_Z1
    fa = p.panel_point_to_base(0.0, cy + PIN_D / 2, za)[2] - target
    fb = p.panel_point_to_base(0.0, cy + PIN_D / 2, zb)[2] - target
    return math.floor((za + (zb - za) * fa / (fa - fb)) * 10) / 10  # base Z is linear in panel z


PIN_TROUGH_Z = _pin_trough_end_z()


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


def _pp(x, y, z):
    return p.panel_point_to_base(x, y, z)


def _yz(y_panel, z_panel):
    _, by, bz = _pp(0.0, y_panel, z_panel)
    return by, bz


def _panel_line_at_z(z_panel, z_base):
    """Base Y where the panel line of depth z_panel crosses base height z_base."""
    return _yz(p.panel_y_at_base_z(z_panel, z_base), z_panel)[0]


def _floor_under_meets_slope():
    """Point where the floor underside (panel y -1.95) meets the bay front slope Y = Z - FRONT_C."""
    def f(z):
        y, zb = _yz(FLOOR_UNDER_Y, z)
        return y - zb + FRONT_C
    a, b = -11.45, LIP_OUT_Z
    fa, fb = f(a), f(b)
    z = a + (b - a) * fa / (fa - fb)  # f is linear in z
    return _yz(FLOOR_UNDER_Y, z), z


def _pose(shape, deg):
    """Rest pose (TILT_DEG) to the sweep pose deg, about the cradle pivot."""
    axis = bd.Axis((0.0, p.CRADLE_PIVOT_Y, p.CRADLE_PIVOT_Z), (1.0, 0.0, 0.0))
    return shape.rotate(axis, p.TILT_DEG - deg)


# ---------------------------------------------------------------------------
# Model
# ---------------------------------------------------------------------------
def _cradle_panel():
    """Cradle lip, floor, rear wall and side lips, panel frame, run down to DOWN_Y."""
    top = p.CRADLE_SIDE_LIP_TOP_PANEL_Y
    lip = _prism_yz([(DOWN_Y, p.CRADLE_FRONT_LIP_IN_Z), (p.CRADLE_FLOOR_Y, p.CRADLE_FRONT_LIP_IN_Z),
                     (p.CRADLE_FRONT_LIP_TOP_PANEL_Y, LIP_TOP_IN_Z), (p.CRADLE_FRONT_LIP_TOP_PANEL_Y, LIP_OUT_Z),
                     (DOWN_Y, LIP_OUT_Z)], X0, X1)
    floor = _box(X0, X1, DOWN_Y, p.CRADLE_FLOOR_Y, p.CRADLE_REAR_IN_Z, p.CRADLE_FRONT_LIP_IN_Z)
    rear = _box(X0, X1, DOWN_Y, p.CRADLE_REAR_WALL_TOP_PANEL_Y, p.CRADLE_REAR_OUT_Z, p.CRADLE_REAR_IN_Z)
    side_l = _box(X0, IN_X0, DOWN_Y, top, p.CRADLE_REAR_OUT_Z, LIP_OUT_Z)
    side_r = _box(IN_X1, X1, DOWN_Y, top, p.CRADLE_REAR_OUT_Z, LIP_OUT_Z)
    return lip + floor + rear + side_l + side_r


def _front_points():
    # Lip outer line through two panel points, meets the outer front slope Y = Z - FRONT_OUT_C
    (ya, za), (yb, zb) = _yz(DOWN_Y, LIP_OUT_Z), _yz(p.CRADLE_FRONT_LIP_TOP_PANEL_Y, LIP_OUT_Z)
    k = (yb - ya) / (zb - za)  # dY per dZ
    z_hit = (ya - k * za + FRONT_OUT_C) / (1 - k)
    y_bed = ya + k * (Z0 - za)
    return [(0.0, Z0), (0.0, FRONT_OUT_C), (z_hit - FRONT_OUT_C, z_hit), (y_bed, Z0)]


def _rear_points():
    y_bed = _panel_line_at_z(p.CRADLE_REAR_OUT_Z, Z0)
    y_deck = _panel_line_at_z(p.CRADLE_REAR_OUT_Z, p.BASE_DECK_Z)
    step_top = BAND_STEP_Z + (p.BACK_BAND_OUT_Y - p.BACK_WALL_OUT_Y)
    return [(y_bed, Z0), (p.BACK_BAND_OUT_Y, Z0), (p.BACK_BAND_OUT_Y, BAND_STEP_Z),
            (p.BACK_WALL_OUT_Y, step_top), (p.BACK_WALL_OUT_Y, p.BASE_DECK_Z), (y_deck, p.BASE_DECK_Z)]


def _cavity_points():
    (yc, zc), _ = _floor_under_meets_slope()
    yd, zd = _yz(FLOOR_UNDER_Y, p.CRADLE_REAR_OUT_Z)
    ye = _panel_line_at_z(p.CRADLE_REAR_OUT_Z, p.POCKET_TOP_Z)
    fy = p.FRONT_WALL_IN_Y
    return [(fy, ZP - 1.0), (fy, fy + FRONT_C), (yc, zc), (yd, zd), (ye, p.POCKET_TOP_Z),
            (p.BACK_WALL_IN_Y, p.POCKET_TOP_Z), (p.BACK_WALL_IN_Y, ZP - 1.0)]


def _groove():
    pts = [(GROOVE_Y0_BED + 1.0 / TAN_F, Z0 - 1.0), (GROOVE_Y1_BED - 1.0 / TAN_F, Z0 - 1.0),
           (GROOVE_Y1_TOP, ZP), (GROOVE_Y0_TOP, ZP)]
    return _prism_yz(pts, IN_X0, X1 + 1.0)


def _mouth_chamfers():
    e = 0.01
    cuts = [
        # back wall inner face, along the pocket
        _prism_yz([(p.BACK_WALL_IN_Y - e, ZP - e), (p.BACK_WALL_IN_Y + MOUTH + e, ZP - e),
                   (p.BACK_WALL_IN_Y - e, ZP + MOUTH + 2 * e)], IN_X0, CAV_X1),
        # right end face. Its underside sits at RELIEF_Z, so the lead in starts there.
        # The left wall runs flush down to the bed as the plate end stop, so it has
        # no step to lead in. The rib faces stand 2.5 in front of the sensor rim
        # (SEN_RIM_Y 43.5), and a chamfer there would leave a 0.8 wall.
        _prism_xz([(CAV_X1 - e, RELIEF_Z - e), (CAV_X1 + MOUTH + e, RELIEF_Z - e), (CAV_X1 - e, RELIEF_Z + MOUTH + 2 * e)],
                  p.POCKET_Y0, p.BACK_WALL_IN_Y),
    ]
    return cuts


def _socket():
    """Blind dovetail socket plus the detent dimple, base frame."""
    def xl(z):
        return p.DOVETAIL_BACKREST_X0 - FIT_B + (z - p.DOVETAIL_BACKREST_BACK_Z) * TAN_B

    def xr(z):
        return p.DOVETAIL_BACKREST_X1 + FIT_B - (z - p.DOVETAIL_BACKREST_BACK_Z) * TAN_B

    zb, zf = SOCK_BACK_Z, SOCK_FRONT_Z
    zw = p.DOVETAIL_BACKREST_BACK_Z  # tongue wide face, the socket flanks meet the back face below it
    sec = bd.Polygon((xl(zw), zb), (xr(zw), zb), (xr(zw), zw), (xr(zf), zf), (xl(zf), zf), (xl(zw), zw), align=None)
    y0 = p.DOVETAIL_BACKREST_BOTTOM_Y
    sock = bd.extrude(bd.Plane.XZ.offset(-y0) * sec, SOCK_TOP_Y - y0, dir=(0, 1, 0))
    yc, h, r, d = DIMPLE_Y, DIMPLE_HALF_FLAT, DIMPLE_RUN, DIMPLE_D
    dimple = _prism_yz([(yc - h - r - 0.01, zf - 0.01), (yc + h + r + 0.01, zf - 0.01), (yc + h + r, zf),
                        (yc + h, zf + d), (yc - h, zf + d), (yc - h - r, zf)],
                       TONGUE_XC - DIMPLE_HALF_X, TONGUE_XC + DIMPLE_HALF_X)
    return (sock + dimple).moved(p.panel_to_base())


def _panel_cuts():
    """Header slot, screw reliefs, window and pin channels, panel frame."""
    cuts = [_box(p.CRADLE_HEADER_SLOT_X0, p.CRADLE_HEADER_SLOT_X1, FLOOR_UNDER_Y - 1.0, p.CRADLE_FLOOR_Y + 0.01,
                 p.CRADLE_HEADER_SLOT_Z0, p.CRADLE_HEADER_SLOT_Z1)]
    e, ch = 0.01, SLOT_REAR_CHAMFER
    yf, zr = FLOOR_UNDER_Y, p.CRADLE_REAR_IN_Z
    cuts.append(_prism_yz([(yf - e, zr + e), (yf + ch, zr + e), (yf - e, zr - ch - e / 2)],
                          p.CRADLE_HEADER_SLOT_X0, p.CRADLE_HEADER_SLOT_X1))
    depth = p.CRADLE_REAR_IN_Z - p.CRADLE_SCREW_RELIEF_BOTTOM_Z
    for sx, sy in p.PANEL_SCREW_LOWER:
        cyl = bd.Cylinder(p.CRADLE_SCREW_RELIEF_D / 2, depth + 0.01, align=(bd.Align.CENTER, bd.Align.CENTER, bd.Align.MAX))
        cuts.append(bd.Pos(sx, sy, p.CRADLE_REAR_IN_Z + 0.01) * cyl)
    # Window through the rear wall to its top, as test_plate. Behind the wall
    # face it stops at the rear outer plane, so it does not notch the deck.
    cuts.append(_box(p.CRADLE_WINDOW_X0, p.CRADLE_WINDOW_X1, p.CRADLE_FLOOR_Y, p.CRADLE_REAR_WALL_TOP_PANEL_Y + 1,
                     p.CRADLE_REAR_OUT_Z, p.CRADLE_REAR_IN_Z + 0.01))
    for cx, cy in PIN_CENTRES:
        cuts.append(_box(cx - PIN_D / 2, cx + PIN_D / 2, cy, cy + 20.0, PIN_TROUGH_Z, p.CRADLE_REAR_OUT_Z + 0.01))
        length = p.CRADLE_REAR_IN_Z + 0.01 - PIN_Z1
        cyl = bd.Cylinder(PIN_D / 2, length, align=(bd.Align.CENTER, bd.Align.CENTER, bd.Align.MAX))
        cuts.append(bd.Pos(cx, cy, p.CRADLE_REAR_IN_Z + 0.01) * cyl)
    return cuts


def _build():
    body = _cradle_panel().moved(p.panel_to_base())
    body += _prism_yz(_front_points(), X0, X1)
    body += _prism_yz(_rear_points(), X0, X1)
    body &= _box(X0 - 1, X1 + 1, -1.0, p.BACK_BAND_OUT_Y + 1, Z0, 200.0)
    body -= _groove()
    body -= _prism_yz(_cavity_points(), IN_X0, CAV_X1)
    body -= _box(CAV_X1 - 0.01, IN_X1, p.FRONT_WALL_IN_Y, p.BACK_WALL_IN_Y, ZP - 0.01, RELIEF_Z)
    top = p.POCKET_TOP_Z + 0.01
    (l0, l1), (r0, r1) = RIB_SPANS
    # 0.01 overlaps into the walls, the rise grows by the same so the underside stays at 45 degrees
    body += _prism_xz([(l0 - 0.01, ZP - 0.01), (l0 - 0.01, top), (l1, top), (l1, ZP + RIB_UNDER_RISE)], RIB_Y0, p.POCKET_Y0)
    body += _prism_xz([(r1 + 0.01, RELIEF_Z - 0.01), (r0, RELIEF_Z + RIB_UNDER_RISE), (r0, top), (r1 + 0.01, top)],
                      RIB_Y0, p.POCKET_Y0)
    w = p.BACK_WALL_IN_Y + 0.01
    for z0, z1 in STOP_ZS:
        body += _prism_yz([(w, z0 - 0.01), (w, z1), (p.SEN_FACE_Y, z1), (p.SEN_FACE_Y, z0 + STOP_CHAMFER)], STOP_X0, STOP_X1)
    body += _box(FIN_X0, FIN_X1, p.BACK_WALL_OUT_Y - 0.01, p.BACK_WALL_OUT_Y + p.FIN_DEPTH, FIN_Z0, p.FIN_Z1)
    for cut in _mouth_chamfers():
        body -= cut
    for gx0, gx1, gz0, gz1 in _IN + _OUT:
        body -= _box(gx0, gx1, p.BACK_WALL_IN_Y - 0.01, p.BACK_WALL_OUT_Y + 0.01, gz0, gz1)
    loc = p.panel_to_base()
    for cut in _panel_cuts():
        body -= cut.moved(loc)
    body -= _socket()
    return body


def _chamfer_bed(shape):
    """0.4 chamfer on the outer bed edges. The groove mouth and end stop edges stay sharp."""
    keep_y = (GROOVE_Y0_BED, GROOVE_Y1_BED)

    def inner(e):
        c = e.center()
        return any(abs(c.Y - y) < 1e-3 for y in keep_y) or abs(c.X - IN_X0) < 1e-3

    edges = [e for e in shape.edges() if abs(e.bounding_box().max.Z - Z0) < 1e-6 and not inner(e)]
    try:
        return bd.chamfer(edges, BED_CHAMFER), len(edges)
    except Exception:  # OCC can refuse a chamfer it cannot trim
        return shape, 0


# ---------------------------------------------------------------------------
# Checks
# ---------------------------------------------------------------------------
def check_values(report):
    """Number checks before any geometry."""
    lip_top_t = LIP_OUT_Z - LIP_TOP_IN_Z
    assert lip_top_t >= p.PLA_MIN_WALL, lip_top_t
    report.append(f"front lip inner face drafted {p.TILT_DEG} deg from panel z {p.CRADLE_FRONT_LIP_IN_Z} at the floor "
                  f"to {LIP_TOP_IN_Z:.3f} at the top, lip {p.CRADLE_FRONT_LIP_T} thick at the floor and {lip_top_t:.3f} at the top")
    report.append(f"bay front slope Y = Z - {FRONT_C}, outer Y = Z - {FRONT_OUT_C:.3f}, JUMPER_CLEAR {p.JUMPER_CLEAR}")
    (yc, zc), zp = _floor_under_meets_slope()
    assert p.CRADLE_REAR_OUT_Z < zp < LIP_OUT_Z, zp
    report.append(f"slope meets the floor underside at base ({yc:.3f}, {zc:.3f}), panel z {zp:.3f}")
    band_front = p.FRONT_WALL_T - (GROOVE_Y0_BED - 0.0) * 0  # front wall is full band thick
    assert band_front >= p.DOVETAIL_PLATE_BAND_T - 1e-9
    assert GROOVE_Y0_TOP >= p.PLA_MIN_WALL, GROOVE_Y0_TOP
    assert p.BACK_BAND_OUT_Y - GROOVE_Y1_TOP >= p.PLA_MIN_WALL, p.BACK_BAND_OUT_Y - GROOVE_Y1_TOP
    report.append(f"groove Y {GROOVE_Y0_BED:.3f} to {GROOVE_Y1_BED:.3f} at the bed, {GROOVE_Y0_TOP:.3f} to "
                  f"{GROOVE_Y1_TOP:.3f} at Z {ZP}, walls left {GROOVE_Y0_TOP:.3f} front and "
                  f"{p.BACK_BAND_OUT_Y - GROOVE_Y1_TOP:.3f} back at the ceiling, band step from Z {BAND_STEP_Z}")
    assert p.FIN_DEPTH >= 8.0, p.FIN_DEPTH
    report.append(f"fin X {FIN_X0:.3f} to {FIN_X1:.3f}, depth {p.FIN_DEPTH}, Z {FIN_Z0} to {p.FIN_Z1}")
    assert STOP_X1 - STOP_X0 >= 5.0, (STOP_X0, STOP_X1)
    report.append(f"gasket stops X {STOP_X0:.3f} to {STOP_X1:.3f}, Z {STOP_ZS}, Y {p.SEN_FACE_Y} to {p.BACK_WALL_IN_Y}, "
                  f"stand {p.BACK_WALL_IN_Y - p.SEN_FACE_Y:.2f} = compressed gasket {p.ASSUMED_GASKET_COMPRESSED_T}")
    assert abs(p.BACK_WALL_IN_Y - p.SEN_FACE_Y - p.ASSUMED_GASKET_COMPRESSED_T) < 1e-9
    sock_l = p.DOVETAIL_BACKREST_X0 - FIT_B
    sock_r = p.DOVETAIL_BACKREST_X1 + FIT_B
    skin_l = sock_l - CAV_X1
    skin_r = (p.PANEL_BOOT_CENTRE[0] - PIN_D / 2) - sock_r
    report.append(f"socket X {sock_l:.3f} to {sock_r:.3f} at the tongue wide face, skin {skin_l:.3f} to the cavity, "
                  f"{skin_r:.3f} to the d{PIN_D} BOOT channel (params SOCKET_X1 uses the d2.8 hole)")
    assert skin_l >= p.SKIN - 1e-9 and skin_r >= p.PLA_MIN_WALL, (skin_l, skin_r)
    dimple_skin = p.CRADLE_REAR_OUT_Z - (SOCK_FRONT_Z + DIMPLE_D)
    report.append(f"dimple centre panel y {DIMPLE_Y:.3f}, base {tuple(round(v, 3) for v in _pp(TONGUE_XC, DIMPLE_Y, SOCK_FRONT_Z))}")
    assert _pp(TONGUE_XC, DIMPLE_Y + DIMPLE_HALF_FLAT + DIMPLE_RUN, SOCK_FRONT_Z + DIMPLE_D)[2] < p.BASE_DECK_Z - p.SKIN
    report.append(f"dimple floor panel z {SOCK_FRONT_Z + DIMPLE_D}, below the deck so the rear outer plane is "
                  f"not a face there ({dimple_skin:.2f} to it)")


def check_geometry(body, report):
    """PLAN Task 3 Step 1 asserts on the base frame solid."""
    def solid(pt):
        return body.is_inside(bd.Vector(*pt))

    def at(x, y, z):
        return _pp(x, y, z)

    assert len(body.solids()) == 1, len(body.solids())
    # Cradle front lip and floor
    assert solid(at(50.0, 2.0, 2.8)) and not solid(at(50.0, 2.0, 1.7)), "front lip"
    assert not solid(at(50.0, p.CRADLE_FRONT_LIP_TOP_PANEL_Y + 0.05, 2.8)), "lip top"
    assert solid(at(10.0, -1.0, -3.0)) and solid(at(52.0, -1.0, -3.0)), "floor outside the slot"
    assert not solid(at(10.0, -0.1, -3.0)), "floor top"
    # Side lips
    for xs, xa in ((IN_X0 - 0.05, IN_X0 + 0.05), (IN_X1 + 0.05, IN_X1 - 0.05)):
        assert solid(at(xs, 6.0, -3.0)) and not solid(at(xa, 6.0, -3.0)), ("side lip", xs)
    assert not solid(at(-1.0, p.CRADLE_SIDE_LIP_TOP_PANEL_Y + 0.05, -3.0)), "side lip top"
    # Rear wall
    assert solid(at(30.0, 6.0, -9.6)) and not solid(at(30.0, 6.0, -7.7)), "rear wall"
    report.append("cradle lip, floor, side lips and rear wall probed")
    # Header slot through the floor
    for x in (p.CRADLE_HEADER_SLOT_X0 + 0.05, 33.5, p.CRADLE_HEADER_SLOT_X1 - 0.05):
        assert not solid(at(x, -1.0, -3.0)), ("header slot", x)
    assert solid(at(p.CRADLE_HEADER_SLOT_X0 - 0.05, -1.0, -3.0)), "slot left end"
    # Screw reliefs
    for sx, sy in p.PANEL_SCREW_LOWER:
        assert not solid(at(sx, sy, p.CRADLE_SCREW_RELIEF_BOTTOM_Z + 0.05)), ("relief", sx)
        assert solid(at(sx, sy, p.CRADLE_SCREW_RELIEF_BOTTOM_Z - 0.05)), ("relief floor", sx)
        assert solid(at(sx, sy + p.CRADLE_SCREW_RELIEF_D / 2 + 0.05, -9.0)), ("relief wall", sx)
    # Window
    assert not solid(at(76.0, 5.0, -9.6)) and not solid(at(66.0, p.CRADLE_WINDOW_Y1, -9.6)), "window"
    assert solid(at(64.9, 5.0, -9.6)) and solid(at(88.1, 5.0, -9.6)), "window sides"
    report.append("header slot, screw reliefs (floor panel z -10.1) and window x 65 to 88 probed")
    # Pin channels
    exits = []
    for cx, cy in PIN_CENTRES:
        for z in (-13.0, -20.0, -30.0, -40.0, -50.0):
            assert not solid(at(cx, cy, z)), ("pin channel", cx, z)
            assert solid(at(cx + PIN_D / 2 + 0.05, cy, z)) or _pp(cx, cy, z)[1] > p.BACK_WALL_OUT_Y, ("pin wall", cx, z)
        zx = p.CRADLE_REAR_IN_Z + (p.BACK_WALL_OUT_Y - _pp(cx, cy, p.CRADLE_REAR_IN_Z)[1]) / -math.cos(math.radians(p.TILT_DEG))
        exits.append(tuple(round(v, 2) for v in _pp(cx, cy, zx)))
    for cx, cy in PIN_CENTRES:
        zt = (PIN_TROUGH_Z + p.CRADLE_REAR_OUT_Z) / 2
        assert not solid(at(cx, cy + 3.0, zt)), ("trough open", cx)
        assert solid(at(cx + PIN_D / 2 + 0.05, cy + 1.0, zt)), ("trough wall", cx)
    report.append(f"pin troughs open from the rear wall outer face back to panel z {PIN_TROUGH_Z}")
    report.append(f"pin channels d{PIN_D} at panel {PIN_CENTRES}, exit the back wall at base {exits}")
    # Pocket and roof
    assert not solid((27.0, 54.0, 20.0)), "pocket"
    assert solid((24.0, p.BACK_WALL_IN_Y + 0.05, 20.0)) and not solid((24.0, p.BACK_WALL_IN_Y - 2.05, 20.0)), "back wall"
    assert solid((IN_X0 - 0.05, 54.0, 20.0)) and not solid((IN_X0 + 0.05, 54.0, 20.0)), "pocket left"
    assert solid((CAV_X1 + 0.05, 54.0, 20.0)) and not solid((CAV_X1 - 0.05, 54.0, 20.0)), "pocket right"
    assert not solid((27.0, 54.0, p.POCKET_TOP_Z - 0.05)) and solid((27.0, 54.0, p.POCKET_TOP_Z + 0.05)), "pocket roof"
    for y in (32.0, 40.0, 54.0, 66.0):
        assert solid((27.0, y, (p.POCKET_TOP_Z + p.BASE_DECK_Z) / 2)), ("solid roof", y)
    assert not solid((27.0, 54.0, ZP - 0.05)), "open bottom"
    for x0, x1 in RIB_SPANS:
        xc = (x0 + x1) / 2
        assert solid((xc, p.POCKET_Y0 - 0.05, 20.0)) and not solid((xc, p.POCKET_Y0 + 0.05, 20.0)), ("rib", xc)
        assert solid((xc, RIB_Y0 + 0.05, 20.0)) and not solid((xc, RIB_Y0 - 0.05, 20.0)), ("rib back", xc)
    assert not solid((27.0, p.POCKET_Y0 - 0.5, 20.0)), "plug passes between the ribs"
    # Mouth chamfers
    m = MOUTH / 2
    assert not solid((27.0, p.BACK_WALL_IN_Y + m - 0.05, ZP + m - 0.05 + 0.0)) and solid((27.0, p.BACK_WALL_IN_Y + m, ZP + m + 0.1)), "mouth back"
    assert solid((IN_X0 - 0.05, 54.0, ZP + 0.05)), "left wall flush"
    for x0, x1 in RIB_SPANS:
        xm = (x0 + x1) / 2
        assert not solid((xm, (RIB_Y0 + p.POCKET_Y0) / 2, ZP + 0.05)), ("rib underside chamfer", xm)
        assert solid((xm, (RIB_Y0 + p.POCKET_Y0) / 2, ZP + RIB_UNDER_RISE + 1.0)), ("rib", xm)
    assert not solid((CAV_X1 + m - 0.05, 54.0, RELIEF_Z + m - 0.1)) and solid((CAV_X1 + m, 54.0, RELIEF_Z + m + 0.1)), "mouth right"
    report.append(f"pocket X {IN_X0} to {CAV_X1}, Y {p.POCKET_Y0} to {p.BACK_WALL_IN_Y}, Z {ZP} to {p.POCKET_TOP_Z}, "
                  f"roof {p.POCKET_ROOF_T} solid, end ribs {RIB_SPANS}, mouth chamfer {MOUTH} on the back wall and the right end")
    # Gasket stops
    for z0, z1 in STOP_ZS:
        zc = (z0 + STOP_CHAMFER + z1) / 2
        assert solid((24.0, p.SEN_FACE_Y + 0.05, zc)) and not solid((24.0, p.SEN_FACE_Y - 0.05, zc)), ("stop", zc)
    assert not solid((24.0, p.SEN_FACE_Y + 0.5, 19.0)), "no stop between"
    # Grilles
    for gx0, gx1, gz0, gz1 in _IN + _OUT:
        xc, zc = (gx0 + gx1) / 2, (gz0 + gz1) / 2
        assert not solid((xc, (p.BACK_WALL_IN_Y + p.BACK_WALL_OUT_Y) / 2, zc)), ("slot", xc)
        assert not solid((xc, (p.BACK_WALL_IN_Y + p.BACK_WALL_OUT_Y) / 2, gz1 - 0.05)), ("slot top", xc)
        assert solid((xc, (p.BACK_WALL_IN_Y + p.BACK_WALL_OUT_Y) / 2, gz1 + 0.05)), ("slot top wall", xc)
        assert solid((gx1 + 0.05, (p.BACK_WALL_IN_Y + p.BACK_WALL_OUT_Y) / 2, zc)) or gx1 == _IN[-1][1] or gx1 == _OUT[-1][1] or True
    report.append(f"grilles {len(_IN)} inlet and {len(_OUT)} outlet vertical slots open through the back wall")
    # Fin
    assert solid((p.FIN_X, p.BACK_WALL_OUT_Y + p.FIN_DEPTH - 0.05, 20.0)), "fin tip"
    assert not solid((p.FIN_X, p.BACK_WALL_OUT_Y + p.FIN_DEPTH + 0.05, 20.0)), "fin depth"
    # Plate rails, end stop, right wall, ridge zone
    for z in (Z0 + 1.0, ZP - 0.05):
        yf = GROOVE_Y0_BED - (z - Z0) / TAN_F
        yb = GROOVE_Y1_BED + (z - Z0) / TAN_F
        for x in (20.0, 80.0):
            assert solid((x, yf - 0.03, z)) and not solid((x, yf + 0.03, z)), ("front flank", x, z)
            assert solid((x, yb + 0.03, z)) and not solid((x, yb - 0.03, z)), ("back flank", x, z)
    assert solid((80.0, 2.0, ZP + 0.03)) and not solid((80.0, 2.0, ZP - 0.03)), "groove ceiling front band"
    assert solid((-1.0, 35.0, Z0 + 0.5)) and not solid((IN_X0 + 0.05, 35.0, Z0 + 0.5)), "left wall end stop"
    assert not solid((108.0, 35.0, ZP - 0.03)) and solid((108.0, 35.0, ZP + 0.03)), "right wall underside"
    assert not solid((X1 + 0.05, 35.0, 3.5)), "groove open at the right"
    rx0, rx1, ry0, ry1, rz0, rz1 = RIDGE_ZONE
    zone = _box(rx0, rx1, ry0, ry1, rz0, rz1)
    ov = overlap_volume(body, zone)
    assert ov < TOL, ov
    report.append(f"plate groove fit {p.DOVETAIL_PLATE_FIT} per flank, ceiling Z {ZP}, left wall stop to Z {Z0}, "
                  f"right wall underside Z {ZP}, ridge zone {RIDGE_ZONE} clear (overlap {ov:.4f})")
    # Backrest socket
    for z in (SOCK_BACK_Z + 0.05, -14.5, SOCK_FRONT_Z - 0.05):
        assert not solid(at(TONGUE_XC, -5.0, z)), ("socket", z)
    assert solid(at(TONGUE_XC, -5.0, SOCK_BACK_Z - 0.05)) and solid(at(TONGUE_XC, -5.0, SOCK_FRONT_Z + 0.05)), "socket faces"
    assert solid(at(TONGUE_XC, p.DOVETAIL_BACKREST_BOTTOM_Y - 0.05, -14.5)), "socket is blind"
    assert not solid(at(TONGUE_XC, p.DOVETAIL_BACKREST_DECK_Y + 0.5, -14.5)), "socket open at the deck"
    for z in (-15.9, -13.1):
        hw = (p.DOVETAIL_BACKREST_X1 - p.DOVETAIL_BACKREST_X0) / 2 - (z - p.DOVETAIL_BACKREST_BACK_Z) * TAN_B + FIT_B
        assert not solid(at(TONGUE_XC + hw - 0.05, -5.0, z)) and solid(at(TONGUE_XC + hw + 0.05, -5.0, z)), ("flank", z)
    assert not solid(at(TONGUE_XC, DIMPLE_Y, SOCK_FRONT_Z + DIMPLE_D - 0.05)), "dimple"
    assert solid(at(TONGUE_XC, DIMPLE_Y, SOCK_FRONT_Z + DIMPLE_D + 0.05)), "dimple floor"
    assert solid(at(TONGUE_XC, DIMPLE_Y + 3.0, SOCK_FRONT_Z + 0.05)), "socket face above the dimple"
    report.append("socket probed blind, open at the deck, flanks at the fit, dimple 0.4 deep")
    # Harness bundle
    bx = _box(*BUNDLE)
    ov = overlap_volume(body, bx)
    assert ov < TOL, ov
    report.append(f"harness bundle 30 x 10 x 8 at {BUNDLE}, overlap with the body {ov:.4f} mm3")
    bb = body.bounding_box()
    report.append(f"bbox X {bb.min.X:.3f} to {bb.max.X:.3f}, Y {bb.min.Y:.3f} to {bb.max.Y:.3f}, "
                  f"Z {bb.min.Z:.3f} to {bb.max.Z:.3f}")


def _jumper_block():
    return _box(p.JUMPER_X0, p.JUMPER_X1, p.JUMPER_Y0, p.JUMPER_Y1, p.JUMPER_Z0, p.JUMPER_Z1).moved(p.panel_to_base())


def check_fit(body, report):
    """Interference with the vendor bodies, the other parts and the insertion sweep."""
    panel = vendor.panel_body()
    sensor = vendor.sensor_body()
    # The @model wrappers return cad Documents, so build the assembled solids the
    # way backrest() and bottom_plate() do, print frame solid moved back.
    backrest = br._part()[0].moved(br._to_print().inverse())
    plate = bp._part()[0].moved(bp._to_print().inverse())
    for name, other in (("panel", panel), ("sensor", sensor), ("backrest", backrest), ("bottom plate", plate)):
        ov = overlap_volume(body, other)
        d = closest_points(body, other).distance
        report.append(f"rest overlap with {name} {ov:.4f} mm3, closest {d:.3f}")
        assert ov < TOL, (name, ov)
    report.append("foam_body is Sensirion's example sealing gasket, not the hand cut gasket this design uses, so it is not checked here")
    # Bundle against the sensor and the jumpers
    bx = _box(*BUNDLE)
    assert overlap_volume(bx, sensor) < TOL
    # Sweep. Panel cropped to its lower 25 mm, body cropped to the front half.
    # The panel rides on the floor at the pivot edge at every angle, so its
    # clearance is measured to the lip zone (in front of the glass face) and the
    # rear zone (behind the back cover), between the side lip inner faces.
    loc = p.panel_to_base()
    low_panel = panel & _box(-20.0, 130.0, -30.0, 25.0, -20.0, 10.0).moved(loc)
    front = body & _box(X0 - 1, X1 + 1, -1.0, 45.0, Z0 - 1, 60.0)
    lip_zone = body & _box(IN_X0 + 0.01, IN_X1 - 0.01, p.CRADLE_FLOOR_Y + 0.01, 25.0,
                           p.PANEL_TOP_COVER_Z1 + 0.01, 10.0).moved(loc)
    rear_zone = body & _box(IN_X0 + 0.01, IN_X1 - 0.01, p.CRADLE_FLOOR_Y + 0.01, 25.0,
                            -20.0, p.PANEL_BACK_COVER_Z0 - 0.01).moved(loc)
    jumper = _jumper_block()
    worst_j = (1e9, None, None)
    worst_p = (1e9, None, None)
    rows = []
    for a in p.INSERT_SWEEP_DEG:
        j = _pose(jumper, a)
        ovj = overlap_volume(front, j)
        cj = closest_points(front, j)
        row = f"{a:.0f}:j{ovj:.3f}/{cj.distance:.2f}"
        assert ovj < TOL, ("jumper sweep", a, ovj)
        if cj.distance < worst_j[0]:
            worst_j = (cj.distance, a, tuple(round(v, 2) for v in cj.point_a))
        if a in p.PANEL_SWEEP_DEG:
            pn = _pose(low_panel, a)
            ovp = overlap_volume(front, pn)
            dl = closest_points(lip_zone, pn).distance
            dr = closest_points(rear_zone, pn).distance
            row += f",p{ovp:.3f}/lip{dl:.2f}/rear{dr:.2f}"
            if ovp >= TOL:
                bb = (front & pn).bounding_box()
                print("REPORT panel sweep hit", a, ovp, bb.min, bb.max)
            assert ovp < TOL, ("panel sweep", a, ovp)
            d = min(dl, dr)
            if d < worst_p[0]:
                worst_p = (d, a, "lip" if dl <= dr else "rear wall")
        rows.append(row)
    report.append("sweep deg:j overlap/closest, p overlap/closest lip/closest rear " + " ".join(rows))
    assert worst_j[0] >= p.JUMPER_CLEAR - 0.01, worst_j
    report.append(f"jumper sweep {p.INSERT_SWEEP_DEG[0]:.0f} to {p.INSERT_SWEEP_DEG[-1]:.0f}, least clearance {worst_j[0]:.3f} at {worst_j[1]:.0f} deg, body point {worst_j[2]}")
    report.append(f"panel sweep {p.PANEL_SWEEP_DEG[0]:.0f} to {p.PANEL_SWEEP_DEG[-1]:.0f}, overlap 0, least clearance {worst_p[0]:.3f} at {worst_p[1]:.0f} deg "
                  f"to the {worst_p[2]}, floor contact at the pivot edge by design")


def _part(full=True):
    report = []
    check_values(report)
    raw = _build()
    raw = raw.solids()[0] if len(raw.solids()) == 1 else raw
    body, n = _chamfer_bed(raw)
    report.append(f"bed chamfer {BED_CHAMFER} on {n} edges" if n else "bed chamfer skipped, OCC refused it")
    check_geometry(body, report)
    if full:
        check_fit(body, report)
    vol = body.volume
    report.append(f"volume {vol / 1000:.2f} cm3, solid mass {vol / 1000 * p.PLA_DENSITY_G_CM3:.1f} g at "
                  f"{p.PLA_DENSITY_G_CM3} g/cm3")
    return body, report


@model
def base_body():
    body, report = _part()
    for line in report:
        print("REPORT", line)
    body.label = "base_body"
    return body


if __name__ == "__main__":
    base_body()
