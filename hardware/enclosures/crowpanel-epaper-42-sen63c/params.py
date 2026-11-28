# SPDX-License-Identifier: CC-BY-4.0. Copyright Walkthru.Earth.
"""Shared numbers for the CrowPanel 4.2 and SEN63C desk base, iteration 1.

All lengths are mm, angles degrees, plain floats. Pure Python, so
`pixi run -e cad python -I params.py` runs the asserts without build123d.
build123d is imported only inside panel_to_base() and sensor_to_base().

Source tags used in the comments
  BRD   hardware/parts/boards/crowpanel-epaper-42.md (section, row)
  SEN   hardware/parts/sensors/sen63c.md (section, row)
  MAT   .claude/skills/hardware-design/materials-and-closures.md (section)
  CADP  text-to-cad cad skill, references/manufacturing-printing.md
  DES   DESIGN.md in this folder (section)
  PLAN  PLAN-iteration-1.md in this folder (task, step)
  ASSUMED   a row of the DESIGN.md "Assumed for iteration 1" table, or a
            number with no source, named ASSUMED_* so iteration 2 changes one
            line
  builder call  a layout choice made here from the sourced numbers, not a
            source value. Each one says what it is derived from.

Frames
  Panel frame: the board sheet viewer frame (BRD "Frame used in this sheet").
    x across the panel, y up the panel from the GPIO header edge, z out of
    the front face. PCB front is z 0.
  Sensor frame: the sensor sheet frame (SEN "Coordinate convention").
    x along the 55.2 length from the inlet end, y across the 25.6 width,
    z from the opening face (z 0) to the connector rim (z 21.3).
  Base frame (PLAN "Global Constraints"): X equals panel x, Y horizontal
    from the front of the base toward the back, Z up, Z 0 on the desk.
"""

import math

# ---------------------------------------------------------------------------
# Panel, from the board sheet (BRD)
# ---------------------------------------------------------------------------
PANEL_SHELL_X0 = -0.15  # BRD Printed shell, top cover row, x -0.15 to 107.15
PANEL_SHELL_X1 = 107.15  # BRD Printed shell, top cover row
PANEL_SHELL_Y0 = -0.15  # BRD Envelope, with printed shell row, y -0.15 to 87.15
PANEL_SHELL_Y1 = 87.15  # BRD Envelope, with printed shell row
PANEL_TOP_COVER_Z1 = 1.5  # BRD Printed shell, top cover z 0.5 to 1.5
PANEL_FRONT_Z = 1.7  # BRD Envelope, z max 1.7 (glass front face)
PANEL_BACK_COVER_Z0 = -7.6  # BRD Printed shell, bottom cover z -7.6 to -5.6
PANEL_PLUNGER_Z0 = -8.1  # BRD Controls, BOOT and RST tact rows, z -8.1
PANEL_DIAL_X0 = -2.449  # BRD Envelope, x min is the dial wheel
PANEL_GLASS_Y0 = 5.0  # BRD Display panel, glass row, y 5 to 82
PANEL_ACTIVE_Y0 = 8.1  # BRD Display panel, active area row, y 8.1 to 71.7
PANEL_MENU_Y0 = 20.5  # DES Cradle bullet, MENU key at y 20.5
PANEL_SIDE_LIP_MAX_Y = 15.0  # DES Cradle bullet, side lips no higher than y 15
PANEL_HEADER_X0 = 20.7  # BRD GPIO header, housing x 20.7 to 46.3
PANEL_HEADER_X1 = 46.3  # BRD GPIO header, housing row
PANEL_HEADER_FACE_Y = 1.0  # BRD GPIO header, socket face points down at y 1.0
PANEL_HEADER_ROW_NEAR_Z = -2.83  # BRD GPIO header, near row z -2.83
PANEL_HEADER_ROW_FAR_Z = -5.37  # BRD GPIO header, far row z -5.37
PANEL_HEADER_MOUTH_X0 = 19.2  # BRD Printed shell, header mouth x 19.2 to 47.8
PANEL_HEADER_MOUTH_X1 = 47.8  # BRD Printed shell, header mouth row
PANEL_BOOT_X0 = 67.555  # BRD Controls, BOOT tact x 67.555 to 72.055
PANEL_BOOT_X1 = 72.055  # BRD Controls, BOOT tact row
PANEL_BOOT_CENTRE = (69.805, 5.5)  # BRD Printed shell, bottom cover d2.8 hole for BOOT
PANEL_RST_X0 = 81.25  # BRD Controls, RST tact x 81.25 to 85.75
PANEL_RST_X1 = 85.75  # BRD Controls, RST tact row
PANEL_RST_CENTRE = (83.5, 5.5)  # BRD Printed shell, bottom cover d2.8 hole for RST
PANEL_BUTTON_HOLE_D = 2.8  # BRD Printed shell, bottom cover d2.8 holes
PANEL_SCREW_LOWER = ((4.0, 4.0), (103.0, 4.0))  # DES Screw head relief bullet, (4, 4) and (103, 4)
PANEL_HEAT_X0 = 68.0  # BRD Heat sources, warm cluster x 68 to 104
PANEL_HEAT_X1 = 104.0  # BRD Heat sources, warm cluster
PANEL_TF_X0 = 19.75  # DES Backrest bullet, TF slot x 19.75 to 36.15
PANEL_TF_X1 = 36.15  # DES Backrest bullet
PANEL_BACKREST_X0 = 47.0  # DES Backrest bullet, spine at x 47 to 67, left of the heat zone
PANEL_BACKREST_X1 = 67.0  # DES Backrest bullet
PANEL_BOOT_RST_WINDOW_X0 = 65.0  # DES BOOT and RST window bullet, x 65 to 88
PANEL_BOOT_RST_WINDOW_X1 = 88.0  # DES BOOT and RST window bullet
PANEL_BOOT_RST_WINDOW_Y1 = 10.0  # DES BOOT and RST window bullet, up to y 10

# STEP to panel viewer frame, BRD Frame used in this sheet:
# x = 53.5 - X, y = Z + 43.5, z = Y + 0.5
PANEL_STEP_ORIGIN = (53.5, 43.5, 0.5)

