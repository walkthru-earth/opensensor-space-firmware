# SPDX-License-Identifier: CC-BY-4.0. Copyright Walkthru.Earth.
"""Shell, the one piece case of the Waveshare ePaper-3.97 and SEN63C desk stand.

DESIGN.md Shell. A box open at the front plane (board bz 3.0, tilted back
TILT_DEG), with the board seat brackets, the sensor bay on the back wall,
the sensor bar posts, the battery shelf ribs and the coin slot, the bezel tab
slots, the latch window, the port and rocker notches and the vents.

Frames. shell() is in the case frame. shell_print() lies on its back wall
(print Z = Y_BACK_OUT - Y), so the walls rise straight up, the sensor bay
grows off the bed and every hole in a side, top or floor wall bridges at most
5 mm along X.

Check: pixi run -e cad cad run shell.py, then
pixi run -e cad cad model dfm shell.py:shell_print --process printing --technology fdm --material pla
"""

from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))

import build123d as bd  # noqa: E402
from cad import model  # noqa: E402

import params as p  # noqa: E402
from shapes import bezel_tab, board_box, board_prism_yz, box, coin_body, pack_body, prism_yz  # noqa: E402

TOP_FILLET = 3.0  # call, outer top side edges, leaves 1.59 of wall at the corner
BOTTOM_CHAMFER = 1.0  # call, outer bottom side edges, feet stay on the flat
BACK_CHAMFER = 1.0  # call, back wall bed edges, also the elephant foot chamfer
GUSSET_REACH = 1.0  # call, plate and gusset start 1.0 inside the side wall


def _outer():
    pts = [(p.y_front(0), 0.0), (p.Y_BACK_OUT, 0.0), (p.Y_BACK_OUT, p.Z_TO), (p.y_front(p.Z_TO), p.Z_TO)]
    s = prism_yz(pts, p.OUT_X0, p.OUT_X1)

    def along_y(e, z):
        a, b = e.position_at(0), e.position_at(1)
        return abs(a.Z - z) < 1e-6 and abs(b.Z - z) < 1e-6 and abs(a.X - b.X) < 1e-6 and abs(a.Y - b.Y) > 1

    s = bd.fillet([e for e in s.edges() if along_y(e, p.Z_TO)], TOP_FILLET)
    s = bd.chamfer([e for e in s.edges() if along_y(e, 0.0)], BOTTOM_CHAMFER)
    back = [e for e in s.edges() if abs(e.bounding_box().min.Y - p.Y_BACK_OUT) < 1e-6
            and abs(e.bounding_box().max.Y - p.Y_BACK_OUT) < 1e-6]
    return bd.chamfer(back, BACK_CHAMFER)


def _brackets():
    """Four plates with stud cups, each with a 45 degree gusset behind it."""
    out = []
    for sx, sy in p.STUDS:
        left = sx < p.PCB_W / 2
        wall = p.IN_X0 - GUSSET_REACH if left else p.IN_X1 + GUSSET_REACH
        tip = sx + p.CUP_OD / 2 if left else sx - p.CUP_OD / 2
        x0, x1 = (wall, tip) if left else (tip, wall)
        y0, y1 = sy - p.CUP_OD / 2, sy + p.CUP_OD / 2
        out.append(board_box(x0, x1, y0, y1, p.BRACKET_BACK_BZ, p.CUP_FLOOR_BZ))
        ring = (bd.Cylinder(p.CUP_OD / 2, p.CUP_DEPTH + p.CUP_GAP, align=(bd.Align.CENTER, bd.Align.CENTER, bd.Align.MIN))
                - bd.Cylinder(p.CUP_ID / 2, 10, align=(bd.Align.CENTER, bd.Align.CENTER, bd.Align.MIN)))
        out.append(p.board_to_case() * bd.Pos(sx, sy, p.CUP_FLOOR_BZ) * ring)
        reach = abs(tip - wall)
        # Gusset in the (bx, bz) plane behind the plate, prism along by.
        tri = [(wall, p.BRACKET_BACK_BZ), (tip, p.BRACKET_BACK_BZ), (wall, p.BRACKET_BACK_BZ - reach)]
        face = bd.Plane.XZ.offset(-y0) * bd.Polygon(*tri, align=None)
        gusset = p.board_to_case() * bd.extrude(face, y1 - y0, dir=(0, 1, 0))
        if sy < p.PCB_H / 2:
            gusset = gusset - _bar_trim()
        out.append(gusset)
    return out


