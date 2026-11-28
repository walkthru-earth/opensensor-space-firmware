# SPDX-License-Identifier: CC-BY-4.0. Copyright Walkthru.Earth.
"""Shared numbers for the Waveshare ESP32-S3-ePaper-3.97 and SEN63C desk stand.

All lengths mm, angles degrees. Pure Python, `pixi run -e cad python -I params.py`
runs the asserts. build123d is imported only inside the Location helpers.

Source tags
  BRD   hardware/parts/boards/waveshare-epaper-397.md (section)
  SEN   hardware/parts/sensors/sen63c.md (section, row)
  MAT   .claude/skills/hardware-design/materials-and-closures.md (section)
  CADP  text-to-cad cad skill, references/manufacturing-printing.md
  DES   DESIGN.md in this folder (section)
  REF   the MakerWorld SEN66 stand, measured from its STEP (DES Shape)
  ASSUMED   no source, named ASSUMED_* so one line changes it
  call  a layout choice made from the sourced numbers, says what it follows

Frames
  Board frame (BRD Frame): the vendor STEP as it is. bx along the PCB 0 to 99.5,
    by up the PCB 0 to 60, bz out of the display. PCB front face bz 0.
  Case frame (DES Frames): X across seen from the front, Y from front to back,
    Z up, Z 0 under the shell floor. Board x equals case X.
    case = (bx, by sin T - bz cos T, OZ + by cos T + bz sin T), T = TILT_DEG.
  Sensor frame (SEN Coordinate convention): x along 55.2 from the inlet end,
    y across 25.6, z from the opening face (0) to the rim (21.3).
"""

import math

# ---------------------------------------------------------------------------
# Board, from the board sheet (BRD), board frame
# ---------------------------------------------------------------------------
PCB_W = 99.5  # BRD Outline, X 0 to 99.5
PCB_H = 60.0  # BRD Outline, Y 0 to 60
PCB_T = 1.6  # BRD Outline, Z -1.6 to 0
DISP_X = (0.32, 98.32)  # BRD Display, module bbox X
DISP_Y = (1.99, 58.23)  # BRD Display, module bbox Y
DISP_FRONT_Z = 1.35  # BRD Display, glass top face Z 1.35
ACTIVE_X = (9.72, 96.12)  # BRD Display, active face X, 86.40 wide
ACTIVE_Y = (4.19, 56.03)  # BRD Display, active face Y, 51.84 tall
STUDS = ((2.7, 2.7), (96.8, 2.7), (2.7, 57.3), (96.8, 57.3))  # BRD Studs, M2.5 x 4 centres
STUD_D = 4.5  # BRD Studs, outer diameter
STUD_END_Z = -5.6  # BRD Studs, back end, also the deepest board point
USB_Y = (25.21, 34.79)  # BRD USB-C, receptacle Y
USB_Z = (-4.85, -0.69)  # BRD USB-C, receptacle Z
TF_Y = (6.28, 22.38)  # BRD TF slot, Y
TF_Z = (-3.45, -1.0)  # BRD TF slot, Z
KEYS_X = (71.2, 86.7)  # BRD Keys, BOOT and PWR, X 71.2 to 76.7 and 81.2 to 86.7
KEYS_TIP_Y = -0.69  # BRD Keys, plungers point -Y to Y -0.69
KEYS_Z = (-3.4, -1.2)  # BRD Keys
ROCKER_X = (72.03, 86.03)  # BRD Rocker, X
ROCKER_TOP_Y = 62.41  # BRD Rocker, lever end Y
ROCKER_Z = (-4.2, -1.6)  # BRD Rocker
ESP_X = (74.73, 92.33)  # BRD Heat, ESP32-S3 shield X
ESP_Y = (22.1, 37.9)  # BRD Heat, shield Y
ANT_X = (93.1, 98.8)  # BRD Antenna, PCB antenna X

