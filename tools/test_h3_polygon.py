"""Host checks for cells_to_multipolygon as the firmware uses it.

Builds the pinned embedded-h3-location library with H3_ALLOC_PREFIX=os_h3_,
the device setting, and links the real device hooks in
firmware/common/location/src/os_h3_alloc.cpp, compiled against the ESP-IDF
heap and sdkconfig fakes in tools/host_fakes/h3_alloc. Proves the hooks,
shapes, winding, closure, error mapping, buffer bounds, cleanup after every
failed allocation, the whole globe through the zero size rounding and bit
equality with cellsToLinkedMultiPolygon, the call h3-js makes. Then drops
nonzero_alloc_size from each hook argument in turn and checks the test fails.
Prints the worst case heap for 720 cells, on a 64-bit host. Synthetic tests
prove no board behavior.
"""

from __future__ import annotations

import argparse
from pathlib import Path
import subprocess
import tempfile

from h3_library import SANITIZE, checkout, compile_library
from host_cxx import CXX_STD

HOOKS = "firmware/common/location/src/os_h3_alloc.cpp"
FAKES = "tools/host_fakes/h3_alloc"
# Each mutation drops nonzero_alloc_size from one hook argument, as
# (name, old text, new text). The old text must appear exactly once in HOOKS.
MUTATIONS = [
    ("malloc size", "heap_caps_malloc(nonzero_alloc_size(size)", "heap_caps_malloc(size"),
    ("calloc count", "nonzero_alloc_size(count)", "count"),
    ("calloc size", "nonzero_alloc_size(count), nonzero_alloc_size(size)",
     "nonzero_alloc_size(count), size"),
    ("realloc size", "heap_caps_realloc(pointer, nonzero_alloc_size(size)",
     "heap_caps_realloc(pointer, size"),
]


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sanitize", action="store_true")
    args = parser.parse_args()
    root = Path(__file__).resolve().parent.parent
    sanitize = list(SANITIZE) if args.sanitize else []
    with tempfile.TemporaryDirectory(prefix="os-h3-polygon-test-") as temporary:
        directory = Path(temporary)
        objects, includes = compile_library(root, directory, args.sanitize, "os_h3_")
        library = checkout(root)
        fakes = ["-I", str(root / FAKES)]
        test_object = directory / "test_h3_polygon.o"
        # The test calls H3 directly for its references, so it sees the H3
        # headers the library compiled with.
        subprocess.run(["clang++", CXX_STD, "-Wall", "-Wextra", "-Werror", *includes, *fakes,
                        "-I", str(directory / "embedded-h3-location/include"),
                        "-I", str(library / "vendor/h3/h3lib/include"), *sanitize,
                        "-c", str(root / "tools/test_h3_polygon.cpp"),
                        "-o", str(test_object)], check=True)

        def build(source: Path, name: str) -> Path:
            hooks_object = directory / f"{name}.o"
            subprocess.run(["clang++", CXX_STD, "-Wall", "-Wextra", "-Werror",
                            "-fno-exceptions", "-fno-rtti", *includes, *fakes, *sanitize,
                            "-c", str(source), "-o", str(hooks_object)], check=True)
            executable = directory / name
            subprocess.run(["clang++", *sanitize, str(test_object), str(hooks_object),
                            *objects, "-o", str(executable)], check=True)
            return executable

        subprocess.run([str(build(root / HOOKS, "h3-polygon-test"))], check=True)

        original = (root / HOOKS).read_text()
        survivors = []
        for index, (name, old, new) in enumerate(MUTATIONS):
            if original.count(old) != 1:
                raise SystemExit(f"mutation {name!r} text is not unique in {HOOKS}")
            mutant = directory / f"mutant-source-{index}" / "os_h3_alloc.cpp"
            mutant.parent.mkdir()
            mutant.write_text(original.replace(old, new))
            result = subprocess.run([str(build(mutant, f"mutant-{index}"))],
                                    capture_output=True, text=True)
            if result.returncode == 0:
                survivors.append(name)
        if survivors:
            raise SystemExit(f"h3 polygon: hook mutations not caught: {survivors}")
        print(f"h3 polygon: {len(MUTATIONS)} hook mutations without nonzero_alloc_size "
              "each fail the test")


if __name__ == "__main__":
    main()