# ---------------------------------------------------------------------------
# Sensor, from the sensor sheet (SEN)
# ---------------------------------------------------------------------------
SEN_L = 55.2  # SEN 1, Length 55.2 +0.2 / -0.4
SEN_W = 25.6  # SEN 1, Width 25.6 +0.1 / -0.2
SEN_H = 21.3  # SEN 1, Height overall 21.3 ±0.5
SEN_MAX_L = 55.4  # SEN 1, outline conflict settled, maximum material size
SEN_MAX_W = 25.7  # SEN 1, same
SEN_MAX_H = 21.8  # SEN 1, same
SEN_SIDE_CLEAR = 0.3  # SEN 7, rule 12, about 0.3 side clearance to the outline
SEN_INLET_SQ = (3.2, 10.2, 2.8, 9.8)  # SEN 2, square inlet x0, x1, y0, y1
SEN_INLET_ROUND = (6.7, 18.2, 6.0)  # SEN 2, round inlet centre x, y and diameter
SEN_OUTLET = (42.4, 12.8, 20.5)  # SEN 2, outlet centre x, y and diameter
SEN_MIN_INLET_AREA = 56.0  # SEN 2, effective inlet area, both inlets
SEN_MIN_OUTLET_AREA = 148.0  # SEN 2, effective outlet area
SEN_CONN_X0 = 17.8  # SEN 4, connector, worked in Task 1 from the STEP pocket (x 17.8 to 28.5)
SEN_CONN_X1 = 28.5  # SEN 4, same
SEN_CONN_Y0 = 16.9  # SEN 4, same, y 16.9 to 21.15
SEN_CONN_Y1 = 21.15  # SEN 4, same
SEN_MASS_MAX_G = 22.0  # SEN 1, Mass 18 min, 20 typ, 22 max g

# STEP to sensor sheet frame, SEN Coordinate convention:
# x = X + 27.6, y = Z + 12.8, z = 21.3 - Y
SEN_STEP_ORIGIN = (27.6, 12.8, 21.3)

# ---------------------------------------------------------------------------
# Material and process (MAT)
# ---------------------------------------------------------------------------
PLA_WALL_1 = 0.45  # MAT 1.3, Prusa walls 0.45, 0.9, 1.35, 1.8 for 1 to 4 perimeters
PLA_WALL_2 = 0.9  # MAT 1.3
PLA_WALL_3 = 1.35  # MAT 1.3
PLA_WALL_4 = 1.8  # MAT 1.3
PLA_MIN_WALL = 0.8  # CADP Walls, pins and holes, FDM standing wall 0.8
PLA_MAX_BRIDGE = 5.0  # CADP Overhangs, flat ceiling bridge up to 5 mm
PLA_OVERHANG_DEG = 45.0  # CADP Overhangs, and MAT 4 overhang for snap faces 45°
PLA_ELEPHANT_FOOT = 0.10  # MAT 1.3, elephant foot example 0.10
PLA_E_XY_MPA = 3259.39  # MAT 1.1, flexural modulus X-Y
PLA_ELONG_XY = 4.14  # MAT 1.1, elongation at break X-Y, percent
PLA_HDT_C = 53.78  # MAT 1.1, HDT at 0.45 MPa
PLA_STRAIN_LIMIT_XY = 1.2  # MAT 1.2, project rule, 1.2 % root strain in X-Y
PLA_DENSITY_G_CM3 = 1.24  # ASSUMED, PLAN Review Focus 5 only, not in MAT or the eSUN TDS
PRINT_BED = 246.0  # PLAN Task 1 Step 1 limit, inside the A1 256 mm cube (MAT 1.3)

# ---------------------------------------------------------------------------
# Fits (MAT 1.4 and 2.6, ASSUMED table)
# ---------------------------------------------------------------------------
FIT_SLIDE = 0.20  # ASSUMED table, sliding fits 0.20 per side, MAT 1.4 range 0.20 to 0.25
FIT_COVER = 0.30  # MAT 1.4, loose fit 0.30 to 0.40, low end
FIT_LATCH_GAP = 0.4  # MAT 6.1, 0.4 gap around the arm
FIT_DETENT = 0.4  # MAT 4, bump 0.4 to 1 (OpenAT), low end, PLAN Task 4 0.4
FIT_PANEL = FIT_SLIDE  # builder call, panel shell to cradle faces at the sliding fit
FIT_SENSOR = SEN_SIDE_CLEAR  # SEN 7 rule 12

# ---------------------------------------------------------------------------
# Assumed for iteration 1, one per row of DESIGN.md table
# ---------------------------------------------------------------------------
ASSUMED_JUMPER_HOUSING = (2.54, 2.54, 14.0)  # ASSUMED, row Jumper housing
ASSUMED_JUMPER_ROOM = 25.0  # ASSUMED, row Room below the shell bottom edge for jumpers
ASSUMED_SCREW_HEAD_D = 4.5  # ASSUMED, row Shell screw heads, M2.5 pan head d 4.5
ASSUMED_SCREW_HEAD_H = 1.8  # ASSUMED, row Shell screw heads, 1.8 past the back cover
ASSUMED_SCREW_RELIEF_D = 6.0  # ASSUMED, row Screw head relief, d 6.0
ASSUMED_SCREW_RELIEF_DEPTH = 2.5  # ASSUMED, row Screw head relief, 2.5 deep
ASSUMED_GASKET_T = 3.0  # ASSUMED, row Sensor gasket, 3.0 closed-cell foam
ASSUMED_GASKET_COMPRESSED_T = 2.0  # ASSUMED, row Sensor gasket, compressed to 2.0
ASSUMED_END_PAD_T = 3.0  # ASSUMED, row Sensor end pads, 3.0 foam
ASSUMED_END_PAD_COMPRESSED_T = 2.0  # ASSUMED, row Sensor end pads, compressed to 2.0
ASSUMED_PLUG_ZONE = 15.0  # ASSUMED, row Plug and bend below the sensor rim
ASSUMED_FIT_SLIDE = 0.20  # ASSUMED, row Sliding fits for the full parts
ASSUMED_FOOT_D = 10.0  # ASSUMED, row Rubber feet, 10 round
ASSUMED_FOOT_H = 3.0  # ASSUMED, row Rubber feet, 3 tall
ASSUMED_PANEL_MASS_G = 120.0  # ASSUMED, PLAN Review Focus 5, not in source

TILT_DEG = 15.0  # DES Cradle bullet, 15 degree tilt

# ---------------------------------------------------------------------------
# Latch (MAT 6.1 and 4.1), used by the test plate and the backrest
# ---------------------------------------------------------------------------
LATCH_L_OPTIONS = (15.0, 18.0, 20.0)  # PLAN Task 2, MAT 6.1 l 15 to 20
LATCH_L = 18.0  # PLAN Task 4, the test plate's 18 mm variant
LATCH_H_ROOT = 1.8  # MAT 6.1
LATCH_H_TIP = 0.9  # MAT 6.1, half the root
LATCH_B = 6.0  # MAT 6.1, b 6 (above the 5 mm Hubs minimum, MAT 4)
LATCH_CATCH = 1.0  # MAT 6.1, undercut 1.0 (range 1.0 to 1.2), PLAN Task 4 overlap at most 1.2
LATCH_LEAD_DEG = 30.0  # MAT 6.1
LATCH_RETURN_DEG = 45.0  # MAT 6.1
LATCH_ROOT_FILLET = 0.9  # MAT 6.1, and MAT 4 at least 0.5 x root


