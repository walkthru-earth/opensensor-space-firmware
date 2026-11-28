# SPDX-License-Identifier: CC-BY-4.0. Copyright Walkthru.Earth.
"""Vendor bodies for the Waveshare ePaper-3.97 and SEN63C desk stand, in the case frame.

The vendor files live in the ignored folder
artifacts/enclosures/waveshare-epaper-397-sen63c/vendor/ (README Vendor files,
SHA-256 checked 2026-11-26). They are read from the .xbf copies converted from
the STEP files there and are never copied into git.

board_body()   Waveshare STEP as it is (the board frame), then params.board_to_case().
sensor_body()  SEN6x STEP, mapped to the sensor sheet frame
               (x = X + 27.6, y = Z + 12.8, z = 21.3 - Y), then sensor_to_case(),
               which puts the opening face on SEN_FACE_Y facing +Y (the back wall),
               sensor x along case X from SEN_X0 and sensor y up case Z from SEN_Z0.
"""

from pathlib import Path

import build123d as bd
from cad import xbf
from cad.build123d import leaves

import params as P

REPO = Path(__file__).resolve().parents[3]
VENDOR_DIR = REPO / "artifacts" / "enclosures" / "waveshare-epaper-397-sen63c" / "vendor"
BOARD_XBF = VENDOR_DIR / "waveshare-epaper-397.xbf"
SENSOR_XBF = VENDOR_DIR / "sen6x.xbf"


def sensor_step_to_sheet():
    """STEP to sensor sheet frame, x = X + 27.6, y = Z + 12.8, z = 21.3 - Y."""
    return bd.Location(bd.Plane(origin=P.SEN_STEP_ORIGIN, x_dir=(1, 0, 0), z_dir=(0, 1, 0)))


def sensor_to_case():
    """Sensor sheet frame to case frame.

    x_dir (1, 0, 0) and z_dir (0, -1, 0) give y_dir = z_dir x x_dir = (0, 0, 1).
    Sheet (x, y, z) lands at (SEN_X0 + x, SEN_FACE_Y - z, SEN_Z0 + y), so the
    opening face z 0 sits on SEN_FACE_Y and the rim z 21.3 on SEN_RIM_Y.
    """
    return bd.Location(bd.Plane(origin=(P.SEN_X0, P.SEN_FACE_Y, P.SEN_Z0), x_dir=(1, 0, 0), z_dir=(0, -1, 0)))


def sensor_point(x, y, z=0.0):
    """Sensor sheet point to case frame, for the grilles, pads and checks."""
    return (P.SEN_X0 + x, P.SEN_FACE_Y - z, P.SEN_Z0 + y)


def _placed(path, step_to_frame, frame_to_case):
    if not path.is_file():
        raise FileNotFoundError(
            f"{path} is missing. Convert the vendor STEP in {VENDOR_DIR} to .xbf first, never refetch it."
        )
    solids = [leaf for leaf in leaves(xbf.read(path)) if getattr(leaf, "solids", None) and leaf.solids()]
    loc = frame_to_case * step_to_frame
    return bd.Compound(children=[s.moved(loc) for s in solids])


def board_body():
    """Waveshare ESP32-S3-ePaper-3.97 board, in the case frame."""
    return _placed(BOARD_XBF, bd.Location(), P.board_to_case())


def sensor_body():
    """SEN63C, in the case frame."""
    return _placed(SENSOR_XBF, sensor_step_to_sheet(), sensor_to_case())
