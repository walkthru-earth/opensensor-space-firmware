"""Build and run the shared e-paper UI host tests and render the previews.

The component under firmware/common/epaper_ui/ is plain C++23 with no
ESP-IDF, so the host compiles the same pages every e-paper board shows.
Every run also renders every page for synthetic scenarios at each size
class and rotation, and checks the 400x300 r0 frames, the CrowPanel's
frames, against the sha256 goldens below, and a few Arabic 400x300 r0
frames against their own. With --preview it writes PNGs of every frame to
the ignored root artifacts/epaper-ui-preview/<size>/r<deg>/, the Arabic ones
under ar/<size>/r<deg>/.
PNG encoding uses only the standard library. No device is accessed.

A golden moves only with a deliberate change to what the CrowPanel shows.
Look at the new PNG first, then paste the hash the failure prints.
"""

from __future__ import annotations

import argparse
import hashlib
from pathlib import Path
import struct
import subprocess
import tempfile
import zlib

from host_cxx import CXX_STD

ROOT = Path(__file__).resolve().parent.parent
COMPONENT = ROOT / "firmware/common/epaper_ui"
# Two header only files come from the CrowPanel, restore_order.h from its
# logger/ and display_profile.h from its display/, which the test holds
# equal to the host 400 x 300 profile.
LOGGER = ROOT / "firmware/esp-idf-crowpanel-epaper/logger"
BOARD_DISPLAY = ROOT / "firmware/esp-idf-crowpanel-epaper/display"
# The ePaper-3.97 profile comes as display/display_profile.h from its board
# root, which also holds the board_io.h it reads the IMU mount from. The test
# holds it to the host 800 x 480 profile.
BOARD_397 = ROOT / "firmware/esp-idf-waveshare-epaper-397"
PREVIEW = ROOT / "artifacts/epaper-ui-preview"
# The shared text component, the string table, bidi and the Arabic fonts the
# Latin fonts fall back to.
TEXT = ROOT / "firmware/common/text/src"
# Every source the two components' CMakeLists.txt build.
SOURCES = sorted((COMPONENT / "src").glob("*.cpp")) + sorted(TEXT.glob("*.cpp"))
# The size class whose r0 frames must not move.
GOLDEN_DIR = "400x300/r0"
# sha256 of each 400x300 r0 PBM, header and frame. The four settings popups
# moved when the Rotation row was added and again when the Language row was,
# every other frame is the one the CrowPanel drew before the UI became
# shared.
GOLDEN = {
    "asleep-dark": "126b725c87cc81fadf7efd504174090c6fa87c82f162d6d5fbb9265f4ce91ca5",
    "asleep": "126b725c87cc81fadf7efd504174090c6fa87c82f162d6d5fbb9265f4ce91ca5",
    "chart-Humidity": "65fa61a9c42099cf890f9648f356e87271b73f89e00ab4df89bb165cdab43608",
    "chart-PM1": "44a3a1d82b410d8c281e1bd240d20e6a003542b64f281cf7e068168538df1297",
    "chart-PM10": "2efc353ce92026ea46a669e07fe4d89f2cc4fdd5ab41a7df46f8ddf9ac916c09",
    "chart-PM2.5-dark": "c811104c312afafc4c92302d4361a1a2fcc7ab784ab17790ad612afab0632c16",
    "chart-PM2.5": "ec43e625516fc102911eb76a1eed6513e87591657824ce66073e248af19080e7",
    "chart-PM4": "42bc691f4d6adf8abda6a20543f6144561a61e074dca3d612f6d869cbcfdbe0d",
    "chart-Temp": "eecea52926d9e66afb8f90cf1f99acc5acbe4abfb248270d4cd79a3a5011deb3",
    "chart-co2": "8b8fa1115c478c2158e6ee3986344bb7f1de61f615ddcad2d00eac733001dae2",
    "chart-empty": "cb762aa7e3973f4e91cd317dafb79406e8703e38db2c325fee8e2103bc018b95",
    "chart-gap": "2d7372de43afeb0d4ecc8533b5d53f96f7896ad9e7ca672785b351c857062523",
    "chart-restart-dark": "fe141a2ba70995b947f95d0ce2a8aeacdc42b9650dbf63826d060c2d4d403951",
    "chart-restart": "58cd1aa0e5f7c21407b63c750876751abe8318b5e760893a0667ece2ba786906",
    "chart-warming": "500d28a2327c70507e2b404dd6ee514be0a7722b6cba8c9851cb26f5aa768e1f",
    "dashboard-bad-air": "455384a4ebd7484f031199e8b3cdf4b4d57c6f7a427a68e3dd3710baaa23b332",
    "dashboard-dark": "b82ec9f7383b4b2ef48430b361316f9d9409c30f4bb082673711b5b191c6fa63",
    "dashboard-gap": "51650090ca783f7be56d8696a4e241b124a1b3b4968b61d35237228a031208a7",
    "dashboard-no-sensor": "a848a17c215b79e83d400d8145d62bb379db26667e9ab19c9a9c97a55e9dafb9",
    "dashboard-restart-dark": "1de41aa57e28c5890f32e37608f27c55bdb8c13b9ef7f5c18d9910a37d048538",
    "dashboard-restart": "ef016444096fe4f5613322cfd8c14637c8fd86367dfac43b6d932fad2c214cab",
    "dashboard-warming": "f230688d53d64141a65faab5306edc3c873d4bde00961d3273ac8b3b2127bd5f",
    "dashboard": "d893d0c6acfc764dedad63f56ade78314f5ea65497a9f79eb346fbcc288d93a6",
    "pairing-dark": "d93fc4c58df213a0f03ee4c668743837d1782e44f9f093cbd018f43774e5f6a9",
    "pairing": "30c93f6ca15036d4add6f5bf5b36a1c034bb85a421df628d4fc14a69d804970e",
    "settings-edit-altitude": "a086ffb791d215ae139dbc2df4d0154a440aef164b329a39373571f4edb5184c",
    "settings-no-wifi-no-sensor": "46ddc56238550e22841bbd93f97ed4d440f0f4ee968613702f0edd128757d4e2",
    "settings-theme-dark": "3feef32b81b4ef8e2267260172293889f201e9657db4304b1a1f92648da1b155",
    "settings": "74403a4ff4a1d08a05e9069491474dc8de364716f002ce8bc8118598b3018752",
    "status-dark": "a0af7f50991ae40ec3e62b30dd4659e90eecbcb6dfc177e888053abcad7bf5b2",
    "status-fault": "35f039edd6ce4d2d1e9f940d053149ab5632e18a728d0aaf9504b3b755ef96ae",
    "status-warming": "c71ce893885c2b861f6806d9bfa399eea86ae707c6cb9588e81ab65fb1d87373",
    "status": "00c8c182626ebd0c10d3e79fdda2757c7cc91449f4c9f36c5d9cf714dc059b5d",
}
# A few Arabic 400x300 r0 frames, one of each kind of page, so a change to
# the bidi pass, the Arabic fonts or the mirroring shows up here.
AR_GOLDEN_DIR = "ar/400x300/r0"
AR_GOLDEN = {
    "chart-PM2.5": "7ee1ba60ef7f43311ef59133f56efb08dc2e21b41d440995fd53b6b0d08814af",
    "dashboard": "7870d88986f8054bb857bf484dbd8fd7aa516c674e1cdee455733a09829e729a",
    "settings": "e87201533012041d7932bca04f9c08be77251384ce2848a135e25d15fd7d466a",
    "status": "2c3c77122de04ab8a6e7bf5a43d7f273832df2a255970edf9ac6cf260bdc5ba5",
}