# ---------------------------------------------------------------------------
# Sensor (SEN)
# ---------------------------------------------------------------------------
SEN_L = 55.2  # SEN 1, length
SEN_W = 25.6  # SEN 1, width
SEN_H = 21.3  # SEN 1, height overall, ±0.5
SEN_MAX_L = 55.4  # SEN 1, maximum material
SEN_MAX_W = 25.7  # SEN 1
SEN_MAX_H = 21.8  # SEN 1
SEN_SIDE_CLEAR = 0.3  # SEN 7 rule 12
SEN_INLET_SQ = (3.2, 10.2, 2.8, 9.8)  # SEN 2, square inlet x0 x1 y0 y1
SEN_INLET_ROUND = (6.7, 18.2, 6.0)  # SEN 2, round inlet x y d
SEN_OUTLET = (42.4, 12.8, 20.5)  # SEN 2, outlet x y d
SEN_MIN_INLET_AREA = 56.0  # SEN 2
SEN_MIN_OUTLET_AREA = 148.0  # SEN 2
SEN_CONN = (17.8, 28.5, 16.9, 21.15)  # SEN 4, connector x0 x1 y0 y1 on the rim side
SEN_MASS_MAX_G = 22.0  # SEN 1
SEN_STEP_ORIGIN = (27.6, 12.8, 21.3)  # SEN Coordinate convention

# ---------------------------------------------------------------------------
# Material, process and fits (MAT, CADP)
# ---------------------------------------------------------------------------
PLA_E_XY_MPA = 3259.39  # MAT 1.1
PLA_ELONG_XY = 4.14  # MAT 1.1, percent
PLA_STRAIN_LIMIT_XY = 1.2  # MAT 1.2, percent
PLA_MIN_WALL = 0.8  # CADP Walls
PLA_MAX_BRIDGE = 5.0  # CADP Overhangs
PRINT_GAP = 0.8  # call, in-part gap that prints open, as the CrowPanel backrest
FIT_SLIDE = 0.20  # MAT 1.4
FIT_COVER = 0.30  # MAT 1.4, loose fit low end
FIT_LATCH_GAP = 0.4  # MAT 6.1
PRINT_BED = 246.0  # MAT 1.3, inside the A1 256 cube with margin

LATCH_H_ROOT = 1.8  # MAT 6.1
LATCH_H_TIP = 0.9  # MAT 6.1
LATCH_B = 6.0  # MAT 6.1
LATCH_CATCH = 1.0  # MAT 6.1
LATCH_L = 18.0  # MAT 6.1, l 15 to 20, the CrowPanel 18 mm arm
LATCH_LEAD_DEG = 30.0  # MAT 6.1
LATCH_RETURN_DEG = 45.0  # MAT 6.1


def latch_strain_tapered(length, catch=LATCH_CATCH, h_root=LATCH_H_ROOT):
    """Root strain percent, Bayer tapered arm, MAT 4.1."""
    return 100.0 * catch * h_root / (1.09 * length**2)


def latch_strain_constant(length, catch, h):
    """Root strain percent, constant section, MAT 2.1, eps = 1.5 h y / L^2."""
    return 100.0 * 1.5 * h * catch / length**2


# ---------------------------------------------------------------------------
# Assumed, one per row of the DESIGN.md table
# ---------------------------------------------------------------------------
ASSUMED_GASKET_T = 3.0  # ASSUMED, closed-cell foam sheet for gasket and pads
ASSUMED_GASKET_COMPRESSED_T = 2.0  # ASSUMED, set by the rigid standoffs
ASSUMED_PAD_COMPRESSED_T = 1.5  # ASSUMED, pads 3.0 to 1.5 at the nominal sensor
ASSUMED_STRIP_T = 1.0  # ASSUMED, foam strip under the sensor
ASSUMED_PLUG_ZONE = 10.0  # ASSUMED, JST GH plug and bend in front of the rim
ASSUMED_FOOT_D = 10.0  # ASSUMED, stick-on rubber foot
ASSUMED_FOOT_H = 3.0  # ASSUMED
ASSUMED_BOARD_MASS_G = 60.0  # ASSUMED, not in source, weigh it
# Bambu Studio 02.09.02.62 slice, 2026-11-26, A1 0.4, 0.16 mm, 3 walls, 15 % gyroid, Generic PLA.
# Weigh the printed parts and replace these.
SLICE_MASS_G = {"shell": 68.00, "bezel": 8.25, "sensor_bar": 1.44}

