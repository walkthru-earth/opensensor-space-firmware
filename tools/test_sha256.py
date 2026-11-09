"""Host checks for the streaming SHA-256, firmware/common/src/sha256_stream.h.

The host build is the FIPS 180-4 reference. The C++ test checks the NIST
examples, chunking and the state rules, then prints digests that this script
compares with Python hashlib. The device build uses PSA Crypto and is not
covered here.
"""

from __future__ import annotations

import argparse
import hashlib
from pathlib import Path
import subprocess
import tempfile

from host_cxx import CXX_STD


def pattern(length: int) -> bytes:
    """The same LCG bytes as pattern() in test_sha256.cpp."""
    out = bytearray()
    state = 0x12345678
    for _ in range(length):
        state = (state * 1664525 + 1013904223) & 0xFFFFFFFF
        out.append(state >> 24)
    return bytes(out)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sanitize", action="store_true")
    args = parser.parse_args()
    root = Path(__file__).resolve().parent.parent
    with tempfile.TemporaryDirectory(prefix="os-sha256-test-") as directory:
        executable = Path(directory) / "sha256-test"
        command = [
            "clang++", CXX_STD, "-Wall", "-Wextra", "-Werror",
            "-I", str(root / "firmware/common/src"),
            str(root / "tools/test_sha256.cpp"),
            str(root / "firmware/common/src/sha256_stream.cpp"),
            "-o", str(executable),
        ]
        if args.sanitize:
            command += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
        subprocess.run(command, check=True)
        output = subprocess.run([str(executable)], check=True, capture_output=True, text=True).stdout
    lines = output.splitlines()
    if len(lines) != 205:
        raise SystemExit(f"expected 205 digests, got {len(lines)}")
    for line in lines:
        length_text, digest = line.split(" ")
        expected = hashlib.sha256(pattern(int(length_text))).hexdigest()
        if digest != expected:
            raise SystemExit(f"length {length_text} want {expected} got {digest}")
    print(f"sha256: NIST examples, chunking, state rules and {len(lines)} hashlib digests passed")


if __name__ == "__main__":
    main()
