# SPDX-License-Identifier: CC-BY-4.0. Copyright Walkthru.Earth.
"""Test plate for the CrowPanel 4.2 and SEN63C desk base, iteration 1.

Loose fit coupons printed flat on one A1 plate before the full parts
(DESIGN.md "4. Test plate", PLAN-iteration-1.md Task 2). Every number comes
from params.py. The few layout values that belong to this plate only are
named below with the reason for each.

Check: pixi run -e cad cad run test_plate.py, then
pixi run -e cad cad model dfm <id> --process printing --technology fdm --material pla
"""

import math
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))

import build123d as bd  # noqa: E402
from cad import model  # noqa: E402

import params as p  # noqa: E402

# ---------------------------------------------------------------------------
# Plate only values (builder calls, each derived from a sourced number)
# ---------------------------------------------------------------------------
PLATE_MAX = 180.0  # PLAN Task 2, the plate fits within 180 x 180 mm
LABEL_H_MIN = 5.0  # PLAN Task 2, text at least 5 mm tall
LABEL_EMBOSS = 0.4  # PLAN Task 2, text embossed 0.4 mm
LABEL_FONT = 10.0  # builder call, bold font size, dfm read strokes of 0.60 mm at size 7.2 and walls min 0.8 at this size (PLA_MIN_WALL 0.8)
LABEL_TEXT_H_MAX = 7.5  # builder call, caps and digits measure 7.16 to 7.41 mm at LABEL_FONT, asserted below
LABEL_MARGIN = 1.5  # builder call, land around a label
LABEL_TAB_T = 1.2  # builder call, 6 layers at 0.2, above the 2 layer skin minimum (CADP)
LABEL_TAB_D = LABEL_TEXT_H_MAX + 2 * LABEL_MARGIN  # 10.5, a tab that holds one line of text
DFM_FACE_BUDGET = 1000  # text-to-cad 0.11.13 manufacturing-printing.md, self-intersection budget for one check
GAP = 6.0  # builder call, space between loose pieces on the plate
SLICE_TIE = 3.0  # builder call, floor web past each header slot end that ties the lip to the rear wall
DOVE_FITS = (0.15, 0.20, 0.25, 0.30)  # PLAN Task 2 ladder, per side, horizontal, as DOVETAIL_PLATE_BAND_T adds it
DOVE_KEY_W = 10.0  # builder call, narrow face of the coupon key, wide face 13.46 holds a label
DOVE_SLIDE_L = 20.0  # builder call, engaged length of the slide
DOVE_STOP_L = 16.0  # builder call, closed end of the socket, holds the label
DOVE_HANDLE_L = 16.0  # builder call, key length left outside the socket, holds the label
DOVE_BED_CHAMFER = 0.4  # PLAN Task 3 Step 3 bed edge chamfer, takes the elephant foot (MAT 1.3, 0.10) off the key
LATCH_ROOT_L = 12.0  # builder call, root block along the arm, holds the label
LATCH_ROOT_W = 12.0  # builder call, root block across the arm
LATCH_BAR_W = 8.0  # builder call, shell edge stand-in bar width
LATCH_BAR_PLAY = p.FIT_LATCH_GAP  # MAT 6.1, 0.4 gap, play between the bar end and the hook return face
LATCH_BAR_CORNER = 1.0  # builder call, bar corner chamfer that clears the 0.9 root fillet
LATCH_HOOK_L = p.LATCH_CATCH / math.tan(math.radians(p.LATCH_RETURN_DEG)) + p.LATCH_CATCH / math.tan(
    math.radians(p.LATCH_LEAD_DEG)
)  # 2.732, return face run plus lead face run, no flat land
POCKET_RING_H = 5.0  # PLAN Task 2, pocket ring 5 mm tall
GRILLE_SAMPLE_X0 = p.BASE_X0  # builder call, the base outer left face
GRILLE_SAMPLE_X1 = p.FIN_X  # builder call, the fin centre between the grilles


# ---------------------------------------------------------------------------
# Small helpers
# ---------------------------------------------------------------------------
def _box(x0, x1, y0, y1, z0, z1):
    return bd.Pos((x0 + x1) / 2, (y0 + y1) / 2, (z0 + z1) / 2) * bd.Box(x1 - x0, y1 - y0, z1 - z0)