# ---------------------------------------------------------------------------
# Case layout
# ---------------------------------------------------------------------------
TILT_DEG = 15.0  # REF, front face normal (0, -0.966, 0.259)
S = math.sin(math.radians(TILT_DEG))
C = math.cos(math.radians(TILT_DEG))
T = math.tan(math.radians(TILT_DEG))
WALL = 2.0  # REF back wall 2.0, call for floor, sides and top
FLOOR_TOP = WALL  # floor Z 0 to 2
FRONT_BZ = 3.0  # call, bezel front face and shell front plane, bz 3.0
BEZEL_BACK_BZ = 0.1  # call, 0.1 off the PCB front so the bezel never loads it
CHIN = 11.0  # call, PCB bottom edge this far above the floor top, room under the keys
OZ = FLOOR_TOP + CHIN  # case Z of the board origin
IN_X0 = -0.5  # call, 0.5 left of the PCB edge
IN_X1 = 106.4  # call, centres the active window, (ACTIVE_X centre 52.92 - IN_X0) x 2
OUT_X0 = IN_X0 - WALL
OUT_X1 = IN_X1 + WALL
TOP_GAP = 3.9  # call, top wall inner above the PCB top front corner, room for the latch arm
Z_TI = OZ + PCB_H * C + TOP_GAP  # top wall inner
Z_TO = Z_TI + WALL  # top outer
Y_BACK_IN = 44.0  # call, sensor stack plus plug zone clear of the board back (assert below)
Y_BACK_OUT = Y_BACK_IN + WALL


def b2c(bx, by, bz):
    """Board frame point to case frame point."""
    return (bx, by * S - bz * C, OZ + by * C + bz * S)


def y_at(z, bz):
    """Case Y of the plane bz at height z."""
    return (z - OZ) * T - bz / C


def y_front(z):
    """Case Y of the shell front plane at height z."""
    return y_at(z, FRONT_BZ)


def board_to_case():
    import build123d as bd

    return bd.Location(bd.Plane(origin=(0.0, 0.0, OZ), x_dir=(1, 0, 0), z_dir=(0, -C, S)))


# ---------------------------------------------------------------------------
# Board seat, four cups behind the studs on brackets from the side walls
# ---------------------------------------------------------------------------
CUP_GAP = 0.1  # call, stud end to cup floor
CUP_ID = STUD_D + 0.5  # call, 0.25 radial play, the studs locate the board
CUP_OD = 8.0  # call, 1.5 ring
CUP_DEPTH = 1.0  # call, ring height over the stud end
CUP_FLOOR_BZ = STUD_END_Z - CUP_GAP  # -5.7
BRACKET_T = 3.0  # call
BRACKET_BACK_BZ = CUP_FLOOR_BZ - BRACKET_T  # -8.7

# ---------------------------------------------------------------------------
# Left wall notches, board frame
# ---------------------------------------------------------------------------
TF_NOTCH_Y = (7.8, 21.0)  # call, card 11 wide plus finger room inside TF_Y
TF_NOTCH_Z0 = -4.5  # call, 1.05 behind the socket
USB_NOTCH_Y = (23.0, 37.0)  # call, USB-C overmold 12.5 plus 0.75 each side
USB_NOTCH_Z0 = -6.52  # call, overmold 6.5 about the receptacle centre, plus 0.5
USB_OVERMOLD_Z1 = 0.48  # call, overmold front at -2.77 + 3.25
ROCKER_NOTCH_X = (68.0, 90.0)  # call, rocker 14 plus 4 each side for a finger
ROCKER_NOTCH_BZ0 = -7.0  # call, notch runs back to bz -7

# ---------------------------------------------------------------------------
# Bezel
# ---------------------------------------------------------------------------
BZ_X0 = IN_X0 + FIT_COVER
BZ_X1 = IN_X1 - FIT_COVER
BZ_TOP_GAP = 0.3  # call, bezel top below the top wall inner
WINDOW_MARGIN = 0.25  # call, window 0.25 outside the active area
WINDOW_CHAMFER = 0.75  # call, leaves a 0.6 lip (3 layers) between the chamfer and the pocket ceiling
POCKET_CLEAR = 0.3  # call, display clearance in the bezel back pocket
POCKET_BZ1 = DISP_FRONT_Z + POCKET_CLEAR  # 1.65
KEY_WINDOW_X = (68.0, 90.0)  # call, BOOT and PWR plus a finger
KEY_WINDOW_Y = (-8.0, 1.5)  # call, below the PCB edge, under the display edge 1.99

