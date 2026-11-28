# SPDX-License-Identifier: CC-BY-4.0. Copyright Walkthru.Earth.
"""Sensor bar, the screwless retainer that holds the SEN63C on its gasket.

DESIGN.md Sensor bar. A straight bar slides in from the right wall, through
the bar posts on each side of the sensor bay, and out through the left wall.
Its front face is the rigid stop on the front faces of the wall and post
holes, its back face squeezes the two foam pads onto the sensor rim, and the
sensor squeezes the gasket onto the back wall stand-offs.

The left end splits into two prongs with a barb each. The barbs pass the
holes with a 30 degree lead and catch the left wall outer face with a 45
degree return. The whole return sits outside the wall, its root 0.1 past
the wall face, so the bar has 0.2 of play along X and nothing is preloaded.
Release by squeezing the two prongs together outside the left wall and
pulling the head, or pull hard, the 45 degree return lets go.

Frames. sensor_bar() is in the case frame. sensor_bar_print() lies on its
front face (print Z = Y - BAR_FRONT_Y), so the prongs and barbs flex in the
layer plane.

Check: pixi run -e cad cad run sensor_bar.py
"""

import math
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))

import build123d as bd  # noqa: E402
from cad import model  # noqa: E402

import params as p  # noqa: E402
from shapes import box  # noqa: E402

RETURN_DEG = 45.0  # call, removable with a firm pull, preloads the head
LAND = 0.3  # call
SLIT = p.BAR_W - 2 * p.PRONG_H
SLIT_ROOT_R = SLIT / 2  # call, round slit end against a stress corner
ZC = (p.BAR_Z0 + p.BAR_Z1) / 2


def _xz_prism(pts):
    face = bd.Plane.XZ.offset(-p.BAR_FRONT_Y) * bd.Polygon(*pts, align=None)
    return bd.extrude(face, p.BAR_T, dir=(0, 1, 0))


def _barb(sign):
    """Barb on the upper (sign 1) or lower (sign -1) prong, an XZ profile."""
    z = p.BAR_Z1 if sign > 0 else p.BAR_Z0
    c = p.PRONG_CATCH
    run = c / math.tan(math.radians(RETURN_DEG))
    lead = c / math.tan(math.radians(p.LATCH_LEAD_DEG))
    x = p.BAR_BARB_X
    pts = [(x, z), (x - run, z + sign * c), (x - run - LAND, z + sign * c), (x - run - LAND - lead, z)]
    if sign < 0:
        pts.reverse()
    return _xz_prism(pts)


def _part():
    y0, y1 = p.BAR_FRONT_Y, p.BAR_BACK_Y
    s = box(p.PRONG_TIP_X, p.BAR_HEAD_X0, y0, y1, p.BAR_Z0, p.BAR_Z1)
    s = s + box(p.BAR_HEAD_X0, p.BAR_HEAD_X0 + p.HEAD[0], y0, y1, ZC - p.HEAD[1] / 2, ZC + p.HEAD[1] / 2)
    root_x = p.PRONG_ROOT_X - SLIT_ROOT_R
    s = s - box(p.PRONG_TIP_X - 1, root_x, y0 - 1, y1 + 1, ZC - SLIT / 2, ZC + SLIT / 2)
    s = s - bd.Pos(root_x, (y0 + y1) / 2, ZC) * bd.Cylinder(SLIT_ROOT_R, p.BAR_T + 2, rotation=(90, 0, 0))
    s = s + _barb(1) + _barb(-1)
    return s


def check_geometry(part, report):
    assert len(part.solids()) == 1, len(part.solids())
    bb = part.bounding_box()
    report.append(f"case bbox X {bb.min.X:.2f} to {bb.max.X:.2f}, Y {bb.min.Y:.2f} to {bb.max.Y:.2f}, "
                  f"Z {bb.min.Z:.2f} to {bb.max.Z:.2f}, volume {part.volume / 1000:.2f} cm3")
    hole_half = p.BAR_W / 2 + p.HOLE_CLEAR + p.WALL_HOLE_GROW
    catch = p.BAR_W / 2 + p.PRONG_CATCH - hole_half
    assert catch > 0.2, catch
    assert bb.min.X >= p.PRONG_TIP_X - 1e-6 and bb.min.X < p.OUT_X0 - 1.0, "prong tips outside the left wall"
    assert bb.max.X > p.OUT_X1, "head outside the right wall"
    assert p.HEAD[1] / 2 > hole_half + 1.0, "head covers the right wall hole"
    yc = (p.BAR_FRONT_Y + p.BAR_BACK_Y) / 2
    assert not part.is_inside(bd.Vector(p.PRONG_TIP_X + 1, yc, ZC)), "slit open"
    xb = p.BAR_BARB_X - p.PRONG_CATCH - LAND / 2
    assert part.is_inside(bd.Vector(xb, yc, p.BAR_Z1 + p.PRONG_CATCH - 0.05)), "upper barb"
    assert part.is_inside(bd.Vector(xb, yc, p.BAR_Z0 - p.PRONG_CATCH + 0.05)), "lower barb"
    assert p.BAR_BARB_X - p.PRONG_CATCH - LAND - p.PRONG_CATCH / math.tan(math.radians(p.LATCH_LEAD_DEG)) \
        >= p.PRONG_TIP_X - 1e-6, "barb fits on the prong"
    report.append(f"barb catch {catch:.2f} over the wall hole, prong strain "
                  f"{p.latch_strain_constant(p.PRONG_L, catch, p.PRONG_H):.2f} % (limit {p.PLA_STRAIN_LIMIT_XY} %), "
                  f"slit {SLIT:.2f}")


@model
def sensor_bar():
    report = []
    part = _part()
    check_geometry(part, report)
    for line in report:
        print("REPORT", line)
    part.label = "sensor_bar"
    return part


@model
def sensor_bar_print():
    part = _part()
    # Rotate 90 about X maps case Y to print Z, then lift the front face to Z 0.
    part = part.moved(bd.Location((0, 0, 0), (1, 0, 0), 90))
    bb = part.bounding_box()
    part = part.moved(bd.Location((0, 0, -bb.min.Z)))
    bb = part.bounding_box()
    print("REPORT", f"print size {bb.size.X:.2f} x {bb.size.Y:.2f} x {bb.size.Z:.2f} mm")
    assert abs(bb.size.Z - p.BAR_T) < 1e-6 and bb.size.X <= p.PRINT_BED, bb
    part.label = "sensor_bar_print"
    return part


if __name__ == "__main__":
    sensor_bar()
    sensor_bar_print()