def _label(text, cx, cy, z, angle=0.0):
    """Bold text embossed LABEL_EMBOSS up from z, centred on (cx, cy)."""
    sk = bd.Text(text, font_size=LABEL_FONT, font_style=bd.FontStyle.BOLD)
    solid = bd.extrude(sk, LABEL_EMBOSS)
    return bd.Pos(cx, cy, z) * bd.Rot(0, 0, angle) * solid


def _label_w(text):
    return bd.Text(text, font_size=LABEL_FONT, font_style=bd.FontStyle.BOLD).bounding_box().size.X


def _probe(shape, pt):
    return shape.is_inside(bd.Vector(*pt))


# ---------------------------------------------------------------------------
# Cradle slices, panel frame, then laid with the floor on the bed
# ---------------------------------------------------------------------------
FLOOR_Y0 = p.CRADLE_FLOOR_Y - p.CRADLE_FLOOR_T  # -1.95, floor underside
LIP_OUT_Z = p.CRADLE_FRONT_LIP_IN_Z + p.CRADLE_FRONT_LIP_T  # 3.7
SLICES = {
    # name: (label, x0, x1), PLAN Task 2 ranges
    "slice_header": ("HDR", p.CRADLE_HEADER_SLOT_X0 - SLICE_TIE, p.CRADLE_HEADER_SLOT_X1 + SLICE_TIE),
    "slice_screw": ("SCR", p.BASE_X0, 15.0),
    "slice_buttons": ("BTN", 64.0, 89.0),
}


def _slice_to_bed(shape, x0):
    """Panel frame to the slice print frame. Rot +90 about X maps (x, y, z) to (x, -z, y).

    The floor underside goes to Z 0 and the label tab edge to Y 0.
    """
    return bd.Pos(-x0, LIP_OUT_Z, -FLOOR_Y0) * bd.Rot(90, 0, 0) * shape


def _slice_point(pt, x0):
    x, y, z = pt
    return (x - x0, -z + LIP_OUT_Z, y - FLOOR_Y0)


def cradle_slice(name):
    """One cradle cross section slice in the panel frame, with its label tab."""
    label, x0, x1 = SLICES[name]
    rear_out = p.CRADLE_REAR_OUT_Z
    body = _box(x0, x1, FLOOR_Y0, p.CRADLE_FLOOR_Y, rear_out, LIP_OUT_Z)  # floor
    body += _box(x0, x1, FLOOR_Y0, p.CRADLE_FRONT_LIP_TOP_PANEL_Y, p.CRADLE_FRONT_LIP_IN_Z, LIP_OUT_Z)  # front lip
    body += _box(x0, x1, FLOOR_Y0, p.CRADLE_REAR_WALL_TOP_PANEL_Y, rear_out, p.CRADLE_REAR_IN_Z)  # rear wall
    if x0 < p.CRADLE_SIDE_IN_X0:  # left side lip, base outer face to the side inner face
        body += _box(x0, p.CRADLE_SIDE_IN_X0, FLOOR_Y0, p.CRADLE_SIDE_LIP_TOP_PANEL_Y, rear_out, LIP_OUT_Z)
    tab_x1 = max(x1, x0 + _label_w(label) + 2 * LABEL_MARGIN)  # the tab may run past a narrow slice
    body += _box(x0, tab_x1, FLOOR_Y0, FLOOR_Y0 + LABEL_TAB_T, rear_out - LABEL_TAB_D, rear_out)  # label tab
    # Header slot, the floor opens across the channel (DES Header slot).
    if x0 < p.CRADLE_HEADER_SLOT_X1 and x1 > p.CRADLE_HEADER_SLOT_X0:
        body -= _box(
            p.CRADLE_HEADER_SLOT_X0, p.CRADLE_HEADER_SLOT_X1, FLOOR_Y0 - 1, p.CRADLE_FLOOR_Y + 0.01,
            p.CRADLE_HEADER_SLOT_Z0, p.CRADLE_HEADER_SLOT_Z1,
        )
    # Screw head relief in the rear wall inner face (DES Screw head relief).
    for sx, sy in p.PANEL_SCREW_LOWER:
        if x0 < sx < x1:
            depth = p.CRADLE_REAR_IN_Z - p.CRADLE_SCREW_RELIEF_BOTTOM_Z
            cyl = bd.Cylinder(p.CRADLE_SCREW_RELIEF_D / 2, depth + 0.01, align=(bd.Align.CENTER, bd.Align.CENTER, bd.Align.MAX))
            body -= bd.Pos(sx, sy, p.CRADLE_REAR_IN_Z + 0.01) * cyl
    # BOOT and RST window, cut through the rear wall to its top. A 2 mm band
    # over the 23 mm window would be a 23 mm bridge, over PLA_MAX_BRIDGE.
    if x0 < p.CRADLE_WINDOW_X1 and x1 > p.CRADLE_WINDOW_X0:
        body -= _box(
            p.CRADLE_WINDOW_X0, p.CRADLE_WINDOW_X1, p.CRADLE_FLOOR_Y, p.CRADLE_REAR_WALL_TOP_PANEL_Y + 1,
            rear_out - 0.01, p.CRADLE_REAR_IN_Z + 0.01,
        )
    laid = _slice_to_bed(body, x0)
    tab_cx = (tab_x1 - x0) / 2
    tab_cy = -(rear_out - LABEL_TAB_D / 2) + LIP_OUT_Z
    laid += _label(label, tab_cx, tab_cy, LABEL_TAB_T)
    return laid