# Top latch, in-plane arm along X, tooth into a window in the top wall
ARM_ROOT_X = 62.0  # call, clear of the rocker notch
ARM_TIP_X = ARM_ROOT_X - LATCH_L  # 44
ARM_Z1 = Z_TI - BZ_TOP_GAP
ARM_SLOT = 1.5  # call, more than the 1.0 tip travel
ARM_B = 7.0  # call, arm depth in Y, above LATCH_B, holds the whole tooth
TOOTH_X = (ARM_TIP_X + 0.4, ARM_TIP_X + 4.4)  # call
TOOTH_H = LATCH_CATCH + BZ_TOP_GAP  # 1.3, 1.0 overlap above the top wall inner
TOOTH_GAP = 0.2  # call
TOOTH_LAND = 0.5  # call
TOOTH_RETURN_DEG = 30.0  # call, from case Y. The bezel prints face down, print up is case Y tilted 15 deg, so 30 here is 45 from vertical in print. MAT 6.1 says 45, the push-off force is still about 1.06 x the 3.2 N deflection force (DES Latch)
STRIP = 2.0  # call, top wall left in front of the tooth window
LATCH_WIN_X = (TOOTH_X[0] - 0.4, TOOTH_X[1] + 0.4)

# Bottom tabs into floor slots
TAB_XS = ((10.0, 14.4), (92.0, 96.4))  # call, 4.4 wide so the floor slots bridge under 5
TAB_BZ = (-2.5, 1.0)  # call
TAB_DROP = 1.8  # call, tab length below the bezel bottom, inside the 2.0 floor
SLOT_FRONT_GAP = 0.2  # call
SLOT_BACK_GAP = 1.5  # call, room for the bezel to tilt in

# ---------------------------------------------------------------------------
# Sensor bay
# ---------------------------------------------------------------------------
SEN_X0 = 17.0  # call, inlet end left, away from the ESP32 on the right
SEN_Z0 = FLOOR_TOP + ASSUMED_STRIP_T  # 3.0
SEN_FACE_Y = Y_BACK_IN - ASSUMED_GASKET_COMPRESSED_T  # opening face
SEN_RIM_Y = SEN_FACE_Y - SEN_H
POCKET_X = (SEN_X0 - SEN_SIDE_CLEAR, SEN_X0 + SEN_MAX_L + SEN_SIDE_CLEAR)
POCKET_Z1 = SEN_Z0 + SEN_MAX_W + SEN_SIDE_CLEAR
RIB_T = 1.6  # call
RIB_DEPTH = 8.0  # call, ribs reach 6 past the face
STANDOFFS = ((0.2, 2.2, 0.2, 2.2), (0.2, 2.2, 23.4, 25.4), (53.0, 55.0, 0.2, 2.2), (53.0, 55.0, 23.4, 25.4))  # call, sensor x0 x1 y0 y1
GRILLE_GROW = 0.75  # call, each grille 0.75 larger than its sensor opening per side

BAR_W = 4.3  # call, Z, so the wall hole (4.3 + 0.6) bridges under 5
BAR_T = 2.5  # call, Y
BAR_Z0 = SEN_Z0 + 4.0  # call, sensor y 4.0 to 8.3, clear of the connector
BAR_Z1 = BAR_Z0 + BAR_W
BAR_BACK_Y = SEN_RIM_Y - ASSUMED_PAD_COMPRESSED_T
BAR_FRONT_Y = BAR_BACK_Y - BAR_T  # rigid stop face
HOLE_CLEAR = 0.2  # call, post hole around the bar, Z each side
HOLE_T = 2.9  # call, post hole in Y, 0.4 behind the bar
WALL_HOLE_GROW = 0.1  # call, wall holes 0.1 larger than the post holes
POST_T = 4.0  # call
POST_FRONT_Y = BAR_FRONT_Y - 2.2  # call
POST_Z = (FLOOR_TOP, BAR_Z1 + 2.5)
PADS = ((1.5, 13.0), (42.0, 53.7))  # call, sensor x under the bar
PRONG_L = 10.0  # call
PRONG_H = 1.5  # call
PRONG_CATCH = 0.6  # call
HEAD = (1.5, 7.5)  # call, head X length and Z height
BAR_HEAD_X0 = OUT_X1 + 0.1  # call, head 0.1 off the right wall outer face
BAR_BARB_X = OUT_X0 - 0.1  # call, barb return face 0.1 past the left wall outer face
PRONG_TIP_X = BAR_BARB_X - 2.0  # call, 2.0 of barb lead outside the wall
PRONG_ROOT_X = PRONG_TIP_X + PRONG_L
POST_XS = ((POCKET_X[0] - RIB_T - POST_T, POCKET_X[0] - RIB_T), (POCKET_X[1] + RIB_T, POCKET_X[1] + RIB_T + POST_T))
TAB_BOSS_Z1 = FLOOR_TOP + 4.0  # call, boss behind the bezel that carries each tab, under the PCB at Z 13
LATCH_WIN_Y = (y_front(Z_TI) + STRIP, y_front(Z_TI) + STRIP + 4.6)  # call, the tooth lead crosses Z_TI at about Ys + 4.16

