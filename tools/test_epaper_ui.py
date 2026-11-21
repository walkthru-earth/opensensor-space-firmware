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
# moved when the Rotation row was added and again when the Language row was.
# Every frame that shows a reading moved when digits got fixed slots, the
# settings popups with them as the dashboard shows behind them. Every other
# frame is the one the CrowPanel drew before the UI became shared.
GOLDEN = {
    "asleep-dark": "126b725c87cc81fadf7efd504174090c6fa87c82f162d6d5fbb9265f4ce91ca5",
    "asleep": "126b725c87cc81fadf7efd504174090c6fa87c82f162d6d5fbb9265f4ce91ca5",
    "chart-Humidity": "2c1b632abd234a05e5b0487dfdefbd18753a581e02f5067875ea625b341a03b9",
    "chart-PM1": "ff0864161d28824c37f25b68b95feda9ad8874730d6abf0fefa52ede1c89f936",
    "chart-PM10": "398c8abb98e6356f20fbdb7ae470574597ba749b23be00e7dc5f10e51fec3adc",
    "chart-PM2.5-dark": "ba1ae2aae4bf0090233076a03e352850025920e91c1867ab626b1afa70b7bed7",
    "chart-PM2.5": "2ae45dc3f64a5c04f52b582719a3b0c3f24f38d253f0dddf4dd23e688c932dab",
    "chart-PM4": "7c1d67992079c084101e277db2bd653dff61490de171bf4e97b4f4c3f9c67921",
    "chart-Temp": "48de46fa13230a67f9ee422cb6fa0a4723d5ebef6fd619eeed10177870c85f17",
    "chart-co2": "4a0314753a47f45a380756cf5daf017e03f7c93e9598ec046738eaa3adc14f3d",
    "chart-empty": "cb762aa7e3973f4e91cd317dafb79406e8703e38db2c325fee8e2103bc018b95",
    "chart-gap": "4b99e275ae6bf5250f6a6aa31f98f2528fdf5f9620188fe6c3ffb877a430b65d",
    "chart-restart-dark": "34ae15c7776f41b00c65b2516d785b796725cb93fb6eab742483d0342b40ced5",
    "chart-restart": "5ab5c199eacb02a1078b02f989c5c986c660073b2bafd3925f4f6ee554a0a816",
    "chart-warming": "500d28a2327c70507e2b404dd6ee514be0a7722b6cba8c9851cb26f5aa768e1f",
    "dashboard-bad-air": "88db9db31e920f41779de4a54a1a3f9b5c33ae79703652a9d7cf6d0af7008448",
    "dashboard-dark": "4cd9bdb214b52bf50dd4d02f4a87216c3958387bafe632305bd848f638b12135",
    "dashboard-gap": "caeb56d85b9e30e7e79fcff817caf0cdb5ef314629da0b20e19b6993a95db625",
    "dashboard-no-sensor": "fcbea4371dea810a9556bde5fc8dc8375514d1ed678de02d3ad1d2c0f37083b2",
    "dashboard-restart-dark": "088164f0684e5e7cbb9b010cc882794ab25eb2f6d4534b280030ea7eb55796d1",
    "dashboard-restart": "01aeff0f6efcfbc96808085bc89f13e1a7f6e5924c5eb4757594d7b5717fe3ba",
    "dashboard-warming": "90dec131a399082b8dc061c3b7d3f028a669aa5ff547260927f38f9c5c4ae4de",
    "dashboard": "472f9f7d4fdbe9bc655994fe64b2b99a28fbe58e4e91d6deaf7d11b6634a8a31",
    "pairing-dark": "d93fc4c58df213a0f03ee4c668743837d1782e44f9f093cbd018f43774e5f6a9",
    "pairing": "30c93f6ca15036d4add6f5bf5b36a1c034bb85a421df628d4fc14a69d804970e",
    "settings-edit-altitude": "6b01295158a1060e12458e77b5514bd86b44a79251ad7e7542b2653ea7ca36e8",
    "settings-no-wifi-no-sensor": "46ddc56238550e22841bbd93f97ed4d440f0f4ee968613702f0edd128757d4e2",
    "settings-theme-dark": "935bfc0fa01358206ef9440d7aa95d4be2d8facc580910211e630bdb7cd3db23",
    "settings": "a57a55a8161b17967155fdb25287f904f1296164d63ce9831c2874fd403b8e98",
    "status-dark": "a0af7f50991ae40ec3e62b30dd4659e90eecbcb6dfc177e888053abcad7bf5b2",
    "status-fault": "35f039edd6ce4d2d1e9f940d053149ab5632e18a728d0aaf9504b3b755ef96ae",
    "status-warming": "c71ce893885c2b861f6806d9bfa399eea86ae707c6cb9588e81ab65fb1d87373",
    "status": "00c8c182626ebd0c10d3e79fdda2757c7cc91449f4c9f36c5d9cf714dc059b5d",
    "storage-fault": "d4884180575902b99bef3df73a5727b46a11b245945b3c4136f3e4d296e4a3d9",
    "thing-id-fault": "3e6ffb8df4b7eb628ecac12c6bd22e96c45b6122e9f8a83a4059e331acf1b3ab",
    # No CrowPanel reports a battery, so it never draws these. They pin the
    # header battery icon a board with a PMIC adds.
    "chart-battery-empty": "fc602b5d2864e1896380ce975f888a8cef5630d71a2cb6de77237adb6c4fb33d",
    "dashboard-battery-dark": "ea648ac0569fe9076a1fec4601dca1354fbcb4db6cb251efa47c7f5e710f267c",
    "dashboard-battery": "d9afb75af2c3be8ff138f42617e4af24cced477566f0874fc0c8c71aead977ce",
    "dashboard-gap-battery": "9f07559487e7e358ab8575c747ea3d8a4d1b7e15985f17058ba239c0d2755afc",
    "status-battery-charging": "073c7f3b6b3fca5af41cc13f4021c8180b718ab0274c9d2ec57786da11c18300",
}
# A few Arabic 400x300 r0 frames, one of each kind of page, so a change to
# the bidi pass, the Arabic fonts or the mirroring shows up here.
AR_GOLDEN_DIR = "ar/400x300/r0"
AR_GOLDEN = {
    "chart-PM2.5": "ff9083d985159f88a8a835add40dae1b1371a6f9ef341581232fe3f87ac80304",
    "dashboard": "600be9948b1080715ef6d5a49d1ed379cf22013804bb63d028f7b576bac02d85",
    "settings": "4152e7d6ae7dc4bd3f5b608f73f50302e4e1da56f756f280ab684d8d6f343ca4",
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