def _check_slices(pieces):
    s = pieces["slice_header"]
    x0 = SLICES["slice_header"][1]
    slot_mid = (p.CRADLE_HEADER_SLOT_Z0 + p.CRADLE_HEADER_SLOT_Z1) / 2
    assert not _probe(s, _slice_point((33.5, -1.0, slot_mid), x0)), "header slot open through the floor"
    assert not _probe(s, _slice_point((p.CRADLE_HEADER_SLOT_X0 + 0.1, -1.0, slot_mid), x0)), "slot starts at x 19"
    assert not _probe(s, _slice_point((p.CRADLE_HEADER_SLOT_X1 - 0.1, -1.0, slot_mid), x0)), "slot ends at x 48"
    assert _probe(s, _slice_point((x0 + SLICE_TIE / 2, -1.0, slot_mid), x0)), "floor tie at the slot end"
    assert p.CRADLE_HEADER_SLOT_X0 <= p.PANEL_HEADER_X0 and p.CRADLE_HEADER_SLOT_X1 >= p.PANEL_HEADER_X1
    # Lip and wall heights against the panel rows.
    assert p.CRADLE_FRONT_LIP_TOP_PANEL_Y < p.PANEL_ACTIVE_Y0
    assert p.CRADLE_REAR_WALL_TOP_PANEL_Y <= p.PANEL_SIDE_LIP_MAX_Y and p.CRADLE_SIDE_LIP_TOP_PANEL_Y <= p.PANEL_SIDE_LIP_MAX_Y
    lip_z = (p.CRADLE_FRONT_LIP_IN_Z + LIP_OUT_Z) / 2
    assert _probe(s, _slice_point((30.0, p.CRADLE_FRONT_LIP_TOP_PANEL_Y - 0.05, lip_z), x0)), "lip reaches y 3.35"
    assert not _probe(s, _slice_point((30.0, p.CRADLE_FRONT_LIP_TOP_PANEL_Y + 0.05, lip_z), x0)), "lip stops at y 3.35"
    # Channel clears the shell depth at the sliding fit.
    chan = p.CRADLE_FRONT_LIP_IN_Z - p.CRADLE_REAR_IN_Z
    assert abs(chan - (p.PANEL_FRONT_Z - p.PANEL_BACK_COVER_Z0 + 2 * p.FIT_PANEL)) < 1e-9

    s = pieces["slice_screw"]
    x0 = SLICES["slice_screw"][1]
    sx, sy = p.PANEL_SCREW_LOWER[0]
    assert not _probe(s, _slice_point((sx, sy, -9.5), x0)), "relief open behind the screw head"
    assert not _probe(s, _slice_point((sx, sy + p.CRADLE_SCREW_RELIEF_D / 2 - 0.1, -9.0), x0)), "relief d 6"
    assert _probe(s, _slice_point((sx, sy + p.CRADLE_SCREW_RELIEF_D / 2 + 0.1, -9.0), x0)), "relief d 6"
    assert _probe(s, _slice_point((sx, sy, p.CRADLE_SCREW_RELIEF_BOTTOM_Z - 0.1), x0)), "skin behind the relief"
    skin = p.CRADLE_SCREW_RELIEF_BOTTOM_Z - p.CRADLE_REAR_OUT_Z
    assert skin >= p.SKIN - 1e-9, skin
    assert p.PANEL_BACK_COVER_Z0 - p.ASSUMED_SCREW_HEAD_H > p.CRADLE_SCREW_RELIEF_BOTTOM_Z, "head clears"
    assert p.CRADLE_SCREW_RELIEF_D > p.ASSUMED_SCREW_HEAD_D
    side_mid = (p.BASE_X0 + p.CRADLE_SIDE_IN_X0) / 2
    assert _probe(s, _slice_point((side_mid, p.CRADLE_SIDE_LIP_TOP_PANEL_Y - 0.05, -3.0), x0)), "side lip"
    assert not _probe(s, _slice_point((p.CRADLE_SIDE_IN_X0 + 0.05, 5.0, -3.0), x0)), "side fit clear"

    s = pieces["slice_buttons"]
    x0 = SLICES["slice_buttons"][1]
    rear_mid = (p.CRADLE_REAR_IN_Z + p.CRADLE_REAR_OUT_Z) / 2
    for bx, by in (p.PANEL_BOOT_CENTRE, p.PANEL_RST_CENTRE):
        assert not _probe(s, _slice_point((bx, by, p.PANEL_PLUNGER_Z0 - 0.1), x0)), "plunger clear"
        assert not _probe(s, _slice_point((bx, by, rear_mid), x0)), "pin path through the wall"
    assert not _probe(s, _slice_point((76.5, p.CRADLE_WINDOW_Y1, rear_mid), x0)), "window open to y 10"
    assert _probe(s, _slice_point((64.5, 5.0, rear_mid), x0)), "wall stub left of the window"
    assert _probe(s, _slice_point((88.5, 5.0, rear_mid), x0)), "wall stub right of the window"