def latch_strain_tapered(length, catch=LATCH_CATCH, h_root=LATCH_H_ROOT):
    """Root strain in percent, Bayer tapered arm, MAT 4.1: eps = y h / (1.09 l^2)."""
    return 100.0 * catch * h_root / (1.09 * length**2)


def latch_strain_constant(length, catch=LATCH_CATCH, h_root=LATCH_H_ROOT):
    """Root strain in percent, Fictiv constant section, MAT 2.1: eps = 1.5 t Y / L^2, Q = 1.

    Constant section, so it over-states the strain of the tapered arm that
    latch_strain_tapered() covers. Kept as the stricter check.
    """
    return 100.0 * 1.5 * h_root * catch / length**2


# ---------------------------------------------------------------------------
# Walls and bottom stack (base frame Z)
# ---------------------------------------------------------------------------
WALL = PLA_WALL_4  # builder call, 4 perimeters, 1.8
SKIN = PLA_WALL_3  # builder call, least skin left behind a relief or socket, 3 perimeters
FOOT_RECESS = 1.0  # builder call, foot sits 1.0 into the plate so it cannot shear off
PLATE_T = 3.0  # builder call, bottom plate thickness, fits a 1.0 foot recess with 2.0 left
BASE_Z0 = ASSUMED_FOOT_H - FOOT_RECESS  # plate underside, 2.0
PLATE_TOP_Z = BASE_Z0 + PLATE_T  # 5.0, also the open bottom of the pocket and bay

# Bottom plate pad under the sensor (DES Bottom plate and Review Focus 3)
PAD_UNDER_T = ASSUMED_END_PAD_T  # builder call, same 3.0 foam as the end pads
PAD_UNDER_COMPRESSION = 1.0  # PLAN Task 4, pad seat leaves 1.0 of compression
PAD_SEAT_D = 0.5  # builder call, shallow seat that locates the pad
SEN_Z0 = PLATE_TOP_Z - PAD_SEAT_D + PAD_UNDER_T - PAD_UNDER_COMPRESSION  # sensor lower edge, 6.5

# ---------------------------------------------------------------------------
# Base X extents
# ---------------------------------------------------------------------------
CRADLE_SIDE_IN_X0 = PANEL_SHELL_X0 - FIT_PANEL  # -0.35
CRADLE_SIDE_IN_X1 = PANEL_SHELL_X1 + FIT_PANEL  # 107.35
BASE_X0 = CRADLE_SIDE_IN_X0 - WALL  # -2.15
BASE_X1 = CRADLE_SIDE_IN_X1 + WALL  # 109.15
BASE_W = BASE_X1 - BASE_X0  # 111.3

# ---------------------------------------------------------------------------
# Cradle, in the panel frame (DES Cradle bullet)
# ---------------------------------------------------------------------------
CRADLE_FLOOR_Y = PANEL_SHELL_Y0  # shell bottom edge rests on the floor
CRADLE_FLOOR_T = WALL  # builder call
CRADLE_FRONT_LIP_IN_Z = PANEL_FRONT_Z + FIT_PANEL  # 1.9, lip inner face clears the glass face
CRADLE_FRONT_LIP_H = 3.5  # DES Cradle bullet, about 3.5 tall
CRADLE_FRONT_LIP_T = WALL  # builder call
CRADLE_FRONT_LIP_TOP_PANEL_Y = CRADLE_FLOOR_Y + CRADLE_FRONT_LIP_H  # 3.35
# The lip inner face leans out by TILT_DEG from the floor to the lip top, see
# base_body.py Front lip draft. The base and the test plate coupon share it.
CRADLE_FRONT_LIP_TOP_IN_Z = CRADLE_FRONT_LIP_IN_Z + CRADLE_FRONT_LIP_H * math.tan(math.radians(TILT_DEG))
CRADLE_REAR_IN_Z = PANEL_BACK_COVER_Z0 - FIT_PANEL  # -7.8
CRADLE_REAR_WALL_T = (ASSUMED_SCREW_RELIEF_DEPTH - FIT_PANEL) + SKIN  # 3.65, skin behind the relief
CRADLE_REAR_OUT_Z = CRADLE_REAR_IN_Z - CRADLE_REAR_WALL_T  # -11.45
CRADLE_SIDE_LIP_TOP_PANEL_Y = 12.0  # builder call, below the y 15 limit and the MENU key
CRADLE_REAR_WALL_TOP_PANEL_Y = CRADLE_SIDE_LIP_TOP_PANEL_Y  # builder call
CRADLE_HEADER_SLOT_X0 = 19.0  # DES Header slot bullet, x 19 to 48
CRADLE_HEADER_SLOT_X1 = 48.0  # DES Header slot bullet
CRADLE_HEADER_SLOT_Z0 = CRADLE_REAR_IN_Z  # builder call, slot spans the full channel width
CRADLE_HEADER_SLOT_Z1 = CRADLE_FRONT_LIP_IN_Z  # builder call
CRADLE_SCREW_RELIEF_D = ASSUMED_SCREW_RELIEF_D
CRADLE_SCREW_RELIEF_BOTTOM_Z = PANEL_BACK_COVER_Z0 - ASSUMED_SCREW_RELIEF_DEPTH  # -10.1
CRADLE_WINDOW_X0 = PANEL_BOOT_RST_WINDOW_X0
CRADLE_WINDOW_X1 = PANEL_BOOT_RST_WINDOW_X1
CRADLE_WINDOW_Y1 = PANEL_BOOT_RST_WINDOW_Y1

# Pivot: the shell front bottom edge, panel (y -0.15, z 1.5)
CRADLE_PIVOT_PANEL = (CRADLE_FLOOR_Y, PANEL_TOP_COVER_Z1)

# Sweeps about the pivot, tilt from upright. The panel rests on the rear wall
# at TILT_DEG 15, so it tips forward from 15 toward 0 to go in or out, and
# PANEL_SWEEP_DEG is its whole envelope (Task 3 sweep, clean, least 0.200).
# The jumper block sweep runs wider, 0 to 35, as extra margin for the slot.
PANEL_SWEEP_DEG = tuple(float(a) for a in range(0, 16))  # builder call, 1 degree steps
INSERT_SWEEP_DEG = tuple(float(a) for a in range(0, 36))  # builder call, jumper block, 1 degree steps
JUMPER_CLEAR = 1.0  # builder call, clearance from the jumper block to any wall

