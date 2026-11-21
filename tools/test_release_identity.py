"""Host test for tools/release_identity.py (#45).

Builds a fake app image and build directory per board from the real
kFirmware, proves the clean case passes and that each broken identity fails
with its own reason. No SDK and no board is needed.
"""

from __future__ import annotations

import dataclasses
import hashlib
import json
from pathlib import Path
import struct
import sys
import tempfile

import boards
import release_identity as ri

ROOT = Path(__file__).resolve().parents[1]
TAG = "v1.2.0"


def pinned_idf() -> str:
    return sorted(ri.idf_versions(ROOT))[-1]


def make_image(*, version: str, project: str, idf: str, elf_sha: bytes, body: bytes,
               magic: int = ri.IMAGE_MAGIC, desc_magic: int = ri.DESC_MAGIC) -> bytes:
    desc = bytearray(ri.DESC_BYTES)
    struct.pack_into("<I", desc, 0, desc_magic)
    for name, value in (("version", version), ("project_name", project), ("time", "00:00:00"),
                        ("date", "Jan  1 2026"), ("idf_ver", idf)):
        offset, size = ri.FIELDS[name]
        raw = value.encode("ascii")
        assert len(raw) < size, name
        desc[offset:offset + len(raw)] = raw
    offset, size = ri.ELF_SHA256
    desc[offset:offset + size] = elf_sha
    header = bytes([magic]) + bytes(ri.DESC_OFFSET - 1)
    return header + bytes(desc) + body


def write_build(build: Path, project: str, image: bytes, elf: bytes, description: dict | None = None) -> None:
    build.mkdir(parents=True, exist_ok=True)
    (build / f"{project}.bin").write_bytes(image)
    (build / f"{project}.elf").write_bytes(elf)
    description = description if description is not None else {
        "project_name": project, "app_bin": f"{project}.bin", "app_elf": f"{project}.elf"}
    (build / "project_description.json").write_text(json.dumps(description))


def expect_error(fragment: str, build: Path, board: str, app: str, version: str = TAG) -> None:
    try:
        ri.check(board, app, build, version)
    except ri.IdentityError as error:
        assert fragment in str(error), (fragment, str(error))
    else:
        raise AssertionError(f"accepted a build that should fail with {fragment!r}")


def check_board(board: boards.Board, scratch: Path) -> int:
    firmware = ri.board_firmware(board, ROOT)
    app = f"opensensor_{board.alias}"
    elf = f"elf of {board.alias}".encode()
    elf_sha = hashlib.sha256(elf).digest()
    body = b"\0rodata\0" + firmware.encode() + b"\0more\0"

    def build_with(name: str, **changes) -> Path:
        image_args = dict(version=TAG, project=app, idf=pinned_idf(), elf_sha=elf_sha, body=body)
        description = changes.pop("description", None)
        elf_bytes = changes.pop("elf", elf)
        image_args.update(changes)
        build = scratch / board.alias / name
        write_build(build, app, make_image(**image_args), elf_bytes, description)
        return build

    clean = build_with("clean")
    identity = ri.check(board.alias, app, clean, TAG)
    assert identity["firmware"] == firmware and identity["app_version"] == TAG, identity
    assert identity["board"] == board.slug and identity["variant"] == board.telemetry_variant, identity
    assert identity["app_elf_sha256"] == elf_sha.hex(), identity
    # A dry run expects the git describe output of the commit.
    assert ri.check(board.alias, app, build_with("dry", version="2654652"), "2654652")["app_version"] == "2654652"

    cases = [
        ("app version 'v1.1.9'", build_with("old-tag", version="v1.1.9"), TAG),
        ("must name a clean commit", build_with("dirty", version="2654652-dirty"), "2654652-dirty"),
        ("project name in image", build_with("project", project="opensensor_other"), TAG),
        ("project name in image", build_with("description", description={
            "project_name": "other", "app_bin": f"{app}.bin", "app_elf": f"{app}.elf"}), TAG),
        ("is not the pinned", build_with("idf", idf="v5.5.5"), TAG),
        ("ELF SHA-256 does not match", build_with("elf", elf=b"another elf"), TAG),
        ("expected only kFirmware", build_with("missing", body=b"\0no identity\0"), TAG),
        ("expected only kFirmware", build_with("stale", body=body + b"idf-other-parquet-v9.9\0"), TAG),
        ("expected only kFirmware", build_with("unterminated", body=b"\0" + firmware.encode()), TAG),
        ("not an ESP-IDF app image", build_with("magic", magic=0), TAG),
        ("has no app description", build_with("desc", desc_magic=0), TAG),
        ("needs project_name", build_with("no-bin", description={"project_name": app}), TAG),
    ]
    for fragment, build, version in cases:
        expect_error(fragment, build, board.alias, app, version)
    expect_error("unknown board", clean, "nope", app)
    expect_error("cannot read", scratch / "absent", board.alias, app)
    return len(cases) + 3


def check_header_rules(board: boards.Board, scratch: Path) -> None:
    """A header without one kFirmware of the shared form fails before any image is read."""
    for name, text, fragment in (
            ("none", "constexpr int x = 1;\n", "must define kFirmware once"),
            ("two", 'kFirmware = "idf-a-parquet-v1.0"\nkFirmware = "idf-a-parquet-v1.1"\n', "found 2"),
            ("form", 'constexpr const char *kFirmware = "cores3 v6";\n', "is not idf-<board>-parquet")):
        fake = dataclasses.replace(board, project=f"firmware/{name}")
        header = scratch / fake.app / "telemetry_contract.h"
        header.parent.mkdir(parents=True, exist_ok=True)
        header.write_text(text)
        try:
            ri.board_firmware(fake, scratch)
        except ri.IdentityError as error:
            assert fragment in str(error), (name, str(error))
        else:
            raise AssertionError(f"header {name} accepted")


def check_idf_versions(scratch: Path) -> None:
    lock = scratch / ri.IDF_LOCK
    lock.parent.mkdir(parents=True, exist_ok=True)
    for text, want in (("ESP_IDF_VERSION=6.1.1\n", {"v6.1.1"}), ("ESP_IDF_VERSION=6.2.0\n", {"v6.2.0", "v6.2"})):
        lock.write_text(text)
        assert ri.idf_versions(scratch) == want, (text, ri.idf_versions(scratch))
    for text in ("", "ESP_IDF_VERSION=6.1\n", "ESP_IDF_VERSION=6.1.1\nESP_IDF_VERSION=6.1.2\n"):
        lock.write_text(text)
        try:
            ri.idf_versions(scratch)
        except ri.IdentityError:
            continue
        raise AssertionError(f"lock {text!r} accepted")


def main() -> int:
    every = boards.boards()
    count = 0
    with tempfile.TemporaryDirectory() as raw:
        scratch = Path(raw)
        for board in every:
            count += check_board(board, scratch / "builds")
        check_header_rules(every[0], scratch / "headers")
        check_idf_versions(scratch / "lock")
    print(f"release identity tests passed, {len(every)} boards, {count} cases")
    return 0


if __name__ == "__main__":
    sys.exit(main())