def compile_one(main: Path, out: Path, sanitize: bool) -> None:
    command = ["clang++", CXX_STD, "-Wall", "-Wextra", "-Werror",
               "-Wshadow", "-Wconversion", "-Wno-sign-conversion",
               f"-I{COMPONENT / 'include'}", f"-I{TEXT}", f"-I{LOGGER}", f"-I{BOARD_DISPLAY}", f"-I{BOARD_397}", str(main), *[str(s) for s in SOURCES],
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
    with tempfile.TemporaryDirectory(prefix="os-epaper-ui-") as directory:
        tmp = Path(directory)
        test = tmp / "epaper-ui-test"
        compile_one(ROOT / "tools/epaper_ui_test.cpp", test, args.sanitize)
        subprocess.run([str(test)], check=True)
        preview = tmp / "epaper-ui-preview"
        compile_one(ROOT / "tools/epaper_ui_preview.cpp", preview, args.sanitize)
        frames = tmp / "frames"
        frames.mkdir()
        subprocess.run([str(preview), str(frames)], check=True,
                       stdout=subprocess.DEVNULL)
        check_golden(frames / GOLDEN_DIR, GOLDEN, GOLDEN_DIR, every=True)
        check_golden(frames / AR_GOLDEN_DIR, AR_GOLDEN, AR_GOLDEN_DIR,
                     every=False)
        if not args.preview:
            return
        for pbm in sorted(frames.rglob("*.pbm")):
            rel = pbm.relative_to(frames).with_suffix(".png")
            out = PREVIEW / rel
            out.parent.mkdir(parents=True, exist_ok=True)
            png_from_pbm(pbm, out, 1)
            # The zoomed copy only for the small class, 800 x 480 reads at 1x.
            if args.scale > 1 and "400x300" in rel.parts[:2]:
                zoom = PREVIEW / f"x{args.scale}" / rel
                zoom.parent.mkdir(parents=True, exist_ok=True)
                png_from_pbm(pbm, zoom, args.scale)
            print(out)


def check_golden(directory: Path, golden: dict[str, str], label: str,
                 every: bool) -> None:
    """Hash the frames in `directory` against `golden`. With `every` a frame
    missing from `golden` fails too, without it only the named ones count."""
    got = {p.stem: hashlib.sha256(p.read_bytes()).hexdigest()
           for p in directory.glob("*.pbm")}
    names = golden.keys() | got.keys() if every else golden.keys()
    failures = []
    for name in sorted(names):
        want = golden.get(name)
        have = got.get(name)
        if want != have:
            failures.append(f"  {name}: want {want} got {have}")
    if failures:
        raise SystemExit(f"{label} frames moved:\n" + "\n".join(failures))
    print(f"epaper ui goldens, {len(golden)} frames at {label} match")


if __name__ == "__main__":
    main()
