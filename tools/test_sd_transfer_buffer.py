"""Check SD DMA buffer ownership, reuse under simulated DMA heap pressure and the one unavailable report."""
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
    with tempfile.TemporaryDirectory(prefix="aq-sd-memory-") as directory:
        executable = Path(directory) / "sd-memory-test"
        command = ["clang++", CXX_STD, "-Wall", "-Wextra", "-Werror",
                   "-I", str(root / "tools/sd_memory_host_fakes"),
                   "-I", str(root / "firmware/common/src"),
                   str(root / "tools/test_sd_transfer_buffer.cpp"), "-o", str(executable)]
        if args.sanitize:
            command += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
        subprocess.run(command, check=True)
        subprocess.run([str(executable)], check=True)
    print("SD DMA buffer: ownership, reuse under simulated DMA heap pressure and the one unavailable report passed (no mount ran)")


if __name__ == "__main__":
    main()