# ---------------------------------------------------------------------------
# Dovetail ladder, DOVETAIL_PLATE profile
# ---------------------------------------------------------------------------
def _trapezoid_yz(half_bottom, half_top, z0, z1, x0, x1):
    """Prism along X with a symmetric trapezoid section in YZ."""
    pts = [(-half_bottom, z0), (half_bottom, z0), (half_top, z1), (-half_top, z1)]
    face = bd.Plane.YZ * bd.Polygon(*pts, align=None)
    return bd.Pos(x0, 0, 0) * bd.extrude(face, x1 - x0)


def dove_key(fit):
    """Male key, the bottom plate edge profile. Printed as the plate prints, narrow face on the bed."""
    half_n = DOVE_KEY_W / 2
    half_w = half_n + p.DOVETAIL_PLATE_RUN
    length = DOVE_SLIDE_L + DOVE_HANDLE_L
    key = _trapezoid_yz(half_n, half_w, 0.0, p.PLATE_T, 0.0, length)
    key = bd.chamfer(key.edges().group_by(bd.Axis.Z)[0], DOVE_BED_CHAMFER)
    key += _label(f"{round(fit * 100):02d}", DOVE_SLIDE_L + DOVE_HANDLE_L / 2, 0.0, p.PLATE_T)
    return key


def dove_socket(fit):
    """Female socket, the base rail band profile turned over so its flanks print as 60 degree overhangs.

    The key goes in wide face down. Flanks sit `fit` out from the key flanks,
    measured across, the way params.DOVETAIL_PLATE_BAND_T adds the fit.
    """
    half_out = DOVE_KEY_W / 2 + p.DOVETAIL_PLATE_BAND_T
    top = p.SKIN + p.PLATE_T
    block = _box(0.0, DOVE_SLIDE_L + DOVE_STOP_L, -half_out, half_out, 0.0, top)
    groove = _trapezoid_yz(
        DOVE_KEY_W / 2 + p.DOVETAIL_PLATE_RUN + fit, DOVE_KEY_W / 2 + fit, p.SKIN, top + 0.01, -0.01, DOVE_SLIDE_L
    )
    block -= groove
    block += _label(f"{round(fit * 100):02d}", DOVE_SLIDE_L + DOVE_STOP_L / 2, 0.0, top)
    return block


