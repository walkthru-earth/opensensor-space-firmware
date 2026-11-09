"""Host build of the pinned embedded-h3-location library for tools/test_*.py.

The board builds fetch walkthru-earth/embedded-h3-location through the IDF
component manager, as firmware/common/location/idf_component.yml asks, and
record the resolved commit in each board dependencies.lock. Host tests compile
the same commit. The library is cloned once at its release tag into the user
cache and checked against every lock, so a host test never builds a different
library than the boards link.

`python tools/h3_library.py` prints the checkout path after the checks.
"""

from __future__ import annotations

import hashlib
import json
import os
from pathlib import Path
import re
import subprocess

from host_cxx import CXX_STD

URL = "https://github.com/walkthru-earth/embedded-h3-location.git"
NAME = "walkthru-earth/embedded-h3-location"
# Every board app that links firmware/common/location. Their locks must agree.
LOCKS = (
    "firmware/esp-idf-cores3/dependencies.lock",
    "firmware/esp-idf-waveshare-sim7670g/dependencies.lock",
    "firmware/esp-idf-waveshare-sim7670g/diagnostic/dependencies.lock",
)
MANIFEST = "firmware/common/location/idf_component.yml"
# H3 warnings the library's own host build does not raise on. The C sources
# are upstream bytes and are never edited.
H3_FLAGS = ("-std=c99", "-O1", "-Wall", "-Wextra", "-Werror",
            "-Wno-sign-compare", "-Wno-deprecated-declarations")
SANITIZE = ("-fsanitize=address,undefined", "-fno-omit-frame-pointer")


def pinned_tag(root: Path) -> str:
    """The git ref in the location component manifest."""
    match = re.search(r'^\s+version:\s+"([^"]+)"\s*$',
                      (root / MANIFEST).read_text(), re.MULTILINE)
    if match is None:
        raise ValueError(f"{MANIFEST} names no version for {NAME}")
    return match.group(1)


def locked_commit(root: Path) -> str:
    """The one commit every board lock resolved the library to."""
    commits = set()
    for relative in LOCKS:
        text = (root / relative).read_text()
        block = re.search(rf"^  {re.escape(NAME)}:\n((?:    .*\n)+)", text,
                          re.MULTILINE)
        if block is None:
            raise ValueError(f"{relative} does not lock {NAME}")
        body = block.group(1)
        if f"      git: {URL}\n" not in body or "      type: git\n" not in body:
            raise ValueError(f"{relative} locks {NAME} from another source")
        version = re.search(r"^    version: ([0-9a-f]{40})$", body, re.MULTILINE)
        if version is None:
            raise ValueError(f"{relative} locks {NAME} without a commit")
        commits.add(version.group(1))
    if len(commits) != 1:
        raise ValueError(f"board locks disagree on {NAME}: {sorted(commits)}")
    return commits.pop()


def checkout(root: Path) -> Path:
    """A verified clone of the pinned library, made once per commit."""
    tag = pinned_tag(root)
    commit = locked_commit(root)
    cache = Path(os.environ.get("XDG_CACHE_HOME", Path.home() / ".cache"))
    target = cache / "opensensor-space-firmware/embedded-h3-location" / commit
    if not (target / ".git").is_dir():
        target.parent.mkdir(parents=True, exist_ok=True)
        partial = target.with_name(f"{commit}.partial")
        subprocess.run(["rm", "-rf", str(partial)], check=True)
        subprocess.run(["git", "-c", "advice.detachedHead=false", "clone", "--quiet", "--depth", "1", "--branch",
                        tag, URL, str(partial)], check=True)
        partial.rename(target)
    head = subprocess.run(["git", "-C", str(target), "rev-parse", "HEAD"],
                          check=True, capture_output=True, text=True).stdout.strip()
    if head != commit:
        raise ValueError(f"{NAME} {tag} is {head}, the board locks say {commit}")
    tagged = subprocess.run(["git", "-C", str(target), "rev-parse", f"{tag}^{{commit}}"],
                            check=True, capture_output=True, text=True).stdout.strip()
    if tagged != commit:
        raise ValueError(f"{NAME} tag {tag} is {tagged}, the board locks say {commit}")
    dirty = subprocess.run(["git", "-C", str(target), "status", "--porcelain"],
                           check=True, capture_output=True, text=True).stdout
    if dirty:
        raise ValueError(f"{target} has local changes")
    verify_vendor(target / "vendor/h3")
    return target


