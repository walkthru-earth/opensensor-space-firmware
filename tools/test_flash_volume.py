"""Build and run the internal flash volume format-once policy tests on the host."""

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
    with tempfile.TemporaryDirectory(prefix="os-flash-volume-") as directory:
        executable = Path(directory) / "flash-volume-test"
        command = ["clang++", CXX_STD, "-Wall", "-Wextra", "-Werror",
                   str(root / "tools/test_flash_volume.cpp"),
                   str(root / "firmware/common/src/flash_volume_policy.cpp"),
                   "-o", str(executable)]
        if args.sanitize:
            command += ["-fsanitize=address,undefined", "-fno-sanitize-recover=all"]
        subprocess.run(command, check=True)
        subprocess.run([str(executable)], check=True)
    print("flash volume mount without format, transient failure retried, "
          "one format then flag, flag set or "
          "unreadable refuses format, unformattable errors, failed format, "
          "flag save failure, unknown space and line width passed")


if __name__ == "__main__":
    main()
