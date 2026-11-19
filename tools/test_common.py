"""Host checks for shared numeric rows, UTC helpers and archive wire primitives.

Synthetic tests do not establish board radio, sensor, SD or power-loss behavior.
"""

from __future__ import annotations

import argparse
from pathlib import Path
import subprocess
import tempfile
from host_cxx import CXX_STD


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sanitize", action="store_true")
    args = parser.parse_args()
    root = Path(__file__).resolve().parent.parent
    # The clock headers compile alone, so they never lean on include order.
    with tempfile.TemporaryDirectory(prefix="aq-common-alone-") as directory:
        for header in ("utc_clock.h", "time_sync.h"):
            alone = Path(directory) / f"{Path(header).stem}_alone.cpp"
            alone.write_text(f'#include "{header}"\n')
            subprocess.run(["clang++", CXX_STD, "-Wall", "-Wextra", "-Werror", "-fsyntax-only",
                            "-I", str(root / "firmware/common/src"), str(alone)], check=True)
    # The second build swaps crc32_update to the ROM routine's semantics.
    for variant, extra in (("loop", []), ("rom", ["-DOS_SYNC_ROM_CRC", "-I", str(root / "tools/rom_crc_host_fake")])):
        with tempfile.TemporaryDirectory(prefix="aq-common-test-") as directory:
            executable = Path(directory) / f"common-test-{variant}"
            command = [
                "clang++", CXX_STD, "-Wall", "-Wextra", "-Werror",
                "-I", str(root / "firmware/common/src"), *extra,
                str(root / "tools/test_common.cpp"), "-o", str(executable),
            ]
            if args.sanitize:
                command += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
            subprocess.run(command, check=True)
            subprocess.run([str(executable)], check=True)
    print("Shared sample, UTC, wire, CRC golden vectors (loop and ROM routine) and archive checks passed")


if __name__ == "__main__":
    main()
