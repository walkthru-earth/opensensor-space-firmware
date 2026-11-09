"""Host checks for cells_to_multipolygon in firmware/common/location.

Compiles the pinned H3 4.5.0 sources with H3_ALLOC_PREFIX=os_h3_, the device
setting, so a counting allocator in the test sees every H3 allocation. Proves
shapes, winding, closure, error mapping, buffer bounds, cleanup after every
failed allocation and bit equality with cellsToLinkedMultiPolygon, the call
h3-js makes. Prints the worst case heap for 720 cells, on a 64-bit host.
Synthetic tests prove no board behavior.
"""

from __future__ import annotations

import argparse
from pathlib import Path
import subprocess
import tempfile

from host_cxx import CXX_STD
from test_location import compile_h3

ALLOC_FLAGS = ("-DH3_PREFIX=", "-DH3_ALLOC_PREFIX=os_h3_")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sanitize", action="store_true")
    args = parser.parse_args()
    root = Path(__file__).resolve().parent.parent
    helper = root / "firmware/common/location/src"
    vendor_include = root / "firmware/common/location/vendor/h3/h3lib/include"
    with tempfile.TemporaryDirectory(prefix="os-h3-polygon-test-") as temporary:
        directory = Path(temporary)
        objects = compile_h3(root, directory, args.sanitize, ALLOC_FLAGS)
        executable = directory / "h3-polygon-test"
        command = ["clang++", CXX_STD, "-Wall", "-Wextra", "-Werror", *ALLOC_FLAGS,
                   "-I", str(helper), "-I", str(directory / "h3"),
                   "-I", str(vendor_include),
                   str(root / "tools/test_h3_polygon.cpp"),
                   str(helper / "os_location.cpp"), *objects, "-o", str(executable)]
        if args.sanitize:
            command += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
        subprocess.run(command, check=True)
        subprocess.run([str(executable)], check=True)


if __name__ == "__main__":
    main()
