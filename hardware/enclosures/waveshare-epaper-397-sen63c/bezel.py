# SPDX-License-Identifier: CC-BY-4.0. Copyright Walkthru.Earth.
"""Bezel, the front of the Waveshare ePaper-3.97 and SEN63C desk stand.

DESIGN.md Bezel. A plate in the front plane (board bz 0.1 to 3.0) between the
side walls, from the floor top to 0.3 under the top wall. It frames the active
display area, keeps 0.1 off the PCB front and 0.3 off the display glass, and
holds the board on its cups. Two bottom tabs drop into floor slots and one
tapered in-plane arm at the top snaps its tooth into a window in the top wall.

Fit it bottom first, tabs into the slots, then swing the top in until the
tooth clicks. Release by pressing the tooth down through the top window and
tilting the top forward.

Frames. bezel() is in the case frame. bezel_print() lies face down (print
Z = 3.0 - bz), so the window chamfer, the tab chamfers and the arm all print
without supports, and the arm flexes along the layers.

Check: pixi run -e cad cad run bezel.py, then
pixi run -e cad cad model dfm bezel.py:bezel_print --process printing --technology fdm --material pla
"""

import math
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))

import build123d as bd  # noqa: E402
from cad import model  # noqa: E402

import params as p  # noqa: E402
from shapes import bezel_tab, board_box, box, prism_yz  # noqa: E402

ROOT_L = 4.0  # call, root block behind the bezel that anchors the arm
ROOT_H = 2.6  # call
ARM_Y0 = p.y_front(p.ARM_Z1)
RUN = p.TOOTH_H / math.tan(math.radians(p.TOOTH_RETURN_DEG))
LEAD = p.TOOTH_H / math.tan(math.radians(p.LATCH_LEAD_DEG))
TOOTH_Y0 = p.LATCH_WIN_Y[0] + p.TOOTH_GAP - p.BZ_TOP_GAP * RUN / p.TOOTH_H


def _window():
    (ax0, ax1), (ay0, ay1) = p.ACTIVE_X, p.ACTIVE_Y
    m, c = p.WINDOW_MARGIN, p.WINDOW_CHAMFER
    cx, cy, w, h = (ax0 + ax1) / 2, (ay0 + ay1) / 2, ax1 - ax0 + 2 * m, ay1 - ay0 + 2 * m
    straight = box(ax0 - m, ax1 + m, ay0 - m, ay1 + m, p.BEZEL_BACK_BZ - 1, p.FRONT_BZ + 1)
    f0 = bd.Plane.XY.offset(p.FRONT_BZ - c) * bd.Pos(cx, cy) * bd.Rectangle(w, h)
    g = c + 0.2
    f1 = bd.Plane.XY.offset(p.FRONT_BZ + 0.2) * bd.Pos(cx, cy) * bd.Rectangle(w + 2 * g, h + 2 * g)
    return p.board_to_case() * (straight + bd.loft([f0, f1]))


def _latch():
    z1 = p.ARM_Z1
    pts = [(p.ARM_TIP_X, z1), (p.ARM_ROOT_X + 0.5, z1), (p.ARM_ROOT_X + 0.5, z1 - p.LATCH_H_ROOT),
           (p.ARM_TIP_X, z1 - p.LATCH_H_TIP)]
    # Built 2 deeper toward the front and cut at the front plane, so the arm front
    # is flush with the bezel face and lies on the plate in print.
    face = bd.Plane.XZ.offset(-(ARM_Y0 - 2.0)) * bd.Polygon(*pts, align=None)
    arm = bd.extrude(face, p.ARM_B + 2.0, dir=(0, 1, 0))
    arm = arm & board_box(-10, 120, -60, 120, -40, p.FRONT_BZ)
    # The root block starts inside the bezel at its lowest point, so no face of it
    # looks down toward the front (15 degrees off flat in print).
    back_y = p.y_at(z1 - ROOT_H, p.BEZEL_BACK_BZ)
    root = box(p.ARM_ROOT_X, p.ARM_ROOT_X + ROOT_L, back_y - 0.5, ARM_Y0 + p.ARM_B, z1 - ROOT_H, z1)
    y0, h = TOOTH_Y0, p.TOOTH_H
    prof = [(y0, z1 - 0.3), (y0, z1), (y0 + RUN, z1 + h), (y0 + RUN + p.TOOTH_LAND, z1 + h),
            (y0 + RUN + p.TOOTH_LAND + LEAD, z1), (y0 + RUN + p.TOOTH_LAND + LEAD, z1 - 0.3)]
    tooth = prism_yz(prof, *p.TOOTH_X)
    return arm + root + tooth


