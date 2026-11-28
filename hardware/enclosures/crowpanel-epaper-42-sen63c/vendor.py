# SPDX-License-Identifier: CC-BY-4.0. Copyright Walkthru.Earth.
"""Vendor bodies for the CrowPanel 4.2 and SEN63C desk base, placed in the base frame.

The vendor files live in the ignored folder
artifacts/enclosures/crowpanel-epaper-42-sen63c/vendor/ (PLAN Global
Constraints, SHA-256 checked 2026-11-19). They are read from the .xbf copies
converted from the STEP files there and are never copied into git.

panel_body()   CrowPanel STEP, mapped to the board sheet viewer frame
               (x = 53.5 - X, y = Z + 43.5, z = Y + 0.5), then panel_to_base().
sensor_body()  SEN6x STEP, mapped to the sensor sheet frame
               (x = X + 27.6, y = Z + 12.8, z = 21.3 - Y), then sensor_to_base(),
               which puts the opening face 2.0 in front of the back wall inner
               face (the compressed gasket), inlet end at the left, openings +Y.
foam_body()    SEN6x example sealing foam (55 x 25.4 x 0.9), mapped to the sensor
               sheet frame (x = X, y = 25.6 - Y, z = -Z, per the sensor sheet, its
               plan is mirrored in y and it lies outside the opening face), then
               sensor_to_base(). Reference only, uncompressed in the 2.0 gasket gap.
"""

from pathlib import Path

import build123d as bd
from cad import xbf
from cad.build123d import leaves

import params as P

REPO = Path(__file__).resolve().parents[3]
VENDOR_DIR = REPO / "artifacts" / "enclosures" / "crowpanel-epaper-42-sen63c" / "vendor"
PANEL_XBF = VENDOR_DIR / "crowpanel-4.2-epaper.xbf"
SENSOR_XBF = VENDOR_DIR / "sen6x.xbf"
FOAM_XBF = VENDOR_DIR / "sen6x-sealing-foam.xbf"


def panel_step_to_sheet():
    """STEP to board sheet viewer frame: x = 53.5 - X, y = Z + 43.5, z = Y + 0.5."""
    # X maps to -x, Z to +y, Y to +z. As a Plane: x_dir (-1, 0, 0), z_dir (0, 1, 0),
    # which gives y_dir = z_dir x x_dir = (0, 0, 1). Checked against the sheet map.
    return bd.Location(bd.Plane(origin=P.PANEL_STEP_ORIGIN, x_dir=(-1, 0, 0), z_dir=(0, 1, 0)))


def sensor_step_to_sheet():
    """STEP to sensor sheet frame: x = X + 27.6, y = Z + 12.8, z = 21.3 - Y."""
    # X maps to +x, Z to +y, Y to -z. As a Plane: x_dir (1, 0, 0), z_dir (0, 1, 0),
    # which gives y_dir = z_dir x x_dir = (0, 0, -1). Point (X, Y, Z) lands at
    # origin + X x_dir + Y y_dir + Z z_dir = (27.6 + X, 12.8 + Z, 21.3 - Y).
    return bd.Location(bd.Plane(origin=P.SEN_STEP_ORIGIN, x_dir=(1, 0, 0), z_dir=(0, 1, 0)))


def foam_step_to_sheet():
    """Foam STEP to sensor sheet frame: x = X, y = 25.6 - Y, z = -Z."""
    # The foam STEP plan is mirrored in y (sensor sheet, Coordinate convention) and
    # its 0.9 thickness runs Z 0 to 0.9 away from the opening face (sheet z = 0,
    # z grows into the sensor). A half turn about x: x_dir (1, 0, 0), z_dir
    # (0, 0, -1), which gives y_dir = z_dir x x_dir = (0, -1, 0). Point (X, Y, Z)
    # lands at (X, 25.6 - Y, -Z). Round inlet (6.7, 7.4) lands at (6.7, 18.2),
    # as the sheet states.
    return bd.Location(bd.Plane(origin=(0.0, P.SEN_W, 0.0), x_dir=(1, 0, 0), z_dir=(0, 0, -1)))


def _placed(path, step_to_sheet, sheet_to_base):
    if not path.is_file():
        raise FileNotFoundError(
            f"{path} is missing. Convert the vendor STEP in {VENDOR_DIR} to .xbf first, never refetch it."
        )
    solids = [leaf for leaf in leaves(xbf.read(path)) if getattr(leaf, "solids", None) and leaf.solids()]
    loc = sheet_to_base * step_to_sheet
    return bd.Compound(children=[s.moved(loc) for s in solids])


def panel_body():
    """CrowPanel 4.2 with its printed shell, in the base frame."""
    return _placed(PANEL_XBF, panel_step_to_sheet(), P.panel_to_base())


def sensor_body():
    """SEN63C, in the base frame."""
    return _placed(SENSOR_XBF, sensor_step_to_sheet(), P.sensor_to_base())


def foam_body():
    """SEN6x example sealing foam, on the opening face in the base frame, reference only."""
    return _placed(FOAM_XBF, foam_step_to_sheet(), P.sensor_to_base())
