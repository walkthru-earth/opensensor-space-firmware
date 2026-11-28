# SPDX-License-Identifier: CC-BY-4.0. Copyright Walkthru.Earth.
"""Small build123d helpers shared by the part scripts of this set.

box() and prism_yz() are in the case frame. board_box() and board_prism_yz()
take board frame numbers and return the solid placed in the case frame.
bezel_tab() is the one tab shape both shell.py (its floor slot) and bezel.py
(the tab itself) use, so the two never drift apart.
"""

import build123d as bd

import params as p


def box(x0, x1, y0, y1, z0, z1):
    return bd.Pos((x0 + x1) / 2, (y0 + y1) / 2, (z0 + z1) / 2) * bd.Box(x1 - x0, y1 - y0, z1 - z0)


def prism_yz(pts, x0, x1):
    """Prism along +X from x0 to x1 with the (y, z) polygon as its section."""
    face = bd.Plane.YZ.offset(x0) * bd.Polygon(*pts, align=None)
    return bd.extrude(face, x1 - x0, dir=(1, 0, 0))


def board_box(bx0, bx1, by0, by1, bz0, bz1):
    return p.board_to_case() * box(bx0, bx1, by0, by1, bz0, bz1)


def board_prism_yz(pts, bx0, bx1):
    """Prism along board x with a (by, bz) polygon, placed in the case frame."""
    return p.board_to_case() * prism_yz(pts, bx0, bx1)


def by_at_z(z, bz):
    """Board y where the plane bz meets the case height z."""
    return (z - p.OZ - bz * p.S) / p.C


def bezel_tab(x0, x1):
    """One bottom tab with its boss, case frame.

    A board frame block bz TAB_BZ from below the floor top up to TAB_BOSS_Z1,
    cut at Z = FLOOR_TOP - TAB_DROP, with a 45 degree chamfer (board frame)
    on the front lower edge so it prints face down without an overhang.
    """
    bz0, bz1 = p.TAB_BZ
    block = board_box(x0, x1, by_at_z(-5.0, bz1), by_at_z(p.TAB_BOSS_Z1 + 5.0, bz0), bz0, bz1)
    keep = box(x0 - 1, x1 + 1, -40, 40, p.FLOOR_TOP - p.TAB_DROP, p.TAB_BOSS_Z1)
    by_line = by_at_z(p.FLOOR_TOP, bz1)
    cut = board_prism_yz([(by_line, bz1), (by_line - 20, bz1 - 20), (by_line - 20, 10), (by_line, 10)], x0 - 1, x1 + 1)
    return (block & keep) - cut


def pack_body():
    """The LiPo pack as a plain box resting on the shelf ribs and feet, case frame."""
    x0 = p.SH_X0 + p.BAT_PLAY / 2
    by0, bz0 = p.BY_SEAT + 0.05, p.RIB_BZ + 0.05
    return board_box(x0, x0 + p.BAT_L, by0, by0 + p.BAT_W, bz0, bz0 + p.BAT_T)


def coin_body():
    """The RTC coin standing in its slot on the floor against the back wall, case frame. ASSUMED size."""
    r = p.ASSUMED_RTC_D / 2
    cyl = bd.Cylinder(r, p.ASSUMED_RTC_T)  # axis along Z, turned to X
    return bd.Pos(sum(p.COIN_SLOT_X) / 2, p.Y_BACK_IN - r - 0.05, p.COIN_FLOOR_Z[1] + r + 0.05) * bd.Rot(0, 90, 0) * cyl