def _sensor_bay():
    x0, x1 = p.POCKET_X
    y_in = p.Y_BACK_IN
    ribs = [
        box(x0 - p.RIB_T, x0, y_in - p.RIB_DEPTH, y_in + 0.5, p.FLOOR_TOP - 0.5, p.POCKET_Z1 + p.RIB_T),
        box(x1, x1 + p.RIB_T, y_in - p.RIB_DEPTH, y_in + 0.5, p.FLOOR_TOP - 0.5, p.POCKET_Z1 + p.RIB_T),
        box(x0 - p.RIB_T, x1 + p.RIB_T, y_in - p.RIB_DEPTH, y_in + 0.5, p.POCKET_Z1, p.POCKET_Z1 + p.RIB_T),
    ]
    # Each stand-off runs on into its corner rib and the floor or top rib, so no
    # thin gap is left between them (a 0.5 to 0.7 slot prints as a burr).
    stands = []
    for a, b, c, d in p.STANDOFFS:
        sx0, sx1 = (x0, p.SEN_X0 + b) if a < p.SEN_L / 2 else (p.SEN_X0 + a, x1)
        sz0, sz1 = (p.FLOOR_TOP - 0.5, p.SEN_Z0 + d) if c < p.SEN_W / 2 else (p.SEN_Z0 + c, p.POCKET_Z1 + 0.01)
        stands.append(box(sx0, sx1, p.SEN_FACE_Y, y_in + 0.5, sz0, sz1))
    posts = [box(a, b, p.POST_FRONT_Y, y_in + 0.5, p.POST_Z[0] - 0.5, p.POST_Z[1]) for a, b in p.POST_XS]
    return ribs + stands + posts


def _battery_shelf():
    """Ribs off the back wall that carry the pack behind the board, and the coin slot.

    DESIGN.md Battery shelf. Each rib is a YZ prism with a horizontal bottom and
    top, its front on the plane bz RIB_BZ where the pack back rests. Feet under the
    pack bottom edge and the two fences are board frame ledges on their rib. All
    of it is X slabs, so in print (back wall down) every face is a wall or looks
    down at most 15 degrees. The coin slot is a box on the bay right rib with a
    pocket open at the top and a front stop that bridges the 4.0 slot.
    """
    z0, z1 = p.RIB_Z
    back = p.Y_BACK_IN + 0.5
    bz_in = p.RIB_BZ - p.LEDGE_INTO_RIB

    def rib(x0, x1):
        return prism_yz([(p.y_at(z0, p.RIB_BZ), z0), (p.y_at(z1, p.RIB_BZ), z1), (back, z1), (back, z0)], x0, x1)

    def ledge(x0, x1, by0, by1, bz1):
        return board_prism_yz([(by0, bz_in), (by0, bz1), (by1, bz1), (by1, bz_in)], x0, x1)

    out = []
    for xc in p.SHELF_RIB_XC:
        x0, x1 = xc - p.RIB_T / 2, xc + p.RIB_T / 2
        out += [rib(x0, x1), ledge(x0, x1, p.BY_SEAT - p.FOOT_T, p.BY_SEAT, p.FOOT_FRONT_BZ)]
    out += [rib(*p.LEFT_FENCE_X), ledge(*p.LEFT_FENCE_X, p.LEFT_FENCE_BY0, p.BY_TOP, p.FENCE_FRONT_BZ)]
    out += [rib(*p.RIGHT_FENCE_X), ledge(*p.RIGHT_FENCE_X, p.BY_SEAT - p.FOOT_T, p.BY_TOP, p.FENCE_FRONT_BZ)]
    cx0, cx1 = p.COIN_SLOT_X
    f0, f1 = p.COIN_FLOOR_Z
    blk = box(p.COIN_BLOCK_X0, p.COIN_RIB_X[1], p.COIN_STOP_Y[0], back, f0, p.COIN_BLOCK_Z1)
    blk = blk - box(cx0, cx1, p.COIN_STOP_Y[1], p.Y_BACK_IN, f1, p.COIN_BLOCK_Z1 + 1)
    blk = blk - box(cx0, cx1, p.COIN_STOP_Y[0] - 1, p.COIN_STOP_Y[1] + 0.01, f1 + p.COIN_STOP_H, p.COIN_BLOCK_Z1 + 1)
    # The block starts on the bay rib at 74.3, left of the pack slot end, so the
    # pack corner space is cut out of it.
    blk = blk - board_box(p.COIN_BLOCK_X0 - 1, p.SH_X1, p.BY_SEAT, 200, p.RIB_BZ, 20)
    out.append(blk)
    return out