def _check_dovetails(pieces):
    assert abs(math.degrees(math.atan2(p.PLATE_T, p.DOVETAIL_PLATE_RUN)) - p.DOVETAIL_PLATE_FLANK_DEG) < 1e-9
    assert p.DOVETAIL_PLATE_FLANK_DEG >= p.PLA_OVERHANG_DEG
    for fit in DOVE_FITS:
        key, sock = pieces[f"dove_key_{fit:.2f}"], pieces[f"dove_socket_{fit:.2f}"]
        top = p.SKIN + p.PLATE_T
        mouth = DOVE_KEY_W / 2 + fit
        wide = DOVE_KEY_W / 2 + p.DOVETAIL_PLATE_RUN + fit
        assert not _probe(sock, (10.0, mouth - 0.02, top - 0.01)), f"mouth {fit}"
        assert _probe(sock, (10.0, mouth + 0.02, top - 0.01)), f"mouth {fit}"
        assert not _probe(sock, (10.0, wide - 0.02, p.SKIN + 0.01)), f"wide {fit}"
        assert _probe(sock, (10.0, wide + 0.02, p.SKIN + 0.01)), f"wide {fit}"
        assert _probe(sock, (DOVE_SLIDE_L + 0.5, 0.0, top - 0.5)), "socket closed at the stop"
        side = DOVE_KEY_W / 2 + p.DOVETAIL_PLATE_BAND_T - wide
        assert side >= p.PLA_MIN_WALL, side
        zk = 1.0  # above the bed chamfer
        half_k = DOVE_KEY_W / 2 + zk / math.tan(math.radians(p.DOVETAIL_PLATE_FLANK_DEG))
        assert _probe(key, (10.0, half_k - 0.02, zk)) and not _probe(key, (10.0, half_k + 0.02, zk)), f"key {fit}"
        half_top = DOVE_KEY_W / 2 + p.DOVETAIL_PLATE_RUN
        assert _probe(key, (10.0, half_top - 0.03, p.PLATE_T - 0.01)), f"key top {fit}"
        assert not _probe(key, (10.0, half_top + 0.03, p.PLATE_T - 0.01)), f"key top {fit}"


# ---------------------------------------------------------------------------
# Latch arms, LATCH numbers, printed flat, flex in XY
# ---------------------------------------------------------------------------
def latch_arm(length):
    """Root block, tapered arm along +X and the hook on the -Y side at the tip.

    Flat face at Y 0 (tension side when the hook is pushed to +Y), taper on the
    +Y face from LATCH_H_ROOT to LATCH_H_TIP, LATCH_B tall in Z.
    """
    c = p.LATCH_CATCH
    ret = c / math.tan(math.radians(p.LATCH_RETURN_DEG))
    pts = [
        (-LATCH_ROOT_L, -LATCH_ROOT_W + p.LATCH_H_ROOT + 2.0),
        (0.0, -LATCH_ROOT_W + p.LATCH_H_ROOT + 2.0),
        (0.0, 0.0),
        (length, 0.0),
        (length + ret, -c),
        (length + LATCH_HOOK_L, 0.0),
        (length + LATCH_HOOK_L, p.LATCH_H_TIP),
        (length, p.LATCH_H_TIP),
        (0.0, p.LATCH_H_ROOT),
        (0.0, p.LATCH_H_ROOT + 2.0),
        (-LATCH_ROOT_L, p.LATCH_H_ROOT + 2.0),
    ]
    face = bd.Polygon(*pts, align=None)
    roots = [v for v in face.vertices() if abs(v.X) < 1e-6 and (abs(v.Y) < 1e-6 or abs(v.Y - p.LATCH_H_ROOT) < 1e-6)]
    face = bd.fillet(roots, p.LATCH_ROOT_FILLET)
    arm = bd.extrude(face, p.LATCH_B)
    arm += _label(f"{round(length):d}", -LATCH_ROOT_L / 2, (-LATCH_ROOT_W + 2 * p.LATCH_H_ROOT + 4.0) / 2, p.LATCH_B, 90)
    return arm


def latch_bar(length):
    """Shell edge stand-in. Its end face is the shell top edge the hook catches.

    Unlabelled to keep the plate under DFM_FACE_BUDGET. The three bars are
    14.6, 17.6 and 19.6 mm long, so each one names its arm by length.
    """
    bar_l = length - LATCH_BAR_PLAY
    bar = _box(0.0, bar_l, -LATCH_BAR_W, 0.0, 0.0, p.LATCH_B)
    corner = [e for e in bar.edges().filter_by(bd.Axis.Z) if abs(e.center().X) < 1e-6 and abs(e.center().Y) < 1e-6]
    return bd.chamfer(corner, LATCH_BAR_CORNER)


