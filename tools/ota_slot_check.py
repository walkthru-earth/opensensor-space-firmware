"""Fail closed when an app image uses more than MAX_SHARE of its smallest app slot.

The slot sizes come from the board's partitions.csv, so a later partition
table change moves the limit with it. The image is the app .bin named by
build/project_description.json, which is the file OTA writes into a slot.
Any missing or malformed input fails the check, it never passes by default.
On success it prints one Markdown table for the CI job summary.

Usage:
  python tools/ota_slot_check.py --label "cores3 (bringup)" \
      --partitions firmware/esp-idf-cores3/bringup/partitions.csv \
      --build firmware/esp-idf-cores3/build/bringup

Why half the slot (issue #45)
  Measured on main ed1038e (2026-10-19) every app slot is 0x600000 (6 MiB) and
  the app .bin this check measures is 1,607,104 bytes for cores3 bringup
  (25.5 %) and 1,446,384 for waveshare logger (23.0 %). Waveshare diagnostic
  is about 300,000 bytes (4.8 %) by `idf.py size`. A 50 % share fails at
  3 MiB, so the largest image can still grow by about 1.5 MB, roughly four times the 341 KB one
  std::format_to_n call cost (AGENTS.md), which leaves room for #40 OTA and
  normal feature work without a false failure. Stopping at half instead of
  near 100 % makes the build fail while a slot still has 3 MiB spare, so there
  is time to trim the image or plan a new partition table. Waiting for the
  image to stop fitting would leave no OTA path for boards in the field.
  Raising the share is an owner decision recorded here with new measurements.

  8 MiB boards (2026-11-17). The first CrowPanel layout had 3 MiB slots and
  its 1,710,960 byte image was 54.4 %. The owner kept the 50 % share and moved
  the unused SPIFFS space into the slots instead, so each slot is 0x3C0000
  (3.75 MiB) and the same image is 43.5 %. No firmware code reads SPIFFS.
"""

from __future__ import annotations

import argparse
import json
from pathlib import Path
import re
import sys

MAX_SHARE = 0.50
SIZE = re.compile(r"(0x[0-9a-f]+|[0-9]+)([km]?)", re.IGNORECASE)


class CheckError(ValueError):
    pass


def parse_size(text: str) -> int:
    """Size field of an ESP-IDF partition CSV, hex or decimal with an optional K or M suffix."""
    match = SIZE.fullmatch(text.strip())
    if not match:
        raise CheckError(f"partition size {text!r} is not a number")
    value = int(match[1], 0) * {"": 1, "k": 1024, "m": 1024 * 1024}[match[2].lower()]
    if value <= 0:
        raise CheckError(f"partition size {text!r} is not positive")
    return value


# The type keywords gen_esp32part.py in ESP-IDF 6.1 accepts, in any case.
TYPE_KEYWORDS = {"app": 0x00, "data": 0x01, "bootloader": 0x02, "partition_table": 0x03}


def is_app_type(kind: str) -> bool:
    """ESP-IDF reads a numeric type with int(x, 0), so 0, 0x00 and 0x0000 all mean app.

    A type that is neither a keyword nor a number from 0 to 0xFE raises, so a
    typo such as 0xZZ cannot hide an app slot from the check. gen_esp32part.py
    rejects it at build time too, this keeps the check closed on its own."""
    value = TYPE_KEYWORDS.get(kind.lower())
    if value is None:
        try:
            value = int(kind, 0)
        except ValueError:
            raise CheckError(f"partition type {kind!r} is not a known keyword or a number") from None
        if not 0 <= value <= 0xFE:
            raise CheckError(f"partition type {kind!r} is outside 0x00 to 0xFE")
    return value == 0


def app_slots(text: str) -> dict[str, int]:
    """Name and size of every app partition in a partitions.csv."""
    slots: dict[str, int] = {}
    for number, line in enumerate(text.splitlines(), 1):
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        fields = [field.strip() for field in line.split(",")]
        if len(fields) < 5:
            raise CheckError(f"partitions.csv line {number} has {len(fields)} fields, expected at least 5")
        name, kind, size = fields[0], fields[1], fields[4]
        if not is_app_type(kind):
            continue
        if name in slots:
            raise CheckError(f"partitions.csv names app partition {name} twice")
        slots[name] = parse_size(size)
    if not slots:
        raise CheckError("partitions.csv has no app partition")
    return slots


def image_path(build: Path) -> Path:
    description = build / "project_description.json"
    try:
        app_bin = json.loads(description.read_text())["app_bin"]
    except (OSError, ValueError, KeyError, TypeError) as error:
        raise CheckError(f"cannot read app_bin from {description}, {error}") from error
    if not isinstance(app_bin, str) or not app_bin:
        raise CheckError(f"{description} has no app_bin name")
    return build / app_bin


def check(partitions: Path, build: Path) -> tuple[int, str, int]:
    """Return the image size, the smallest slot name and its size, or raise CheckError."""
    try:
        text = partitions.read_text()
    except OSError as error:
        raise CheckError(f"cannot read partitions.csv {partitions}, {error}") from error
    slots = app_slots(text)
    image = image_path(build)
    if not image.is_file():
        raise CheckError(f"app image {image} is not a file")
    try:
        used = image.stat().st_size
    except OSError as error:
        raise CheckError(f"cannot read the app image {image}, {error}") from error
    if used <= 0:
        raise CheckError(f"app image {image} is empty")
    slot = min(slots, key=lambda name: (slots[name], name))
    limit = int(slots[slot] * MAX_SHARE)
    if used > limit:
        raise CheckError(f"app image {image.name} is {used:,} bytes, {used / slots[slot]:.1%} of slot "
                         f"{slot} ({slots[slot]:,} bytes from {partitions}), over the "
                         f"{MAX_SHARE:.0%} budget of {limit:,} bytes. Trim the image, or change "
                         "MAX_SHARE in tools/ota_slot_check.py with new measurements")
    return used, slot, slots[slot]


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--label", required=True)
    parser.add_argument("--partitions", type=Path, required=True, help="the board variant partitions.csv")
    parser.add_argument("--build", type=Path, required=True, help="build directory with project_description.json")
    args = parser.parse_args()
    try:
        used, slot, size = check(args.partitions, args.build)
    except CheckError as error:
        raise SystemExit(f"OTA slot check failed for {args.label}, {error}") from error
    print("\n".join([
        f"### {args.label} OTA slot share", "",
        "| Image | App bytes | Smallest slot | Slot bytes | Share | Budget |",
        "| --- | ---: | --- | ---: | ---: | ---: |",
        f"| {args.label} | {used:,} | {slot} | {size:,} | {used / size:.1%} | {MAX_SHARE:.0%} |",
    ]) + "\n")
    sys.stdout.flush()


if __name__ == "__main__":
    main()
