#!/usr/bin/env python3
"""Fail closed when a release app image does not carry the identity the release names (#45).

A release has three version strings. The tag names the release and every
asset. Each board's `kFirmware` in telemetry_contract.h is the firmware
identity the board reports in LIVE, its BLE info and its Parquet footers. The
app description of the .bin (esp_app_desc_t) holds the version from
`git describe`, the project name, the ESP-IDF version and the ELF SHA-256.
This check ties them together. It fails when

- the image is not an ESP-IDF app image or has no app description
- the app version differs from --expect-version, the tag on a tag build, or
  ends in -dirty
- the project name differs from --app or from the build's project_description.json
- the ESP-IDF version differs from tools/idf-dependencies.lock
- the ELF SHA-256 in the description differs from the build's ELF
- the board's kFirmware is not in the image, or the image holds another
  firmware identity of the same form

Any missing or malformed input fails the check, it never passes by default.
On success it writes the identity as JSON next to the release files and
prints one Markdown table row for the job summary.

Usage:
  python tools/release_identity.py --board cores3 --app opensensor_cores3 \\
      --build firmware/esp-idf-cores3/build/bringup --expect-version v1.2.0 \\
      --out dist/opensensor_cores3-bringup-v1.2.0.identity.json
"""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import re
import struct
import sys

import boards

ROOT = Path(__file__).resolve().parents[1]
IDF_LOCK = Path("tools/idf-dependencies.lock")

# esp_image_header_t (24 bytes) and the first segment header (8 bytes) come
# before esp_app_desc_t, see components/esp_app_format/include/esp_app_desc.h
# in ESP-IDF 6.1.1. tools/ble_sync.py reads the same offsets.
IMAGE_MAGIC = 0xE9
DESC_OFFSET = 32
DESC_MAGIC = 0xABCD5432
DESC_BYTES = 256
FIELDS = {  # name: (offset in esp_app_desc_t, size)
    "version": (16, 32),
    "project_name": (48, 32),
    "time": (80, 16),
    "date": (96, 16),
    "idf_ver": (112, 32),
}
ELF_SHA256 = (144, 32)

# Every board names its firmware idf-<board>-parquet-v<major>.<minor>. The
# image may hold only its own, so a stale header or a second identity fails.
FIRMWARE_FORM = re.compile(r"idf-[a-z0-9]+-parquet-v[0-9]+(?:\.[0-9]+)*")
FIRMWARE_IN_IMAGE = re.compile(rb"idf-[a-z0-9]+-parquet-v[0-9]+(?:\.[0-9]+)*(?=\x00)")
K_FIRMWARE = re.compile(r'\bkFirmware =\s*"([^"]+)"')


class IdentityError(ValueError):
    pass


def text_field(desc: bytes, name: str) -> str:
    offset, size = FIELDS[name]
    raw = desc[offset:offset + size]
    if b"\0" not in raw:
        raise IdentityError(f"app description {name} is not NUL terminated")
    try:
        return raw.split(b"\0", 1)[0].decode("ascii")
    except UnicodeDecodeError as error:
        raise IdentityError(f"app description {name} is not ASCII") from error


def app_description(image: bytes) -> dict[str, str]:
    """The text fields and the ELF SHA-256 of the image's esp_app_desc_t."""
    if len(image) < DESC_OFFSET + DESC_BYTES or image[0] != IMAGE_MAGIC:
        raise IdentityError("not an ESP-IDF app image")
    desc = image[DESC_OFFSET:DESC_OFFSET + DESC_BYTES]
    if struct.unpack_from("<I", desc, 0)[0] != DESC_MAGIC:
        raise IdentityError("the image has no app description, use the app .bin, not a merged flash image")
    fields = {name: text_field(desc, name) for name in FIELDS}
    offset, size = ELF_SHA256
    fields["app_elf_sha256"] = desc[offset:offset + size].hex()
    return fields


def board_firmware(board: boards.Board, root: Path) -> str:
    header = root / board.app / "telemetry_contract.h"
    try:
        found = K_FIRMWARE.findall(header.read_text(encoding="utf-8"))
    except OSError as error:
        raise IdentityError(f"cannot read {header}, {error}") from error
    if len(found) != 1:
        raise IdentityError(f"{header} must define kFirmware once, found {len(found)}")
    if not FIRMWARE_FORM.fullmatch(found[0]):
        raise IdentityError(f"kFirmware {found[0]!r} in {header} is not idf-<board>-parquet-v<version>")
    return found[0]


