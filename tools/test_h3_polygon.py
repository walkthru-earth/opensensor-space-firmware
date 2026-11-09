"""Host checks for cells_to_multipolygon as the firmware uses it.

Builds the pinned embedded-h3-location library with H3_ALLOC_PREFIX=os_h3_,
the device setting, so test hooks with the device semantics see every H3
allocation. Proves shapes, winding, closure, error mapping, buffer bounds,
cleanup after every failed allocation, the whole globe through the zero size
rounding and bit equality with cellsToLinkedMultiPolygon, the call h3-js makes.
Prints the worst case heap for 720 cells, on a 64-bit host. Synthetic tests
prove no board behavior.
"""

from __future__ import annotations

import argparse
from pathlib import Path
import subprocess
import tempfile

from h3_library import checkout, compile_library
from host_cxx import CXX_STD


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sanitize", action="store_true")
    args = parser.parse_args()
    root = Path(__file__).resolve().parent.parent
    with tempfile.TemporaryDirectory(prefix="os-h3-polygon-test-") as temporary:
        directory = Path(temporary)
        objects, includes = compile_library(root, directory, args.sanitize, "os_h3_")
        library = checkout(root)
        executable = directory / "h3-polygon-test"
        # The test calls H3 directly for its references, so it sees the H3
        # headers the library compiled with.
        command = ["clang++", CXX_STD, "-Wall", "-Wextra", "-Werror", *includes,
                   "-I", str(directory / "embedded-h3-location/include"),
                   "-I", str(library / "vendor/h3/h3lib/include"),
                   str(root / "tools/test_h3_polygon.cpp"),
                   *objects, "-o", str(executable)]
        if args.sanitize:
            command += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
        subprocess.run(command, check=True)
        subprocess.run([str(executable)], check=True)


if __name__ == "__main__":
    main()