# ---------------------------------------------------------------------------
# Battery shelf, ribs off the back wall behind the board (DES Battery shelf)
# ---------------------------------------------------------------------------
BAT_L = 61.5  # owner measured 2026-11-26, the pack along bx, the lead leaves a 41 edge
BAT_W = 41.0  # owner measured 2026-11-26, the pack along by
BAT_T = 4.0  # owner measured 2026-11-26, the pack along bz
BAT_LEAD_L = 55.0  # owner measured 2026-11-26, pack edge to the plug
ASSUMED_BAT_MASS_G = 28.0  # ASSUMED, a typical 1500 mAh pouch cell, weigh it
ASSUMED_RTC_MASS_G = 3.0  # ASSUMED, a 2032 coin with tabs and lead
ASSUMED_RTC_D = 20.0  # ASSUMED, ML2032 or LIR2032 rechargeable coin with tabs
ASSUMED_RTC_T = 3.2  # ASSUMED, the coin with its welded tabs
RTC_LEAD_MIN_L = 80.0  # call, the shortest coin lead to buy, rtc_lead_path below needs about 65
MX125_PLUG = (4.25, 3.3)  # BRD Back side parts, header body width and depth, the plug matches it
HDR_X = {"bat": (31.18, 35.43), "rtc": (42.68, 46.93), "spk": (54.18, 58.43)}  # BRD Headers and battery
HDR_TOP_Y = 7.13  # BRD Back side parts, right angle, the plug goes in along -by
HDR_BZ = -5.0  # BRD Back side parts
HDR_MID_BZ = (HDR_BZ - PCB_T) / 2  # -3.3
PLUG_RUN = 5.0  # call, plug body and wire bend past the header top
ESP_BZ = -4.7  # BRD Back side parts, shield can back
BOARD_BACK_BZ = -5.0  # BRD Back side parts, deepest board part inside the pack outline, the headers
LEAD_GAP = 4.1  # call, the pack lead runs between the board parts and the pack front
BAT_PLAY = 0.5  # call, per axis
PACK_FRONT_BZ = BOARD_BACK_BZ - LEAD_GAP  # -9.1
RIB_BZ = PACK_FRONT_BZ - BAT_T - BAT_PLAY  # -13.6, rib fronts, the pack back rests here
SH_X0 = 12.7  # call, puts the right fence in the middle of the back vent gap at 75.5, 4.4 right of the left bracket tips so the lead turns forward there
SH_X1 = SH_X0 + BAT_L + BAT_PLAY  # 74.7
SHELF_RIB_XC = (15.0, 30.5, 48.5, 66.5)  # call, in the back vent gaps (30.5 + 4.5 k) or left of the vents, 1.5 off the left fence
LEFT_FENCE_X = (SH_X0 - RIB_T, SH_X0)
RIGHT_FENCE_X = (SH_X1, SH_X1 + RIB_T)
RIB_Z = (POCKET_Z1 + 0.6, 72.0)  # call, rib bottom 0.6 over the sensor pocket, top 2.9 under the top wall
BY_SEAT = 23.0  # call, pack bottom edge, the pack top then sits 1.9 under the top wall
BY_TOP = BY_SEAT + BAT_W + BAT_PLAY  # 64.5
FOOT_T = 2.0  # call, feet under the pack bottom edge
FOOT_FRONT_BZ = BOARD_BACK_BZ - 2.9  # -7.9, the pack stays on the feet when it leans forward onto the board parts
FENCE_FRONT_BZ = PACK_FRONT_BZ - 0.5  # -9.6
LEFT_FENCE_BY0 = BY_TOP - 12.0  # call, top 12 only, the pack lead leaves the left edge lower down
LEDGE_INTO_RIB = 0.3  # call, feet and fences run this far into their rib