# Jumper block in the panel frame: housings on both rows, 2.54 square, hanging
# from the socket face to ASSUMED_JUMPER_ROOM below the shell bottom edge.
JUMPER_X0 = PANEL_HEADER_X0
JUMPER_X1 = PANEL_HEADER_X1
JUMPER_Y0 = PANEL_SHELL_Y0 - ASSUMED_JUMPER_ROOM  # -25.15
JUMPER_Y1 = PANEL_HEADER_FACE_Y  # 1.0
JUMPER_Z0 = PANEL_HEADER_ROW_FAR_Z - ASSUMED_JUMPER_HOUSING[0] / 2  # -6.64
JUMPER_Z1 = PANEL_HEADER_ROW_NEAR_Z + ASSUMED_JUMPER_HOUSING[0] / 2  # -1.56

_S = math.sin(math.radians(TILT_DEG))
_C = math.cos(math.radians(TILT_DEG))


def _ceil_half(v):
    return math.ceil(v * 2.0 - 1e-9) / 2.0


def _rel(dy, dz, deg):
    """Panel offset from the pivot to base (dY, dZ) at tilt deg back from vertical."""
    s, c = math.sin(math.radians(deg)), math.cos(math.radians(deg))
    return dy * s - dz * c, dy * c + dz * s


def _jumper_corners():
    py, pz = CRADLE_PIVOT_PANEL
    return [(y - py, z - pz) for y in (JUMPER_Y0, JUMPER_Y1) for z in (JUMPER_Z0, JUMPER_Z1)]


def _jumper_polygon(deg):
    py, pz = CRADLE_PIVOT_PANEL
    ring = [(JUMPER_Y0, JUMPER_Z0), (JUMPER_Y0, JUMPER_Z1), (JUMPER_Y1, JUMPER_Z1), (JUMPER_Y1, JUMPER_Z0)]
    return [_rel(y - py, z - pz, deg) for y, z in ring]


def _max_y_in_band(poly, z_lo, z_hi):
    """Largest dY of a convex polygon clipped to z_lo <= dZ <= z_hi, or None."""
    pts = [p for p in poly if z_lo <= p[1] <= z_hi]
    n = len(poly)
    for i in range(n):
        (y0, z0), (y1, z1) = poly[i], poly[(i + 1) % n]
        for zc in (z_lo, z_hi):
            if (z0 - zc) * (z1 - zc) < 0:
                t = (zc - z0) / (z1 - z0)
                pts.append((y0 + t * (y1 - y0), zc))
    return max((p[0] for p in pts), default=None)


JUMPER_DROP = max(-min(_rel(dy, dz, a)[1] for dy, dz in _jumper_corners()) for a in INSERT_SWEEP_DEG)
JUMPER_FRONT_REACH = max(-min(_rel(dy, dz, a)[0] for dy, dz in _jumper_corners()) for a in INSERT_SWEEP_DEG)

# ---------------------------------------------------------------------------
# Pivot in the base frame
# ---------------------------------------------------------------------------
DOVETAIL_PLATE_FLANK_DEG = 60.0  # builder call, flank from horizontal, steeper than PLA_OVERHANG_DEG
DOVETAIL_PLATE_RUN = PLATE_T / math.tan(math.radians(DOVETAIL_PLATE_FLANK_DEG))  # 1.732
DOVETAIL_PLATE_FIT = FIT_SLIDE  # MAT 2.6, 0.20 to 0.25
DOVETAIL_PLATE_DETENT = FIT_DETENT  # PLAN Task 4, end detent 0.4
DOVETAIL_PLATE_BAND_T = math.ceil((DOVETAIL_PLATE_RUN + DOVETAIL_PLATE_FIT + SKIN) * 10.0) / 10.0  # 3.3, wall band at the rails, rounded up to 0.1

FRONT_WALL_T = DOVETAIL_PLATE_BAND_T  # builder call, full height so the rail band needs no step
FRONT_WALL_IN_Y = FRONT_WALL_T  # base front face at Y 0

CRADLE_PIVOT_Y = _ceil_half(FRONT_WALL_IN_Y + JUMPER_CLEAR + JUMPER_FRONT_REACH)  # 16.5
CRADLE_PIVOT_Z = _ceil_half(PLATE_TOP_Z + JUMPER_DROP + JUMPER_CLEAR)  # 32.5
CRADLE_PIVOT = (CRADLE_PIVOT_Y, CRADLE_PIVOT_Z)


def panel_point_to_base(x, y, z):
    """Panel frame point to base frame (X, Y, Z), pure Python, same map as panel_to_base()."""
    py, pz = CRADLE_PIVOT_PANEL
    dy, dz = _rel(y - py, z - pz, TILT_DEG)
    return x, CRADLE_PIVOT_Y + dy, CRADLE_PIVOT_Z + dz


def panel_y_at_base_z(z_panel, z_base):
    """Panel y where the line of panel depth z_panel crosses base height z_base."""
    py, pz = CRADLE_PIVOT_PANEL
    return py + (z_base - CRADLE_PIVOT_Z - (z_panel - pz) * _S) / _C


# ---------------------------------------------------------------------------
# Sensor pocket (DES Sensor pocket, Gaskets, Soft mounting)
# ---------------------------------------------------------------------------
POCKET_X0 = CRADLE_SIDE_IN_X0  # builder call, the left wall inner face, inlet end at the outer left
POCKET_L = SEN_MAX_L + 2 * SEN_SIDE_CLEAR  # 56.0
POCKET_X1 = POCKET_X0 + POCKET_L  # 55.65
SEN_X0 = POCKET_X0 + (POCKET_L - SEN_L) / 2  # 0.05, nominal sensor centred
POCKET_Z0 = PLATE_TOP_Z  # open bottom, closed by the bottom plate
POCKET_H = (SEN_Z0 - PLATE_TOP_Z) + SEN_MAX_W + SEN_SIDE_CLEAR  # 27.5
POCKET_TOP_Z = POCKET_Z0 + POCKET_H  # 32.5
POCKET_ROOF_T = WALL  # builder call, solid thermal roof (DES Thermal wall)
BASE_DECK_Z = POCKET_TOP_Z + POCKET_ROOF_T  # 34.3, base top behind the cradle
POCKET_D = ASSUMED_GASKET_COMPRESSED_T + SEN_MAX_H + ASSUMED_END_PAD_COMPRESSED_T  # 25.8
POCKET_MOUTH_CHAMFER = 1.0  # PLAN Review Focus 3, 1 mm lead in at the open bottom
POCKET_END_RIB_W = 7.0  # builder call, end pad ribs at both ends of the rim
POCKET_END_RIB_T = WALL  # builder call