def _grilles():
    g = p.GRILLE_GROW
    sq = p.SEN_INLET_SQ
    y0, y1 = p.Y_BACK_IN - 1, p.Y_BACK_OUT + 1
    cuts = [box(p.SEN_X0 + sq[0] - g, p.SEN_X0 + sq[1] + g, y0, y1, p.SEN_Z0 + sq[2] - g, p.SEN_Z0 + sq[3] + g)]
    for x, y, d in (p.SEN_INLET_ROUND, p.SEN_OUTLET):
        cyl = bd.Cylinder(d / 2 + g, y1 - y0, align=(bd.Align.CENTER, bd.Align.CENTER, bd.Align.MIN))
        cuts.append(bd.Pos(p.SEN_X0 + x, y0, p.SEN_Z0 + y) * bd.Rot(-90, 0, 0) * cyl)
    return cuts


BAR_GUSSET_CLEAR = 0.8  # call, lower gussets stay this far off the bar channel


def _bar_trim():
    """Half space Z < Y - k that holds the whole bar channel, for the lower gussets.

    The plane runs through (BAR_FRONT_Y - BAR_GUSSET_CLEAR, BAR_Z1 + HOLE_CLEAR +
    BAR_GUSSET_CLEAR) at 45 degrees in Y and Z, so the trimmed gusset face looks
    down at 45 degrees in print (back wall down) and the bar meets only air.
    """
    k = (p.BAR_FRONT_Y - BAR_GUSSET_CLEAR) - (p.BAR_Z1 + p.HOLE_CLEAR + BAR_GUSSET_CLEAR)
    return prism_yz([(-20, -20 - k), (80, 80 - k), (80, -40), (-20, -40)], p.OUT_X0 - 1, p.OUT_X1 + 1)


def _bar_holes():
    hz0, hz1 = p.BAR_Z0 - p.HOLE_CLEAR, p.BAR_Z1 + p.HOLE_CLEAR
    hy0, hy1 = p.BAR_FRONT_Y, p.BAR_FRONT_Y + p.HOLE_T
    g = p.WALL_HOLE_GROW
    return [
        box(p.IN_X0 - 0.01, p.IN_X1 + 0.01, hy0, hy1, hz0, hz1),
        box(p.OUT_X0 - 1, p.IN_X0 + 0.01, hy0 - g, hy1 + g, hz0 - g, hz1 + g),
        box(p.IN_X1 - 0.01, p.OUT_X1 + 1, hy0 - g, hy1 + g, hz0 - g, hz1 + g),
    ]


def _notches():
    wall = (p.OUT_X0 - 1, p.IN_X0 + 0.01)
    cuts = [
        board_box(*wall, *p.TF_NOTCH_Y, p.TF_NOTCH_Z0, 20),
        board_box(*wall, *p.USB_NOTCH_Y, p.USB_NOTCH_Z0, 20),
        board_box(*p.ROCKER_NOTCH_X, 40, 100, p.ROCKER_NOTCH_BZ0, 20),
        box(*p.LATCH_WIN_X, *p.LATCH_WIN_Y, p.Z_TI - 1, p.Z_TO + 1),
    ]
    for x0, x1 in p.TAB_XS:
        bb = (bezel_tab(x0, x1) & box(x0 - 1, x1 + 1, -40, 40, -5, p.FLOOR_TOP)).bounding_box()
        cuts.append(box(x0 - p.FIT_COVER, x1 + p.FIT_COVER, bb.min.Y - p.SLOT_FRONT_GAP,
                        bb.max.Y + p.SLOT_BACK_GAP, -1, p.FLOOR_TOP + 0.01))
    return cuts