# Coin slot right of the pack, behind the ESP32 can, on the bay right rib
COIN_SLOT_X = (RIGHT_FENCE_X[1], RIGHT_FENCE_X[1] + ASSUMED_RTC_T + 0.8)  # 0.8 play across
COIN_RIB_X = (COIN_SLOT_X[1], COIN_SLOT_X[1] + RIB_T)
COIN_BLOCK_X0 = POCKET_X[1] + RIB_T  # 74.3, merges into the bay right rib
COIN_FLOOR_Z = (POST_Z[1] + 1.2, POST_Z[1] + 2.8)  # call, 1.2 over the right bar post
COIN_STOP_Y = (19.4, 21.0)  # call, front stop 1.6 thick, slot 23 long for the 20 coin
COIN_STOP_H = 8.0  # call, the coin drops in from above and the stop holds its lower half
COIN_BLOCK_Z1 = COIN_FLOOR_Z[1] + ASSUMED_RTC_D + 1.4  # 38.0


def bat_lead_path(lead_by):
    """Pack lead length, from the pack left edge at height lead_by, forward into
    the gap in front of the pack, straight across to the BAT header and into it, mm."""
    hx = sum(HDR_X["bat"]) / 2
    run_bz = (PACK_FRONT_BZ + BOARD_BACK_BZ) / 2
    mid_bz = RIB_BZ + BAT_PLAY / 2 + BAT_T / 2
    return (1.0 + abs(mid_bz - run_bz) + math.hypot(hx - SH_X0, lead_by - HDR_TOP_Y - PLUG_RUN)
            + abs(run_bz - HDR_MID_BZ) + PLUG_RUN)


def rtc_lead_path():
    """Coin lead length, from the far tab over the front stop, down to the board
    and across under the pack to the RTC header, Manhattan, mm."""
    hx = sum(HDR_X["rtc"]) / 2
    _, hy, hz = b2c(hx, HDR_TOP_Y + PLUG_RUN, HDR_MID_BZ)
    ex, ey, ez = sum(COIN_SLOT_X) / 2, COIN_STOP_Y[0], COIN_FLOOR_Z[1] + COIN_STOP_H
    tab = ASSUMED_RTC_D / 2 + 3.0  # far tab round to the stop
    return tab + abs(ex - hx) + abs(ey - hy) + abs(ez - hz) + PLUG_RUN


# ---------------------------------------------------------------------------
# Vents and feet
# ---------------------------------------------------------------------------
VENT_W = 2.0  # call
BACK_VENT_XS = tuple(53.0 + 4.5 * (i - 5.5) for i in range(12))  # call
BACK_VENT_Z = (46.0, 64.0)  # call
TOP_VENT_XS = tuple(x for x in (8.0 + 4.5 * i for i in range(21)) if not (40.0 < x < 52.0 or 64.0 < x < 92.0))
TOP_VENT_Y = (24.0, 40.0)  # call
FLOOR_VENT_XS = tuple(18.0 + 5.0 * i for i in range(15))  # call, 18 to 88
FLOOR_VENT_Y = (4.0, 13.0)  # call, ends in front of the bar posts at Y 14.5
FOOT_CENTRES = ((4.0, -0.5), (102.0, -0.5), (4.0, 40.0), (102.0, 40.0))  # call


