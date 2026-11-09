"""Build the pinned embedded-h3-location library and check the firmware
location policy over it, res 8 publishing, the official H3 fixtures and the
logger mapping."""

from __future__ import annotations

import argparse
from pathlib import Path
import subprocess
import tempfile

from h3_library import compile_library
from host_cxx import CXX_STD


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sanitize", action="store_true")
    args = parser.parse_args()
    root = Path(__file__).resolve().parent.parent
    with tempfile.TemporaryDirectory(prefix="os-location-test-") as temporary:
        directory = Path(temporary)
        objects, includes = compile_library(root, directory, args.sanitize)
        executable = directory / "location-test"
        command = ["clang++", CXX_STD, "-Wall", "-Wextra", "-Werror", *includes,
                   "-I", str(root / "firmware/common/logger/src"),
                   "-I", str(root / "firmware/common/runtime/src"),
                   "-I", str(root / "firmware/common/src"),
                   str(root / "tools/test_location.cpp"),
                   *objects, "-o", str(executable)]
        if args.sanitize:
            command += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
        subprocess.run(command, check=True)
        subprocess.run([str(executable)], check=True)
    print("Location: pinned library, H3 fixtures, invalid indexes, res 8 policy, centers and countries passed")


if __name__ == "__main__":
    main()