# Keep-outs that set the pocket front (the end-rib face) in Y.
# (a) The cavity stays behind the cradle rear wall outer face at the cavity top.
_REAR_OUT_Y_AT_POCKET_TOP = panel_point_to_base(
    0.0, panel_y_at_base_z(CRADLE_REAR_OUT_Z, POCKET_TOP_Z), CRADLE_REAR_OUT_Z
)[1]
# (b) The jumper block stays in front of the pocket over the sweep.
_JUMPER_REAR_MAX = max(
    _max_y_in_band(_jumper_polygon(a), PLATE_TOP_Z - CRADLE_PIVOT_Z, POCKET_TOP_Z - CRADLE_PIVOT_Z) or -1e9
    for a in INSERT_SWEEP_DEG
)
# (c) The plug zone in front of the connector stays behind the jumper block.
PLUG_ZONE_Z0 = SEN_Z0 + SEN_CONN_Y0 - JUMPER_CLEAR
PLUG_ZONE_Z1 = SEN_Z0 + SEN_CONN_Y1 + JUMPER_CLEAR
_JUMPER_REAR_IN_PLUG_BAND = max(
    _max_y_in_band(_jumper_polygon(a), PLUG_ZONE_Z0 - CRADLE_PIVOT_Z, PLUG_ZONE_Z1 - CRADLE_PIVOT_Z) or -1e9
    for a in INSERT_SWEEP_DEG
)
POCKET_Y0 = _ceil_half(
    max(
        _REAR_OUT_Y_AT_POCKET_TOP,
        CRADLE_PIVOT_Y + _JUMPER_REAR_MAX + JUMPER_CLEAR,
        CRADLE_PIVOT_Y + _JUMPER_REAR_IN_PLUG_BAND + JUMPER_CLEAR + ASSUMED_PLUG_ZONE,
    )
)
POCKET_Y1 = POCKET_Y0 + POCKET_D  # back wall inner face
BACK_WALL_IN_Y = POCKET_Y1
BACK_WALL_T = WALL
BACK_WALL_OUT_Y = BACK_WALL_IN_Y + BACK_WALL_T
BACK_BAND_OUT_Y = BACK_WALL_IN_Y + DOVETAIL_PLATE_BAND_T  # rail band steps outward, no ledge in the pocket
SEN_FACE_Y = BACK_WALL_IN_Y - ASSUMED_GASKET_COMPRESSED_T  # opening face, gasket between it and the wall
SEN_RIM_Y = SEN_FACE_Y - SEN_H
PLUG_ZONE_Y0 = POCKET_Y0 - ASSUMED_PLUG_ZONE
BASE_D = BACK_BAND_OUT_Y  # body depth, front face at Y 0, without the fin

# ---------------------------------------------------------------------------
# Back wall grilles (DES Back wall grilles), vertical slots, sensor frame x, y
# ---------------------------------------------------------------------------
GRILLE_SLOT_W = 2.5  # builder call, slot width, under the 5 mm bridge at its top end
GRILLE_BAR_W = PLA_WALL_3  # builder call, 3 perimeters
GRILLE_INLET_N = 4  # builder call
GRILLE_INLET_X0 = SEN_INLET_SQ[0] - 0.9  # builder call, 2.3
GRILLE_INLET_Y0 = SEN_INLET_SQ[2] - 0.5  # builder call, 2.3
GRILLE_INLET_Y1 = SEN_INLET_ROUND[1] + SEN_INLET_ROUND[2] / 2 + 0.5  # builder call, 21.7
GRILLE_OUTLET_N = 6  # builder call
GRILLE_OUTLET_Y0 = SEN_OUTLET[1] - SEN_OUTLET[2] / 2 - 0.65  # builder call, 1.9
GRILLE_OUTLET_Y1 = SEN_OUTLET[1] + SEN_OUTLET[2] / 2 + 0.65  # builder call, 23.7
GRILLE_GASKET_BORDER = 1.5  # builder call, least gasket land between a grille and the sensor edge


def _grille_span(n):
    return n * GRILLE_SLOT_W + (n - 1) * GRILLE_BAR_W


GRILLE_INLET_X1 = GRILLE_INLET_X0 + _grille_span(GRILLE_INLET_N)  # 16.35
GRILLE_OUTLET_X0 = SEN_OUTLET[0] - _grille_span(GRILLE_OUTLET_N) / 2  # 31.525
GRILLE_OUTLET_X1 = SEN_OUTLET[0] + _grille_span(GRILLE_OUTLET_N) / 2  # 53.275
GRILLE_INLET_AREA = GRILLE_INLET_N * GRILLE_SLOT_W * (GRILLE_INLET_Y1 - GRILLE_INLET_Y0)
GRILLE_OUTLET_AREA = GRILLE_OUTLET_N * GRILLE_SLOT_W * (GRILLE_OUTLET_Y1 - GRILLE_OUTLET_Y0)


def grille_slots_base(kind):
    """Slots as (X0, X1, Z0, Z1) in the base frame, for 'inlet' or 'outlet'."""
    if kind == "inlet":
        n, x0, y0, y1 = GRILLE_INLET_N, GRILLE_INLET_X0, GRILLE_INLET_Y0, GRILLE_INLET_Y1
    else:
        n, x0, y0, y1 = GRILLE_OUTLET_N, GRILLE_OUTLET_X0, GRILLE_OUTLET_Y0, GRILLE_OUTLET_Y1
    pitch = GRILLE_SLOT_W + GRILLE_BAR_W
    return [
        (SEN_X0 + x0 + i * pitch, SEN_X0 + x0 + i * pitch + GRILLE_SLOT_W, SEN_Z0 + y0, SEN_Z0 + y1)
        for i in range(n)
    ]


# Fin between the grilles (Review Focus 4)
FIN_T = WALL  # builder call
FIN_X = SEN_X0 + (GRILLE_INLET_X1 + GRILLE_OUTLET_X0) / 2  # centre, base X
FIN_DEPTH = 10.0  # builder call, past the back wall outer face, PLAN at least 8
FIN_Z0 = BASE_Z0  # builder call, down to the bed so the fin prints with no support
FIN_Z1 = BASE_DECK_Z  # builder call

# Ambient vent slots in the bottom plate (DES Ambient vent), under the harness bay
VENT_X0 = PANEL_HEADER_X0  # builder call
VENT_X1 = PANEL_HEADER_X1  # builder call
VENT_Y0 = FRONT_WALL_IN_Y + 3.0  # builder call
VENT_Y1 = FRONT_WALL_IN_Y + 11.0  # builder call
VENT_MIN_GRILLE_DIST = 20.0  # PLAN Task 4, at least 20 from any grille


