"""Build and run the fake-bus SEN6x driver tests; no device is accessed."""

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
    with tempfile.TemporaryDirectory(prefix="os-sen6x-test-") as directory:
        executable = Path(directory) / "sen6x-test"
        command = ["clang++", CXX_STD, "-Wall", "-Wextra", "-Werror",
                   str(root / "tools/test_sen6x.cpp"), "-o", str(executable)]
        if args.sanitize:
            command += ["-fsanitize=address,undefined", "-fno-sanitize-recover=all"]
        subprocess.run(command, check=True)
        subprocess.run([str(executable)], check=True)
    print("SEN6x CRC-8, SEN63C and number concentration decode, sentinels, "
          "CRC mismatch, NACK, status bits, identity text, restart window, "
          "unset IO, configuration writes and CO2 conditioning window passed")


if __name__ == "__main__":
    main()