def _check_latches(pieces, report):
    for length in p.LATCH_L_OPTIONS:
        eps_t = p.latch_strain_tapered(length)
        eps_c = p.latch_strain_constant(length)
        report.append(f"latch l {length:g} mm, tapered {eps_t:.3f} %, constant section {eps_c:.3f} %")
        assert eps_t <= p.PLA_STRAIN_LIMIT_XY, (length, eps_t)
        arm = pieces[f"latch_arm_{length:g}"]
        zc = p.LATCH_B / 2
        for x in (2.0, length / 2, length - 0.05):
            h = p.LATCH_H_ROOT - (p.LATCH_H_ROOT - p.LATCH_H_TIP) * x / length
            assert _probe(arm, (x, h - 0.02, zc)) and not _probe(arm, (x, h + 0.02, zc)), (length, x, h)
            assert _probe(arm, (x, 0.02, zc)) and not _probe(arm, (x, -0.02, zc)), (length, x)
        hx = length + p.LATCH_CATCH  # hook bottom vertex
        assert _probe(arm, (hx, -p.LATCH_CATCH + 0.03, zc)) and not _probe(arm, (hx, -p.LATCH_CATCH - 0.03, zc))
        assert not _probe(arm, (length - 0.02, -0.02, zc)), "return face starts at the arm tip"
        bb = arm.bounding_box()
        assert abs(bb.min.Z) < 1e-6 and abs(bb.max.Z - (p.LATCH_B + LABEL_EMBOSS)) < 1e-6
        bar = pieces[f"latch_bar_{length:g}"]
        bbb = bar.bounding_box()
        assert abs(bbb.size.X - (length - LATCH_BAR_PLAY)) < 1e-6
        assert p.LATCH_CATCH <= 1.2  # PLAN Task 4, overlap at most 1.2


# ---------------------------------------------------------------------------
# Pocket ring, base frame X and Y, 5 mm of the pocket from its open bottom
# ---------------------------------------------------------------------------
def pocket_ring():
    """Only one on the plate and unlabelled, to keep the plate under DFM_FACE_BUDGET."""
    x0, x1, y0, y1 = p.POCKET_X0, p.POCKET_X1, p.POCKET_Y0, p.POCKET_Y1
    w, rib_t, rib_w = p.WALL, p.POCKET_END_RIB_T, p.POCKET_END_RIB_W
    h = POCKET_RING_H
    ring = _box(x0 - w, x0, y0 - rib_t, y1 + p.BACK_WALL_T, 0, h)  # left wall
    ring += _box(x1, x1 + w, y0 - rib_t, y1 + p.BACK_WALL_T, 0, h)  # right wall
    ring += _box(x0, x1, y1, y1 + p.BACK_WALL_T, 0, h)  # back wall, inner face is the gasket land
    ring += _box(x0, x0 + rib_w, y0 - rib_t, y0, 0, h)  # end pad rib, inlet end
    ring += _box(x1 - rib_w, x1, y0 - rib_t, y0, 0, h)  # end pad rib, outlet end
    eps = 1e-3
    mouth = [
        e
        for e in ring.edges()
        if abs(e.center().Z) < eps
        and x0 - eps <= e.center().X <= x1 + eps
        and y0 - eps <= e.center().Y <= y1 + eps
        and (abs(e.center().X - x0) < eps or abs(e.center().X - x1) < eps or abs(e.center().Y - y0) < eps or abs(e.center().Y - y1) < eps)
    ]
    return bd.chamfer(mouth, p.POCKET_MOUTH_CHAMFER)