def _vent_to_grille_distance():
    best = 1e9
    for kind in ("inlet", "outlet"):
        for gx0, gx1, gz0, gz1 in grille_slots_base(kind):
            dx = max(0.0, gx0 - VENT_X1, VENT_X0 - gx1)
            dy = max(0.0, BACK_WALL_IN_Y - VENT_Y1)
            dz = max(0.0, gz0 - PLATE_TOP_Z)
            best = min(best, math.sqrt(dx * dx + dy * dy + dz * dz))
    return best


VENT_GRILLE_DIST = _vent_to_grille_distance()

# ---------------------------------------------------------------------------
# Bottom plate (DES Bottom plate)
# ---------------------------------------------------------------------------
DOVETAIL_PLATE_Y0 = FRONT_WALL_IN_Y  # nominal plate edge, the base socket adds the 0.2 fit per flank
DOVETAIL_PLATE_Y1 = BACK_WALL_IN_Y  # nominal, as above
DOVETAIL_PLATE_STOP_X = POCKET_X0  # builder call, end stop at the left wall, detent ridge snaps past the right wall
DOVETAIL_PLATE_EXIT_X = BASE_X1  # builder call, slides out toward +X
FOOT_INSET = 6.5  # builder call, foot radius + min wall + bed chamfer, rounded up to 0.5 (bottom_plate.py)
FOOT_CENTRES = tuple(
    (x, y)
    for x in (DOVETAIL_PLATE_STOP_X + FOOT_INSET, DOVETAIL_PLATE_EXIT_X - FOOT_INSET)
    for y in (DOVETAIL_PLATE_Y0 + FOOT_INSET, DOVETAIL_PLATE_Y1 - FOOT_INSET)
)

# ---------------------------------------------------------------------------
# Backrest and its dovetail (DES Backrest), panel frame for x and z
# ---------------------------------------------------------------------------
BACKREST_FRONT_Z = CRADLE_REAR_IN_Z  # spine front face 0.2 behind the back cover
DOVETAIL_BACKREST_FIT = FIT_SLIDE
DOVETAIL_BACKREST_DETENT = FIT_DETENT  # DES Backrest, small detent at the end of the dovetail
DOVETAIL_BACKREST_FLANK_DEG = 60.0  # builder call, flank from the wide face
DOVETAIL_BACKREST_DEPTH = 3.0  # builder call
DOVETAIL_BACKREST_ENGAGE = 20.0  # builder call, along the panel y direction below the deck
# Socket x: 1 skin clear of the pocket, and 1 skin clear of a d2.8 BOOT pin path.
# base_body.py cuts the pin channel at BUTTON_PIN_CHANNEL_D, so the real skin
# to the channel is 1.15, not SKIN. Accepted for iteration 1 (dfm passes),
# so the backrest tongue keeps its width.
BUTTON_PIN_CHANNEL_D = 3.2  # PLAN Task 3 Step 1, d3.2 BOOT and RST channels
_BOOT_PATH_X0 = PANEL_BOOT_CENTRE[0] - PANEL_BUTTON_HOLE_D / 2
DOVETAIL_BACKREST_SOCKET_X0 = POCKET_X1 + SKIN  # 57.0
DOVETAIL_BACKREST_SOCKET_X1 = _BOOT_PATH_X0 - SKIN  # 67.055
DOVETAIL_BACKREST_X0 = DOVETAIL_BACKREST_SOCKET_X0 + DOVETAIL_BACKREST_FIT
DOVETAIL_BACKREST_X1 = DOVETAIL_BACKREST_SOCKET_X1 - DOVETAIL_BACKREST_FIT
DOVETAIL_BACKREST_WIDE = DOVETAIL_BACKREST_X1 - DOVETAIL_BACKREST_X0
DOVETAIL_BACKREST_NARROW = DOVETAIL_BACKREST_WIDE - 2 * DOVETAIL_BACKREST_DEPTH / math.tan(
    math.radians(DOVETAIL_BACKREST_FLANK_DEG)
)
# Socket z: behind the cradle rear wall with a skin, so the BOOT and RST
# window in the rear wall never opens into it. Narrow face forward.
DOVETAIL_BACKREST_SOCKET_FRONT_Z = CRADLE_REAR_OUT_Z - SKIN  # -12.8
DOVETAIL_BACKREST_FRONT_Z = DOVETAIL_BACKREST_SOCKET_FRONT_Z - DOVETAIL_BACKREST_FIT  # -13.0
DOVETAIL_BACKREST_BACK_Z = DOVETAIL_BACKREST_FRONT_Z - DOVETAIL_BACKREST_DEPTH  # -16.0, wide face
BACKREST_SPINE_T = BACKREST_FRONT_Z - DOVETAIL_BACKREST_BACK_Z  # 8.2
BACKREST_LOWER_FRONT_Z = CRADLE_REAR_OUT_Z - FIT_SLIDE  # spine front below the rear wall top
DOVETAIL_BACKREST_DECK_Y = panel_y_at_base_z(DOVETAIL_BACKREST_FRONT_Z, BASE_DECK_Z)
DOVETAIL_BACKREST_BOTTOM_Y = DOVETAIL_BACKREST_DECK_Y - DOVETAIL_BACKREST_ENGAGE
BACKREST_TOP_Y = 97.0  # backrest.py HEAD_Y1, head top with the thumb tab
BACKREST_HEIGHT = BACKREST_TOP_Y - DOVETAIL_BACKREST_BOTTOM_Y  # length along the panel, printed flat

# ---------------------------------------------------------------------------
# Overall height
# ---------------------------------------------------------------------------
_SIDE_LIP_FRONT_TOP = panel_point_to_base(
    0.0, CRADLE_SIDE_LIP_TOP_PANEL_Y, CRADLE_FRONT_LIP_IN_Z + CRADLE_FRONT_LIP_T
)
_REAR_WALL_TOP = panel_point_to_base(0.0, CRADLE_REAR_WALL_TOP_PANEL_Y, CRADLE_REAR_IN_Z)
BASE_TOP_Z = max(_SIDE_LIP_FRONT_TOP[2], _REAR_WALL_TOP[2], BASE_DECK_Z)
BASE_H = BASE_TOP_Z - BASE_Z0  # body height, plate underside to the highest lip corner


# ---------------------------------------------------------------------------
# Locations (build123d imported here only)
# ---------------------------------------------------------------------------
def panel_to_base():
    """Panel frame to base frame: 75 degree turn about X (15 degree lean back), then the pivot."""
    import build123d as bd

    py, pz = CRADLE_PIVOT_PANEL
    ty = CRADLE_PIVOT_Y - (py * _S - pz * _C)
    tz = CRADLE_PIVOT_Z - (py * _C + pz * _S)
    return bd.Location((0.0, ty, tz), (90.0 - TILT_DEG, 0.0, 0.0))