def _checks():
    # Window inside the bezel and the case is centred on the active area
    assert abs((IN_X0 + IN_X1) / 2 - sum(ACTIVE_X) / 2) < 0.1
    assert BZ_X1 - BZ_X0 < PRINT_BED and Z_TO < PRINT_BED and Y_BACK_OUT - y_front(0) < PRINT_BED
    # Grille areas at least twice the minimum, open holes without bars
    sq = (SEN_INLET_SQ[1] - SEN_INLET_SQ[0] + 2 * GRILLE_GROW) * (SEN_INLET_SQ[3] - SEN_INLET_SQ[2] + 2 * GRILLE_GROW)
    rd = math.pi * (SEN_INLET_ROUND[2] / 2 + GRILLE_GROW) ** 2
    out = math.pi * (SEN_OUTLET[2] / 2 + GRILLE_GROW) ** 2
    assert sq + rd >= 2 * SEN_MIN_INLET_AREA, sq + rd
    assert out >= 2 * SEN_MIN_OUTLET_AREA, out
    # Plug zone clear of the board back plane at the connector height
    conn_z1 = SEN_Z0 + SEN_CONN[3]
    assert SEN_FACE_Y - SEN_MAX_H - ASSUMED_PLUG_ZONE > y_at(conn_z1, STUD_END_Z) + 0.5
    # Sensor below the ESP32 and the inlet end away from it
    assert SEN_Z0 + SEN_MAX_W < b2c(0, ESP_Y[0], 0)[2]
    assert SEN_X0 + SEN_INLET_ROUND[0] < ESP_X[0] - 40
    # Rocker and board top clear the top wall while the board goes in
    assert b2c(0, ROCKER_TOP_Y, FRONT_BZ)[2] < Z_TI - 0.5
    # Latch arm clears the PCB top while it deflects
    assert ARM_Z1 - LATCH_H_TIP - LATCH_CATCH > b2c(0, PCB_H, 0)[2] + 0.5
    assert ARM_Z1 - LATCH_H_ROOT - ARM_SLOT >= b2c(0, DISP_Y[1] + POCKET_CLEAR, FRONT_BZ)[2] + 0.5
    # Strains
    assert latch_strain_tapered(LATCH_L) <= PLA_STRAIN_LIMIT_XY
    hole_half = BAR_W / 2 + HOLE_CLEAR + WALL_HOLE_GROW
    prong_y = BAR_W / 2 + PRONG_CATCH - hole_half
    assert latch_strain_constant(PRONG_L, prong_y, PRONG_H) <= PLA_STRAIN_LIMIT_XY
    assert BAR_W - 2 * PRONG_H >= 2 * prong_y + 0.2
    # Bridges
    for w in (BAR_W + 2 * HOLE_CLEAR + 2 * WALL_HOLE_GROW, LATCH_WIN_X[1] - LATCH_WIN_X[0],
              TAB_XS[0][1] - TAB_XS[0][0] + 0.4, VENT_W):
        assert w <= PLA_MAX_BRIDGE, w
    assert PRONG_ROOT_X < POST_XS[0][0] - 1.0, "prongs flex outside the left post"
    assert TAB_BOSS_Z1 < b2c(0, 0, TAB_BZ[0])[2] - 2.0, "tab boss under the PCB"
    run = TOOTH_H / math.tan(math.radians(TOOTH_RETURN_DEG))
    lead = TOOTH_H / math.tan(math.radians(LATCH_LEAD_DEG))
    base0 = LATCH_WIN_Y[0] + TOOTH_GAP - BZ_TOP_GAP * run / TOOTH_H
    assert base0 + run + TOOTH_LAND + lead <= y_front(ARM_Z1) + ARM_B, "tooth on the arm"
    assert base0 + run + TOOTH_LAND + lead - BZ_TOP_GAP * lead / TOOTH_H < LATCH_WIN_Y[1] - 0.2, "tooth inside the window"
    # Pads compress
    assert 0 < ASSUMED_PAD_COMPRESSED_T - (SEN_MAX_H - SEN_H) < ASSUMED_GASKET_T
    # Battery shelf. The pack sits behind every board part with room for its lead
    assert PACK_FRONT_BZ < BOARD_BACK_BZ - MX125_PLUG[1] + 0.5 and FOOT_FRONT_BZ < BOARD_BACK_BZ - 2.0
    assert ROCKER_Z[0] > FENCE_FRONT_BZ + 4.0 and ESP_BZ > FENCE_FRONT_BZ + 4.0
    # Pack top under the top wall, rib tops under it and over the pack top back corner
    assert b2c(0, BY_TOP, PACK_FRONT_BZ)[2] < Z_TI - 1.5, "pack top under the top wall"
    assert b2c(0, BY_TOP, RIB_BZ - LEDGE_INTO_RIB)[2] < RIB_Z[1] < Z_TI - 2.0
    # Feet and fences sit inside their ribs, the lowest foot point over the sensor pocket
    foot_low = b2c(0, BY_SEAT - FOOT_T, RIB_BZ - LEDGE_INTO_RIB)
    assert foot_low[2] > RIB_Z[0], foot_low
    assert foot_low[1] < SEN_FACE_Y - SEN_MAX_H, "feet in front of the sensor rim"
    assert RIB_Z[0] > POCKET_Z1 + 0.5
    # Ribs inside the pack outline, clear of the back vents and the brackets
    for xc in SHELF_RIB_XC:
        assert SH_X0 < xc - RIB_T / 2 and xc + RIB_T / 2 < SH_X1, xc
        for v in BACK_VENT_XS:
            assert abs(xc - v) >= RIB_T / 2 + VENT_W / 2 + 0.4, (xc, v)
    assert all(RIGHT_FENCE_X[1] < v - VENT_W / 2 - 0.4 or RIGHT_FENCE_X[0] > v + VENT_W / 2 + 0.4 for v in BACK_VENT_XS)
    assert COIN_BLOCK_Z1 < BACK_VENT_Z[0], "coin block under the back vents"
    assert LEFT_FENCE_X[0] > STUDS[0][0] + CUP_OD / 2 + 4.0 and COIN_RIB_X[1] < STUDS[1][0] - CUP_OD / 2 - 5.0
    assert abs(SH_X1 - SH_X0 - BAT_L - BAT_PLAY) < 1e-9 and abs(BY_TOP - BY_SEAT - BAT_W - BAT_PLAY) < 1e-9
    # Coin slot over the bar post and the bar path, behind every board part, coin inside it
    assert COIN_FLOOR_Z[0] > POST_Z[1] + 1.0 and COIN_FLOOR_Z[0] > BAR_Z1 + HOLE_CLEAR + 2.0
    assert COIN_STOP_Y[0] > y_at(COIN_BLOCK_Z1, FENCE_FRONT_BZ) + 2.0, "coin block behind the board"
    assert Y_BACK_IN - COIN_STOP_Y[1] >= ASSUMED_RTC_D + 2.0 and COIN_BLOCK_Z1 > COIN_FLOOR_Z[1] + ASSUMED_RTC_D + 1.0
    assert COIN_SLOT_X[1] - COIN_SLOT_X[0] <= PLA_MAX_BRIDGE, "front stop bridges the slot"
    # Leads reach their headers. The pack lead must leave at or below the middle of its edge.
    mid = bat_lead_path(BY_SEAT + BAT_W / 2)
    assert mid <= BAT_LEAD_L - 2.0, mid
    assert rtc_lead_path() <= RTC_LEAD_MIN_L - 5.0, rtc_lead_path()