def idf_versions(root: Path) -> set[str]:
    """The IDF_VER strings the pinned SDK can write. Espressif tags x.y.0 as vx.y."""
    try:
        lines = (root / IDF_LOCK).read_text(encoding="utf-8").splitlines()
    except OSError as error:
        raise IdentityError(f"cannot read {IDF_LOCK}, {error}") from error
    pinned = [line.split("=", 1)[1].strip() for line in lines if line.startswith("ESP_IDF_VERSION=")]
    if len(pinned) != 1 or not re.fullmatch(r"[0-9]+\.[0-9]+\.[0-9]+", pinned[0]):
        raise IdentityError(f"{IDF_LOCK} must set ESP_IDF_VERSION=<x.y.z> once")
    versions = {f"v{pinned[0]}"}
    if pinned[0].endswith(".0"):
        versions.add(f"v{pinned[0][:-2]}")
    return versions


def build_files(build: Path) -> tuple[str, Path, Path]:
    """Project name, app .bin and ELF named by build/project_description.json."""
    description = build / "project_description.json"
    try:
        project = json.loads(description.read_text(encoding="utf-8"))
    except (OSError, ValueError) as error:
        raise IdentityError(f"cannot read {description}, {error}") from error
    names = [project.get(key) if isinstance(project, dict) else None for key in ("project_name", "app_bin", "app_elf")]
    if not all(isinstance(name, str) and name for name in names):
        raise IdentityError(f"{description} needs project_name, app_bin and app_elf")
    return names[0], build / names[1], build / names[2]


def check(board_name: str, app: str, build: Path, expect_version: str, root: Path = ROOT) -> dict[str, str]:
    """The release identity of one board image, or IdentityError naming the first broken rule."""
    try:
        board = boards.find(board_name, root)
    except boards.BoardError as error:
        raise IdentityError(str(error)) from error
    firmware = board_firmware(board, root)
    project_name, app_bin, app_elf = build_files(build)
    try:
        image = app_bin.read_bytes()
        elf = app_elf.read_bytes()
    except OSError as error:
        raise IdentityError(f"cannot read the build output, {error}") from error
    desc = app_description(image)

    if not expect_version or expect_version.endswith("-dirty"):
        raise IdentityError(f"expected version {expect_version!r} must name a clean commit or tag")
    if desc["version"] != expect_version:
        raise IdentityError(f"app version {desc['version']!r} is not {expect_version!r}")
    if not (desc["project_name"] == project_name == app):
        raise IdentityError(f"project name in image {desc['project_name']!r}, in project_description.json "
                            f"{project_name!r}, expected {app!r}")
    if desc["idf_ver"] not in idf_versions(root):
        raise IdentityError(f"ESP-IDF version {desc['idf_ver']!r} is not the pinned {sorted(idf_versions(root))}")
    if desc["app_elf_sha256"] != hashlib.sha256(elf).hexdigest():
        raise IdentityError(f"app description ELF SHA-256 does not match {app_elf.name}")
    held = sorted({match.decode("ascii") for match in FIRMWARE_IN_IMAGE.findall(image)})
    if held != [firmware]:
        raise IdentityError(f"image holds firmware identities {held}, expected only kFirmware {firmware!r}")

    return {
        "board": board.slug,
        "alias": board.alias,
        "variant": board.telemetry_variant,
        "project_name": desc["project_name"],
        "firmware": firmware,
        "app_version": desc["version"],
        "idf_version": desc["idf_ver"],
        "build_date": desc["date"],
        "build_time": desc["time"],
        "app_elf_sha256": desc["app_elf_sha256"],
        "image_sha256": hashlib.sha256(image).hexdigest(),
    }


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--board", required=True, help="board name tools/boards.py resolves")
    parser.add_argument("--app", required=True, help="ESP-IDF project name, the release asset prefix")
    parser.add_argument("--build", type=Path, required=True, help="build directory with project_description.json")
    parser.add_argument("--expect-version", required=True, help="tag on a tag build, else git describe output")
    parser.add_argument("--out", type=Path, required=True, help="identity JSON to write")
    args = parser.parse_args(argv)
    try:
        identity = check(args.board, args.app, args.build, args.expect_version)
    except IdentityError as error:
        print(f"release identity: {error}", file=sys.stderr)
        return 1
    args.out.write_text(json.dumps(identity, indent=2) + "\n", encoding="utf-8")
    print(f"| {identity['project_name']} | {identity['variant']} | `{identity['firmware']}` | "
          f"`{identity['app_version']}` | `{identity['idf_version']}` | `{identity['app_elf_sha256'][:16]}` |")
    return 0


if __name__ == "__main__":
    sys.exit(main())