def sensor_to_base():
    """Sensor sheet frame to base frame: x to +X, y (width) to +Z, opening face normal to +Y."""
    import build123d as bd

    return bd.Location((SEN_X0, SEN_FACE_Y, SEN_Z0), (90.0, 0.0, 0.0))


# ---------------------------------------------------------------------------
# Plan Task 1 Step 1 asserts (written first, then the constants above)
# ---------------------------------------------------------------------------
def check():
    results = {}

    def ok(name, cond, value):
        results[name] = value
        assert cond, f"{name}: {value}"

    ok("pocket L >= max L + side clearance", POCKET_L >= SEN_MAX_L + 2 * SEN_SIDE_CLEAR, POCKET_L)
    ok(
        "pocket H + seat >= max W + compressed pad",
        POCKET_H + PAD_SEAT_D >= SEN_MAX_W + (PAD_UNDER_T - PAD_UNDER_COMPRESSION),
        POCKET_H + PAD_SEAT_D,
    )
    ok(
        "pocket D >= max H + gasket + end pad",
        POCKET_D >= SEN_MAX_H + ASSUMED_GASKET_COMPRESSED_T + ASSUMED_END_PAD_COMPRESSED_T,
        POCKET_D,
    )
    # The rigid gasket stops set the gasket squeeze only while the end pads
    # push the sensor onto them. With the sensor on the stops the pads get
    # POCKET_D - ASSUMED_GASKET_COMPRESSED_T - sensor height. At the largest
    # sensor that is 1.0 of squeeze, at the smallest it is 0.0, so a small
    # sensor rests on the stops with no preload and the gasket can lift it
    # off them (DESIGN.md Open items, settled on the test plate). The checks
    # below keep that from getting worse. The pads must still touch a sensor
    # on the stops, and both foams together must still hold it.
    sen_min_h = 2 * SEN_H - SEN_MAX_H
    pad_squeeze_min = ASSUMED_END_PAD_T - (POCKET_D - ASSUMED_GASKET_COMPRESSED_T - sen_min_h)
    ok("end pads touch the smallest sensor on the stops", pad_squeeze_min >= -1e-9, pad_squeeze_min)
    ok(
        "gasket and pads together squeeze the smallest sensor",
        ASSUMED_GASKET_T + ASSUMED_END_PAD_T - (POCKET_D - sen_min_h) >= 0.5,
        ASSUMED_GASKET_T + ASSUMED_END_PAD_T - (POCKET_D - sen_min_h),
    )
    ok("front lip top panel y < 8.1", CRADLE_FRONT_LIP_TOP_PANEL_Y < PANEL_ACTIVE_Y0, CRADLE_FRONT_LIP_TOP_PANEL_Y)
    ok("side lip top panel y < 15", CRADLE_SIDE_LIP_TOP_PANEL_Y < PANEL_SIDE_LIP_MAX_Y, CRADLE_SIDE_LIP_TOP_PANEL_Y)
    ok(
        "header slot spans x 19 to 48",
        CRADLE_HEADER_SLOT_X0 <= 19.0 and CRADLE_HEADER_SLOT_X1 >= 48.0,
        (CRADLE_HEADER_SLOT_X0, CRADLE_HEADER_SLOT_X1),
    )
    ok("inlet grille >= 112 mm2", GRILLE_INLET_AREA >= 2 * SEN_MIN_INLET_AREA, GRILLE_INLET_AREA)
    ok("outlet grille >= 300 mm2", GRILLE_OUTLET_AREA >= 300.0, GRILLE_OUTLET_AREA)
    ok("pocket right end x < 58", POCKET_X1 < PANEL_HEAT_X0 - 10.0, POCKET_X1)
    ok("BASE_W <= 246", BASE_W <= PRINT_BED, BASE_W)
    ok("BASE_D <= 246", BASE_D <= PRINT_BED, BASE_D)
    ok("backrest height <= 246", BACKREST_HEIGHT <= PRINT_BED, BACKREST_HEIGHT)

    # Supporting checks for Tasks 2 to 5.
    ok("latch tapered strain <= 1.2", latch_strain_tapered(LATCH_L) <= PLA_STRAIN_LIMIT_XY, latch_strain_tapered(LATCH_L))
    ok(
        "latch constant-section strain <= 1.2",
        latch_strain_constant(LATCH_L) <= PLA_STRAIN_LIMIT_XY,
        latch_strain_constant(LATCH_L),
    )
    ok("latch catch <= 1.2", LATCH_CATCH <= 1.2, LATCH_CATCH)
    ok("fin >= 8", FIN_DEPTH >= 8.0, FIN_DEPTH)
    ok(
        "fin between grilles, both faces",
        GRILLE_INLET_X1 < FIN_X - SEN_X0 - FIN_T / 2 and FIN_X - SEN_X0 + FIN_T / 2 < GRILLE_OUTLET_X0,
        FIN_X,
    )
    ok("vents >= 20 from grilles", VENT_GRILLE_DIST >= VENT_MIN_GRILLE_DIST, VENT_GRILLE_DIST)
    ok(
        "grilles inside the sensor face with gasket land",
        min(GRILLE_INLET_X0, GRILLE_INLET_Y0, GRILLE_OUTLET_Y0, SEN_L - GRILLE_OUTLET_X1, SEN_W - GRILLE_OUTLET_Y1)
        >= GRILLE_GASKET_BORDER,
        min(GRILLE_INLET_X0, GRILLE_INLET_Y0, GRILLE_OUTLET_Y0, SEN_L - GRILLE_OUTLET_X1, SEN_W - GRILLE_OUTLET_Y1),
    )
    ok("inlet and outlet grilles split by the outlet edge gap", GRILLE_INLET_X1 < GRILLE_OUTLET_X0, GRILLE_OUTLET_X0 - GRILLE_INLET_X1)
    ok("pad compression 1.0", abs(PAD_UNDER_COMPRESSION - 1.0) < 1e-9, PAD_UNDER_COMPRESSION)
    ok("sensor top below pocket top", SEN_Z0 + SEN_MAX_W <= POCKET_TOP_Z, POCKET_TOP_Z - SEN_Z0 - SEN_MAX_W)
    ok("rear wall skin behind screw relief >= 1.35", CRADLE_SCREW_RELIEF_BOTTOM_Z - CRADLE_REAR_OUT_Z >= SKIN - 1e-9, CRADLE_SCREW_RELIEF_BOTTOM_Z - CRADLE_REAR_OUT_Z)
    ok("pocket behind rear wall", POCKET_Y0 >= _REAR_OUT_Y_AT_POCKET_TOP, POCKET_Y0 - _REAR_OUT_Y_AT_POCKET_TOP)
    ok(
        "jumper clear of front wall over sweep",
        CRADLE_PIVOT_Y - JUMPER_FRONT_REACH >= FRONT_WALL_IN_Y + JUMPER_CLEAR,
        CRADLE_PIVOT_Y - JUMPER_FRONT_REACH - FRONT_WALL_IN_Y,
    )
    ok(
        "jumper clear of plate over sweep",
        CRADLE_PIVOT_Z - JUMPER_DROP >= PLATE_TOP_Z + JUMPER_CLEAR,
        CRADLE_PIVOT_Z - JUMPER_DROP - PLATE_TOP_Z,
    )
    ok(
        "plug zone behind jumper block over sweep",
        PLUG_ZONE_Y0 >= CRADLE_PIVOT_Y + _JUMPER_REAR_IN_PLUG_BAND + JUMPER_CLEAR,
        PLUG_ZONE_Y0 - CRADLE_PIVOT_Y - _JUMPER_REAR_IN_PLUG_BAND,
    )
    ok("right end rib clear of jumpers", POCKET_X1 - POCKET_END_RIB_W >= JUMPER_X1 + JUMPER_CLEAR, POCKET_X1 - POCKET_END_RIB_W - JUMPER_X1)
    ok("left end rib clear of connector", POCKET_X0 + POCKET_END_RIB_W <= SEN_X0 + SEN_CONN_X0 - JUMPER_CLEAR, SEN_X0 + SEN_CONN_X0 - POCKET_X0 - POCKET_END_RIB_W)
    ok("plate rail band skin >= 1.35", DOVETAIL_PLATE_BAND_T - DOVETAIL_PLATE_RUN - DOVETAIL_PLATE_FIT >= SKIN - 1e-9, DOVETAIL_PLATE_BAND_T - DOVETAIL_PLATE_RUN - DOVETAIL_PLATE_FIT)
    # The groove run is short, but the plate slides under the whole footprint,
    # so the base ceilings over it span 37 to 67 mm and print on supports from
    # the bed (Task 3 dfm). This check covers only the groove run.
    ok("rail groove run <= 5", DOVETAIL_PLATE_RUN + DOVETAIL_PLATE_FIT <= PLA_MAX_BRIDGE, DOVETAIL_PLATE_RUN + DOVETAIL_PLATE_FIT)
    ok("backrest socket clear of pocket", DOVETAIL_BACKREST_SOCKET_X0 - POCKET_X1 >= SKIN - 1e-9, DOVETAIL_BACKREST_SOCKET_X0 - POCKET_X1)
    ok("backrest tongue under the spine", PANEL_BACKREST_X0 <= DOVETAIL_BACKREST_X0 and DOVETAIL_BACKREST_X1 <= PANEL_BACKREST_X1, (DOVETAIL_BACKREST_X0, DOVETAIL_BACKREST_X1))
    ok("backrest spine left of heat zone", PANEL_BACKREST_X1 < PANEL_HEAT_X0, (PANEL_BACKREST_X1, PANEL_HEAT_X0))
    ok("backrest spine clear of TF slot", PANEL_BACKREST_X0 > PANEL_TF_X1, (PANEL_BACKREST_X0, PANEL_TF_X1))
    ok("backrest tongue left of heat zone", DOVETAIL_BACKREST_SOCKET_X1 < PANEL_HEAT_X0, DOVETAIL_BACKREST_SOCKET_X1)
    ok("backrest dovetail narrow >= 5", DOVETAIL_BACKREST_NARROW >= 5.0, DOVETAIL_BACKREST_NARROW)
    ok("backrest socket bottom above plate", panel_point_to_base(0, DOVETAIL_BACKREST_BOTTOM_Y, DOVETAIL_BACKREST_BACK_Z)[2] > PLATE_TOP_Z + SKIN, panel_point_to_base(0, DOVETAIL_BACKREST_BOTTOM_Y, DOVETAIL_BACKREST_BACK_Z)[2])
    ok("feet inside the plate", all(DOVETAIL_PLATE_Y0 + ASSUMED_FOOT_D / 2 <= y <= DOVETAIL_PLATE_Y1 - ASSUMED_FOOT_D / 2 for _, y in FOOT_CENTRES), FOOT_CENTRES)
    return results