_checks()

if __name__ == "__main__":
    print(f"OZ {OZ:.2f} Z_TI {Z_TI:.2f} Z_TO {Z_TO:.2f} front Y at Z0 {y_front(0):.2f} at top {y_front(Z_TO):.2f}")
    print(f"outer X {OUT_X0} to {OUT_X1}, Y {y_front(0):.2f} to {Y_BACK_OUT}, strain arm {latch_strain_tapered(LATCH_L):.2f} %")
    print(f"sensor face Y {SEN_FACE_Y}, rim Y {SEN_RIM_Y}, bar Y {BAR_FRONT_Y} to {BAR_BACK_Y}")
    print(f"pack top Z {b2c(0, BY_TOP, PACK_FRONT_BZ)[2]:.2f} against {Z_TI:.2f}, pack bottom Z "
          f"{b2c(0, BY_SEAT, RIB_BZ)[2]:.2f}, lowest foot Z {b2c(0, BY_SEAT - FOOT_T, RIB_BZ - LEDGE_INTO_RIB)[2]:.2f}")
    print(f"pack lead from the lower corner {bat_lead_path(BY_SEAT + 4.0):.1f}, from mid edge "
          f"{bat_lead_path(BY_SEAT + BAT_W / 2):.1f}, against {BAT_LEAD_L}. Coin lead {rtc_lead_path():.1f}, "
          f"buy {RTC_LEAD_MIN_L} or longer")