def _vents():
    w = p.VENT_W / 2
    cuts = [box(x - w, x + w, p.Y_BACK_IN - 1, p.Y_BACK_OUT + 1, *p.BACK_VENT_Z) for x in p.BACK_VENT_XS]
    cuts += [box(x - w, x + w, *p.TOP_VENT_Y, p.Z_TI - 1, p.Z_TO + 1) for x in p.TOP_VENT_XS]
    cuts += [box(x - w, x + w, *p.FLOOR_VENT_Y, -1, p.FLOOR_TOP + 1) for x in p.FLOOR_VENT_XS]
    return cuts


def _part():
    report = []
    s = _outer() - box(p.IN_X0, p.IN_X1, -60, p.Y_BACK_IN, p.FLOOR_TOP, p.Z_TI)
    inside = _outer()  # keeps the brackets, gussets and bay inside the outer skin
    adds = bd.Compound(children=_brackets() + _sensor_bay() + _battery_shelf()) & inside
    s = s + adds
    # Anything in front of the front plane goes, the bezel lives there.
    s = s - board_prism_yz([(-40, p.FRONT_BZ), (120, p.FRONT_BZ), (120, 40), (-40, 40)], -10, 120)
    for c in _grilles() + _bar_holes() + _notches() + _vents():
        s = s - c
    solids = s.solids()
    report.append(f"solids {len(solids)}, volume {sum(x.volume for x in solids) / 1000:.1f} cm3")
    s = max(solids, key=lambda x: x.volume) if len(solids) > 1 else solids[0]
    return s, report