if __name__ == "__main__":
    for key, value in check().items():
        print(f"PASS  {key}: {value}")
    print()
    for name in (
        "BASE_X0", "BASE_X1", "BASE_W", "BASE_D", "BASE_H", "BASE_TOP_Z", "BASE_Z0", "PLATE_TOP_Z",
        "BASE_DECK_Z", "CRADLE_PIVOT", "JUMPER_DROP", "JUMPER_FRONT_REACH", "POCKET_X0", "POCKET_X1",
        "POCKET_Y0", "POCKET_Y1", "POCKET_Z0", "POCKET_TOP_Z", "SEN_X0", "SEN_Z0", "SEN_FACE_Y",
        "SEN_RIM_Y", "PLUG_ZONE_Y0", "GRILLE_INLET_AREA", "GRILLE_OUTLET_AREA", "FIN_X",
        "BACK_WALL_OUT_Y", "BACK_BAND_OUT_Y", "VENT_GRILLE_DIST", "DOVETAIL_BACKREST_X0",
        "DOVETAIL_BACKREST_X1", "DOVETAIL_BACKREST_WIDE", "DOVETAIL_BACKREST_NARROW",
        "DOVETAIL_BACKREST_DECK_Y", "DOVETAIL_BACKREST_BOTTOM_Y", "BACKREST_HEIGHT",
        "DOVETAIL_PLATE_RUN", "DOVETAIL_PLATE_BAND_T", "FOOT_CENTRES",
    ):
        value = globals()[name]
        if isinstance(value, float):
            value = round(value, 3)
        print(f"{name} = {value}")
    print("_REAR_OUT_Y_AT_POCKET_TOP =", round(_REAR_OUT_Y_AT_POCKET_TOP, 3))
    print("_JUMPER_REAR_MAX =", round(_JUMPER_REAR_MAX, 3), "_JUMPER_REAR_IN_PLUG_BAND =", round(_JUMPER_REAR_IN_PLUG_BAND, 3))
    for length in LATCH_L_OPTIONS:
        print(f"latch l {length}: tapered {latch_strain_tapered(length):.3f} %, constant {latch_strain_constant(length):.3f} %")
    print("all asserts pass")
