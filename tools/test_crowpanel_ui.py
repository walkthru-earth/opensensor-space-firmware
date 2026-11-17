"""Build and run the CrowPanel e-paper UI host tests and render the previews.

The UI sources under firmware/esp-idf-crowpanel-epaper/display/ are plain
C++23 with no ESP-IDF, so the host compiles the same pages the panel shows.
With --preview the harness also renders every page for synthetic scenarios
and writes PNGs to the board's ignored artifacts/ui-preview/. PNG encoding
uses only the standard library. No device is accessed.
"""

from __future__ import annotations

import argparse
from pathlib import Path
import struct
import subprocess
import tempfile
import zlib

from host_cxx import CXX_STD

ROOT = Path(__file__).resolve().parent.parent
DISPLAY = ROOT / "firmware/esp-idf-crowpanel-epaper/display"
PREVIEW = ROOT / "firmware/esp-idf-crowpanel-epaper/artifacts/ui-preview"
# The host-portable part of the display stack. epd_uc8276c.cpp needs ESP-IDF.
SOURCES = ["canvas.cpp", "chart.cpp", "history.cpp", "pages.cpp",
           "settings.cpp", "ui_model.cpp", "fonts_gen.cpp"]


def compile_one(main: Path, out: Path, sanitize: bool) -> None:
    command = ["clang++", CXX_STD, "-Wall", "-Wextra", "-Werror",
               "-Wshadow", "-Wconversion", "-Wno-sign-conversion",
               f"-I{DISPLAY}", str(main), *[str(DISPLAY / s) for s in SOURCES],
               "-o", str(out)]
    if sanitize:
        command += ["-g", "-fsanitize=address,undefined",
                    "-fno-sanitize-recover=all"]
    subprocess.run(command, check=True)


def png_from_pbm(pbm: Path, png: Path, scale: int) -> None:
    data = pbm.read_bytes()
    # P4 header is three whitespace separated tokens then one byte.
    parts = data.split(maxsplit=3)
    if parts[0] != b"P4":
        raise SystemExit(f"{pbm} is not a binary PBM")
    width, height = int(parts[1]), int(parts[2])
    pixels = parts[3]
    stride = (width + 7) // 8
    rows = bytearray()
    for y in range(height):
        line = pixels[y * stride:(y + 1) * stride]
        if scale == 1:
            # PNG grey 1 bit, 1 is white, PBM 1 is black.
            row = b"\x00" + bytes(b ^ 0xFF for b in line)
            rows += row
            continue
        grey = bytearray()
        for x in range(width):
            black = line[x >> 3] & (0x80 >> (x & 7))
            grey += bytes([0 if black else 255]) * scale
        for _ in range(scale):
            rows += b"\x00" + grey
    depth = 1 if scale == 1 else 8
    w, h = width * scale, height * scale

    def chunk(kind: bytes, body: bytes) -> bytes:
        crc = zlib.crc32(kind + body) & 0xFFFFFFFF
        return struct.pack(">I", len(body)) + kind + body + struct.pack(">I", crc)

    png.write_bytes(b"\x89PNG\r\n\x1a\n"
                    + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, depth, 0, 0, 0, 0))
                    + chunk(b"IDAT", zlib.compress(bytes(rows), 9))
                    + chunk(b"IEND", b""))


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sanitize", action="store_true")
    parser.add_argument("--preview", action="store_true",
                        help="also render the page previews to PNG")
    parser.add_argument("--scale", type=int, default=2,
                        help="extra preview copies at this zoom, 1 to skip")
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="os-crowpanel-ui-") as directory:
        tmp = Path(directory)
        test = tmp / "crowpanel-ui-test"
        compile_one(ROOT / "tools/crowpanel_ui_test.cpp", test, args.sanitize)
        subprocess.run([str(test)], check=True)
        if not args.preview:
            return
        preview = tmp / "crowpanel-ui-preview"
        compile_one(ROOT / "tools/crowpanel_ui_preview.cpp", preview, args.sanitize)
        frames = tmp / "frames"
        frames.mkdir()
        subprocess.run([str(preview), str(frames)], check=True,
                       stdout=subprocess.DEVNULL)
        PREVIEW.mkdir(parents=True, exist_ok=True)
        zoom = PREVIEW / f"x{args.scale}"
        if args.scale > 1:
            zoom.mkdir(exist_ok=True)
        for pbm in sorted(frames.glob("*.pbm")):
            png_from_pbm(pbm, PREVIEW / f"{pbm.stem}.png", 1)
            if args.scale > 1:
                png_from_pbm(pbm, zoom / f"{pbm.stem}.png", args.scale)
            print(PREVIEW / f"{pbm.stem}.png")


if __name__ == "__main__":
    main()
