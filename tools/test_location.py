"""Compile the real pinned H3 C library and exercise the location privacy helper."""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import tempfile
from host_cxx import CXX_STD


def compile_h3(root: Path, directory: Path, sanitize: bool = False,
               extra_flags: tuple[str, ...] = ()) -> list[str]:
    """Shared host-test linkage; validate pinned vendor bytes, compile C as C.

    extra_flags go to every H3 source, for example the H3_ALLOC_PREFIX
    allocator hook the H3 polygon test counts allocations with.
    """
    vendor = root / "firmware/common/location/vendor/h3"
    manifest = json.loads((vendor / "manifest.json").read_text())
    assert manifest["tag"] == "v4.5.0"
    assert manifest["commit"] == "1b536c34225191ba24a75a840f634d4a48c3b206"
    for relative, expected in manifest["files"].items():
        actual = hashlib.sha256((vendor / relative).read_bytes()).hexdigest()
        if actual != expected:
            raise ValueError(f"Vendored H3 hash mismatch: {relative}")
    h3_build = directory / "h3"
    h3_build.mkdir()
    header = (vendor / "h3lib/include/h3api.h.in").read_text()
    for part, version in zip(("MAJOR", "MINOR", "PATCH"), (4, 5, 0), strict=True):
        header = header.replace(f"@H3_VERSION_{part}@", str(version))
    (h3_build / "h3api.h").write_text(header)
    objects = []
    for source in sorted((vendor / "h3lib/lib").glob("*.c")):
        object_path = h3_build / f"{source.stem}.o"
        command = ["clang", "-std=c99", "-O1", "-Wall", "-Wextra", "-Werror",
                   "-Wno-sign-compare", "-Wno-deprecated-declarations",
                   "-I", str(vendor / "h3lib/include"), "-I", str(h3_build),
                   *extra_flags, "-c", str(source), "-o", str(object_path)]
        if sanitize:
            command += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
        subprocess.run(command, check=True)
        objects.append(str(object_path))
    return objects


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sanitize", action="store_true")
    args = parser.parse_args()
    root = Path(__file__).resolve().parent.parent
    helper = root / "firmware/common/location/src"
    with tempfile.TemporaryDirectory(prefix="aq-location-test-") as temporary:
        directory = Path(temporary)
        objects = compile_h3(root, directory, args.sanitize)
        executable = directory / "location-test"
        command = ["clang++", CXX_STD, "-Wall", "-Wextra", "-Werror",
                   "-I", str(helper),
                   "-I", str(root / "firmware/common/logger/src"),
                   "-I", str(root / "firmware/common/runtime/src"),
                   "-I", str(root / "firmware/common/src"),
                   str(root / "tools/test_location.cpp"),
                   str(helper / "os_location.cpp"), "-I", str(directory / "h3"),
                   *objects, "-o", str(executable)]
        if args.sanitize:
            command += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
        subprocess.run(command, check=True)
        subprocess.run([str(executable)], check=True)
    print("Location: pinned H3 fixtures, invalid indexes, privacy coarsening, centers and countries passed")


if __name__ == "__main__":
    main()