def _check_pocket(ring):
    x0, x1, y0, y1 = p.POCKET_X0, p.POCKET_X1, p.POCKET_Y0, p.POCKET_Y1
    assert x1 - x0 >= p.SEN_MAX_L + 2 * p.SEN_SIDE_CLEAR - 1e-9
    assert abs((y1 - y0) - (p.ASSUMED_GASKET_COMPRESSED_T + p.SEN_MAX_H + p.ASSUMED_END_PAD_COMPRESSED_T)) < 1e-9
    zc, ym, xm = 3.0, (y0 + y1) / 2, (x0 + x1) / 2
    assert not _probe(ring, (x0 + 0.05, ym, zc)) and _probe(ring, (x0 - 0.05, ym, zc)), "left wall"
    assert not _probe(ring, (x1 - 0.05, ym, zc)) and _probe(ring, (x1 + 0.05, ym, zc)), "right wall"
    assert not _probe(ring, (xm, y1 - 0.05, zc)) and _probe(ring, (xm, y1 + 0.05, zc)), "gasket land"
    rib_c = x0 + p.POCKET_END_RIB_W / 2
    assert _probe(ring, (rib_c, y0 - 0.05, zc)) and not _probe(ring, (rib_c, y0 + 0.05, zc)), "end rib face"
    assert not _probe(ring, (xm, y0 - p.POCKET_END_RIB_T / 2, zc)), "open between the ribs"
    c = p.POCKET_MOUTH_CHAMFER
    assert not _probe(ring, (x0 - c / 2 + 0.05, ym, 0.05)), "mouth chamfer at the open bottom"
    assert _probe(ring, (x0 - 0.05, ym, c + 0.05)), "full wall above the chamfer"
    bb = ring.bounding_box()
    assert abs(bb.min.Z) < 1e-6 and abs(bb.max.Z - POCKET_RING_H) < 1e-6


# ---------------------------------------------------------------------------
# Grille sample, back wall with the inlet slots, standing as the base prints
# ---------------------------------------------------------------------------
def grille_sample():
    """Base X, wall thickness along Y (pocket side at Y 0), base Z minus PLATE_TOP_Z.

    Only one on the plate and unlabelled, to keep the plate under DFM_FACE_BUDGET.
    """
    x0, x1 = GRILLE_SAMPLE_X0, GRILLE_SAMPLE_X1
    t = p.BACK_WALL_T
    wall = _box(x0, x1, 0.0, t, 0.0, p.POCKET_H)
    wall += _box(x0, x1, t, t + LABEL_TAB_D, 0.0, LABEL_TAB_T)  # foot outside the wall, steadies the 27.5 mm wall
    for sx0, sx1, sz0, sz1 in p.grille_slots_base("inlet"):
        wall -= _box(sx0, sx1, -0.01, t + 0.01, sz0 - p.PLATE_TOP_Z, sz1 - p.PLATE_TOP_Z)
    return wall


def _check_grille(g, report):
    slots = p.grille_slots_base("inlet")
    area = sum((sx1 - sx0) * (sz1 - sz0) for sx0, sx1, sz0, sz1 in slots)
    report.append(f"inlet grille open area {area:.1f} mm2, need {2 * p.SEN_MIN_INLET_AREA:.1f}")
    assert area >= 2 * p.SEN_MIN_INLET_AREA
    assert p.GRILLE_SLOT_W <= p.PLA_MAX_BRIDGE and p.GRILLE_BAR_W >= p.PLA_MIN_WALL
    yc = p.BACK_WALL_T / 2
    for sx0, sx1, sz0, sz1 in slots:
        zc = (sz0 + sz1) / 2 - p.PLATE_TOP_Z
        assert not _probe(g, ((sx0 + sx1) / 2, yc, zc)), "slot open"
        assert _probe(g, (sx0 - p.GRILLE_BAR_W / 2, yc, zc)), "bar or border"
        assert _probe(g, ((sx0 + sx1) / 2, yc, sz1 - p.PLATE_TOP_Z + 0.1)), "bridge over the slot"
    # Gasket land between the outer slots and the sensor edge.
    assert slots[0][0] - p.SEN_X0 >= p.GRILLE_GASKET_BORDER - 1e-9


# ---------------------------------------------------------------------------
# Plate
# ---------------------------------------------------------------------------
def _pieces():
    pieces = {name: cradle_slice(name) for name in SLICES}
    for fit in DOVE_FITS:
        pieces[f"dove_key_{fit:.2f}"] = dove_key(fit)
        pieces[f"dove_socket_{fit:.2f}"] = dove_socket(fit)
    for length in p.LATCH_L_OPTIONS:
        pieces[f"latch_arm_{length:g}"] = latch_arm(length)
        pieces[f"latch_bar_{length:g}"] = latch_bar(length)
    pieces["pocket_ring"] = pocket_ring()
    pieces["grille_inlet"] = grille_sample()
    return pieces