def verify_vendor(vendor: Path) -> None:
    """Check the vendored H3 against its manifest, as the library CMake does."""
    manifest = json.loads((vendor / "manifest.json").read_text())
    assert manifest["tag"] == "v4.5.0"
    assert manifest["commit"] == "1b536c34225191ba24a75a840f634d4a48c3b206"
    present = {path.relative_to(vendor).as_posix()
               for path in vendor.rglob("*") if path.is_file()}
    present -= {"manifest.json"}
    present = {name for name in present if not name.endswith(".DS_Store")}
    for relative, expected in manifest["files"].items():
        present.discard(relative)
        actual = hashlib.sha256((vendor / relative).read_bytes()).hexdigest()
        if actual != expected:
            raise ValueError(f"Vendored H3 hash mismatch: {relative}")
    if present:
        raise ValueError(f"Vendored H3 file not in manifest.json: {sorted(present)}")


def compile_library(root: Path, directory: Path, sanitize: bool = False,
                    alloc_prefix: str = "") -> tuple[list[str], list[str]]:
    """Compile the library into `directory`, return (objects, include flags).

    The include flags name the library header and the firmware location policy
    header (firmware/common/location/src). alloc_prefix is the H3 allocator
    prefix, empty for plain malloc. The device builds use os_h3_.
    """
    library = checkout(root)
    vendor = library / "vendor/h3"
    build = directory / "embedded-h3-location"
    (build / "include").mkdir(parents=True)
    version = (vendor / "VERSION").read_text().strip()
    parts = re.fullmatch(r"(\d+)\.(\d+)\.(\d+)", version)
    if parts is None:
        raise ValueError(f"vendor/h3/VERSION is not X.Y.Z: {version}")
    header = (vendor / "h3lib/include/h3api.h.in").read_text()
    for name, value in zip(("MAJOR", "MINOR", "PATCH"), parts.groups(), strict=True):
        header = header.replace(f"@H3_VERSION_{name}@", value)
    (build / "include/h3api.h").write_text(header)
    if alloc_prefix and re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", alloc_prefix) is None:
        raise ValueError(f"not a C identifier prefix: {alloc_prefix!r}")
    prefix = ("-DH3_PREFIX=", f"-DH3_ALLOC_PREFIX={alloc_prefix}") if alloc_prefix else ()
    private = ("-I", str(library / "src"), "-I", str(vendor / "h3lib/include"),
               "-I", str(build / "include"))
    extra = SANITIZE if sanitize else ()
    manifest = json.loads((vendor / "manifest.json").read_text())
    sources = [vendor / name for name in sorted(manifest["files"])
               if re.fullmatch(r"h3lib/lib/[^/]+\.c", name)]
    objects = []
    for source in sources:
        output = build / f"{source.stem}.o"
        subprocess.run(["clang", *H3_FLAGS, *private, *prefix, *extra,
                        "-c", str(source), "-o", str(output)], check=True)
        objects.append(str(output))
    for source in ("location.cpp", "multipolygon.cpp"):
        output = build / f"{Path(source).stem}.lib.o"
        subprocess.run(["clang++", CXX_STD, "-O1", "-Wall", "-Wextra", "-Wpedantic",
                        "-Werror", "-fno-exceptions", "-fno-rtti",
                        "-I", str(library / "include"), *private, *prefix, *extra,
                        "-c", str(library / "src" / source), "-o", str(output)],
                       check=True)
        objects.append(str(output))
    includes = ["-I", str(library / "include"),
                "-I", str(root / "firmware/common/location/src")]
    return objects, includes


def library_flags(root: Path, directory: Path, sanitize: bool = False,
                  alloc_prefix: str = "") -> list[str]:
    """compile_library as one list of include flags and objects to link."""
    objects, includes = compile_library(root, directory, sanitize, alloc_prefix)
    return [*includes, *objects]


if __name__ == "__main__":
    print(checkout(Path(__file__).resolve().parent.parent))