def _part():
    report = []
    s = board_box(p.BZ_X0, p.BZ_X1, -30, 80, p.BEZEL_BACK_BZ, p.FRONT_BZ) & box(-10, 120, -40, 60, p.FLOOR_TOP, p.ARM_Z1)
    s = s - _window()
    s = s - board_box(-1, p.DISP_X[1] + p.POCKET_CLEAR, p.DISP_Y[0] - p.POCKET_CLEAR, p.DISP_Y[1] + p.POCKET_CLEAR,
                      p.BEZEL_BACK_BZ - 1, p.POCKET_BZ1)
    s = s - board_box(*p.KEY_WINDOW_X, *p.KEY_WINDOW_Y, -1, p.FRONT_BZ + 1)
    # Join the key window to the display pocket behind the front skin, which
    # removes a 0.19 sliver between them.
    s = s - board_box(*p.KEY_WINDOW_X, p.KEY_WINDOW_Y[1] - 0.01, p.DISP_Y[0] - p.POCKET_CLEAR + 0.01,
                      p.BEZEL_BACK_BZ - 1, p.POCKET_BZ1)
    s = s - box(*p.ROCKER_NOTCH_X, -40, 60, p.b2c(0, 60.3, p.FRONT_BZ)[2], 100)
    s = s - box(p.ARM_TIP_X - p.PRINT_GAP, p.ARM_ROOT_X, -40, 60, p.ARM_Z1 - p.LATCH_H_ROOT - p.ARM_SLOT, 100)
    s = s + _latch()
    for x0, x1 in p.TAB_XS:
        s = s + bezel_tab(x0, x1)
    solids = s.solids()
    report.append(f"solids {len(solids)}, volume {sum(x.volume for x in solids) / 1000:.2f} cm3")
    return (solids[0] if len(solids) == 1 else s), report


def check_geometry(part, report):
    assert len(part.solids()) == 1, len(part.solids())
    bb = part.bounding_box()
    report.append(f"case bbox X {bb.min.X:.2f} to {bb.max.X:.2f}, Y {bb.min.Y:.2f} to {bb.max.Y:.2f}, "
                  f"Z {bb.min.Z:.2f} to {bb.max.Z:.2f}")
    assert bb.min.X >= p.IN_X0 + p.FIT_COVER - 1e-6 and bb.max.X <= p.IN_X1 - p.FIT_COVER + 1e-6

    def solid(bx, by, bz):
        return part.is_inside(bd.Vector(*p.b2c(bx, by, bz)))

    assert not solid(sum(p.ACTIVE_X) / 2, sum(p.ACTIVE_Y) / 2, 2.0), "window open"
    assert not solid(p.ACTIVE_X[0] - 0.1, sum(p.ACTIVE_Y) / 2, 2.0), "window edge outside the active area"
    assert solid(p.ACTIVE_X[0] - 2.0, sum(p.ACTIVE_Y) / 2, 2.0), "frame left of the window"
    assert not solid(5.0, 30.0, 1.0), "display pocket"
    assert solid(p.PCB_W - 0.3, 30.0, 0.5), "bezel back over the PCB margin"
    assert not solid(sum(p.KEY_WINDOW_X) / 2, -3.0, 2.0), "key window"
    zc = p.ARM_Z1 - p.LATCH_H_TIP - 0.5
    assert not part.is_inside(bd.Vector(p.ARM_TIP_X + 1, ARM_Y0 + 1, zc)), "slot under the arm tip"
    assert part.is_inside(bd.Vector(sum(p.TOOTH_X) / 2, TOOTH_Y0 + RUN, p.ARM_Z1 + p.TOOTH_H - 0.1)), "tooth"
    for x0, x1 in p.TAB_XS:
        assert part.is_inside(bd.Vector((x0 + x1) / 2, *p.b2c(0, -12.0, -1.0)[1:])), ("tab", x0)
    report.append(f"arm root strain {p.latch_strain_tapered(p.LATCH_L):.2f} % (limit {p.PLA_STRAIN_LIMIT_XY} %), "
                  f"tooth Y {TOOTH_Y0:.2f} to {TOOTH_Y0 + RUN + p.TOOTH_LAND + LEAD:.2f}, window Y "
                  f"{p.LATCH_WIN_Y[0]:.2f} to {p.LATCH_WIN_Y[1]:.2f}")


def _to_print():
    """Case frame to print frame, face down, print Z = 3.0 - bz."""
    return bd.Location((0, 0, p.FRONT_BZ), (1, 0, 0), 180) * p.board_to_case().inverse()


@model
def bezel():
    part, report = _part()
    check_geometry(part, report)
    for line in report:
        print("REPORT", line)
    part.label = "bezel"
    return part


@model
def bezel_print():
    part, report = _part()
    part = part.moved(_to_print())
    bb = part.bounding_box()
    report.append(f"print size {bb.size.X:.2f} x {bb.size.Y:.2f} x {bb.size.Z:.2f} mm, min Z {bb.min.Z:.3f}")
    assert abs(bb.min.Z) < 1e-6 and max(bb.size.X, bb.size.Y, bb.size.Z) <= p.PRINT_BED, bb
    for line in report:
        print("REPORT", line)
    part.label = "bezel_print"
    return part


if __name__ == "__main__":
    bezel()
    bezel_print()