def _layout(pieces):
    """Place each piece by its bounding box min corner. Returns name to placed shape."""
    placed = {}
    spots = {}
    # Row 1, cradle slices then the pocket ring.
    x = 0.0
    for name in SLICES:
        spots[name] = (x, 0.0)
        x += pieces[name].bounding_box().size.X + GAP
    spots["pocket_ring"] = (x, 0.0)
    row1_d = max(pieces[n].bounding_box().size.Y for n in list(SLICES) + ["pocket_ring"])
    # Row 2, a column of sockets, a column of keys in the same fit order,
    # then the latch arms each with its bar, then the grille.
    y2 = row1_d + GAP
    x = 0.0
    row = max(pieces[f"dove_socket_{f:.2f}"].bounding_box().size.Y for f in DOVE_FITS) + GAP
    for kind in ("dove_socket", "dove_key"):
        for i, fit in enumerate(DOVE_FITS):  # each key beside its socket
            spots[f"{kind}_{fit:.2f}"] = (x, y2 + i * row)
        x += max(pieces[f"{kind}_{f:.2f}"].bounding_box().size.X for f in DOVE_FITS) + GAP
    y = y2
    for length in p.LATCH_L_OPTIONS:
        arm, bar = pieces[f"latch_arm_{length:g}"], pieces[f"latch_bar_{length:g}"]
        spots[f"latch_arm_{length:g}"] = (x, y)
        y += arm.bounding_box().size.Y + GAP / 2
        spots[f"latch_bar_{length:g}"] = (x, y)
        y += bar.bounding_box().size.Y + GAP
    x += max(pieces[f"latch_arm_{n:g}"].bounding_box().size.X for n in p.LATCH_L_OPTIONS) + GAP
    spots["grille_inlet"] = (x, y2)
    for name, shape in pieces.items():
        bb = shape.bounding_box()
        sx, sy = spots[name]
        placed[name] = bd.Pos(sx - bb.min.X, sy - bb.min.Y, -bb.min.Z) * shape
    return placed


@model
def test_plate():
    report = []
    pieces = _pieces()
    _check_slices(pieces)
    _check_dovetails(pieces)
    _check_latches(pieces, report)
    _check_pocket(pieces["pocket_ring"])
    _check_grille(pieces["grille_inlet"], report)

    # Labels, measured on each label solid.
    for text in ("HDR", "SCR", "BTN", "15", "20", "25", "30", "18"):
        sk = bd.Text(text, font_size=LABEL_FONT, font_style=bd.FontStyle.BOLD)
        hb = sk.bounding_box().size.Y
        report.append(f"label {text} height {hb:.2f} mm")
        assert LABEL_H_MIN <= hb <= LABEL_TEXT_H_MAX, (text, hb)
        assert abs(bd.extrude(sk, LABEL_EMBOSS).bounding_box().size.Z - LABEL_EMBOSS) < 1e-6

    placed = _layout(pieces)
    for name, shape in placed.items():
        assert len(shape.solids()) == 1, (name, len(shape.solids()))
        shape.label = name
    faces = sum(len(shape.faces()) for shape in placed.values())
    report.append(f"plate faces {faces}, dfm budget {DFM_FACE_BUDGET}")
    assert faces <= DFM_FACE_BUDGET, faces
    boxes = {name: shape.bounding_box() for name, shape in placed.items()}
    names = list(boxes)
    for i, a in enumerate(names):
        for b in names[i + 1 :]:
            ba, bb_ = boxes[a], boxes[b]
            apart = (
                ba.max.X + GAP - 1e-6 <= bb_.min.X or bb_.max.X + GAP - 1e-6 <= ba.min.X
                or ba.max.Y + GAP / 2 - 1e-6 <= bb_.min.Y or bb_.max.Y + GAP / 2 - 1e-6 <= ba.min.Y
            )
            assert apart, (a, b)
    plate = bd.Compound(children=list(placed.values()), label="test_plate")
    bb = plate.bounding_box()
    report.append(f"plate {bb.size.X:.1f} x {bb.size.Y:.1f} x {bb.size.Z:.1f} mm")
    assert bb.size.X <= PLATE_MAX and bb.size.Y <= PLATE_MAX, (bb.size.X, bb.size.Y)
    assert abs(bb.min.Z) < 1e-6
    for name, shape in placed.items():
        sb = shape.bounding_box()
        report.append(
            f"piece {name} at X {sb.min.X:.1f} Y {sb.min.Y:.1f}, size {sb.size.X:.1f} x {sb.size.Y:.1f} x {sb.size.Z:.1f}"
        )
    for line in report:
        print("REPORT", line)
    return plate


if __name__ == "__main__":
    test_plate()