def check_geometry(part, report):
    assert len(part.solids()) == 1, len(part.solids())
    bb = part.bounding_box()
    report.append(f"case bbox X {bb.min.X:.2f} to {bb.max.X:.2f}, Y {bb.min.Y:.2f} to {bb.max.Y:.2f}, "
                  f"Z {bb.min.Z:.2f} to {bb.max.Z:.2f}")
    assert abs(bb.min.Z) < 1e-6 and abs(bb.max.Y - p.Y_BACK_OUT) < 1e-6

    def solid(x, y, z):
        return part.is_inside(bd.Vector(x, y, z))

    sq = p.SEN_INLET_SQ
    assert not solid(p.SEN_X0 + (sq[0] + sq[1]) / 2, p.Y_BACK_IN + 1, p.SEN_Z0 + (sq[2] + sq[3]) / 2), "square inlet open"
    assert not solid(p.SEN_X0 + p.SEN_OUTLET[0], p.Y_BACK_IN + 1, p.SEN_Z0 + p.SEN_OUTLET[1]), "outlet open"
    a, b, c, d = p.STANDOFFS[0]
    assert solid(p.SEN_X0 + (a + b) / 2, p.SEN_FACE_Y + 0.5, p.SEN_Z0 + (c + d) / 2), "standoff"
    assert not solid(p.SEN_X0 + 30, p.SEN_FACE_Y + 0.5, p.SEN_Z0 + 12), "gasket gap open"
    zc = (p.BAR_Z0 + p.BAR_Z1) / 2
    for x in (p.OUT_X0 + 1, sum(p.POST_XS[0]) / 2, sum(p.POST_XS[1]) / 2, p.OUT_X1 - 1):
        assert not solid(x, p.BAR_FRONT_Y + 1, zc), ("bar path", x)
    assert solid(sum(p.POST_XS[0]) / 2, p.BAR_FRONT_Y - 1, zc), "post in front of the bar"
    assert not solid(sum(p.LATCH_WIN_X) / 2, sum(p.LATCH_WIN_Y) / 2, p.Z_TI + 1), "latch window"
    assert solid(sum(p.LATCH_WIN_X) / 2, p.LATCH_WIN_Y[0] - 1, p.Z_TI + 1), "strip in front of the window"
    for sx, sy in p.STUDS:
        assert solid(*p.b2c(sx, sy, p.CUP_FLOOR_BZ - 0.5)), ("cup floor", sx, sy)
        assert not solid(*p.b2c(sx, sy, p.CUP_FLOOR_BZ + 0.5)), ("cup open", sx, sy)
    # Battery shelf
    for xc in p.SHELF_RIB_XC:
        assert solid(xc, p.y_at(50.0, p.RIB_BZ) + 1.0, 50.0), ("rib", xc)
        assert solid(*p.b2c(xc, p.BY_SEAT - p.FOOT_T / 2, (p.RIB_BZ + p.FOOT_FRONT_BZ) / 2)), ("foot", xc)
    mid_bz = p.RIB_BZ + (p.BAT_T + p.BAT_PLAY) / 2
    assert not solid(*p.b2c(40.0, 40.0, mid_bz)), "pack space open"
    assert not solid(*p.b2c((p.SH_X0 + p.SHELF_RIB_XC[0]) / 2, 30.0, mid_bz)), "lead corner open"
    assert solid(*p.b2c(sum(p.LEFT_FENCE_X) / 2, p.BY_TOP - 3.0, mid_bz)), "left fence"
    assert not solid(*p.b2c(sum(p.LEFT_FENCE_X) / 2, p.BY_SEAT + 5.0, mid_bz)), "left edge open low"
    assert solid(*p.b2c(sum(p.RIGHT_FENCE_X) / 2, p.BY_SEAT + 5.0, mid_bz)), "right fence"
    cx = sum(p.COIN_SLOT_X) / 2
    assert solid(cx, 32.0, sum(p.COIN_FLOOR_Z) / 2), "coin floor"
    assert solid(cx, sum(p.COIN_STOP_Y) / 2, p.COIN_FLOOR_Z[1] + 2.0), "coin front stop"
    assert not solid(cx, 32.0, p.COIN_FLOOR_Z[1] + 10.0), "coin slot open"
    assert not solid(cx, sum(p.COIN_STOP_Y) / 2, p.COIN_FLOOR_Z[1] + p.COIN_STOP_H + 2.0), "coin slot open above the stop"
    for x in p.BACK_VENT_XS:
        assert not solid(x, p.Y_BACK_IN + 1.0, sum(p.BACK_VENT_Z) / 2), ("back vent open", x)
    for name, body in (("pack", pack_body()), ("coin", coin_body())):
        inter = part & body
        clash = 0.0 if inter is None else inter.volume
        assert clash < 1e-3, (name, clash)
    report.append(f"pack {p.BAT_L} x {p.BAT_W} x {p.BAT_T} and coin {p.ASSUMED_RTC_D} x {p.ASSUMED_RTC_T} fit with no clash")


def _to_print():
    """Case frame to print frame, back wall on the bed, print Z = Y_BACK_OUT - Y."""
    return bd.Location((0, 0, p.Y_BACK_OUT)) * bd.Location((0, 0, 0), (1, 0, 0), -90)


@model
def shell():
    part, report = _part()
    check_geometry(part, report)
    for line in report:
        print("REPORT", line)
    part.label = "shell"
    return part


@model
def shell_print():
    part, report = _part()
    part = part.moved(_to_print())
    bb = part.bounding_box()
    report.append(f"print size {bb.size.X:.2f} x {bb.size.Y:.2f} x {bb.size.Z:.2f} mm, min Z {bb.min.Z:.3f}")
    assert abs(bb.min.Z) < 1e-6 and max(bb.size.X, bb.size.Y, bb.size.Z) <= p.PRINT_BED, bb
    for line in report:
        print("REPORT", line)
    part.label = "shell_print"
    return part


if __name__ == "__main__":
    shell()
    shell_print()
