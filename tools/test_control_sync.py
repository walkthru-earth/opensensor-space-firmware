"""Check real worker-side shared control handlers with transport/platform fakes.

This establishes frame/security/action behavior, not real radio scheduling.
"""

from __future__ import annotations

import argparse
import hashlib
from pathlib import Path
import re
import subprocess
import tempfile

from h3_library import library_flags
from host_cxx import CXX_STD

BOARDS = {
    "cores3": "firmware/esp-idf-cores3/bringup",
    "waveshare": "firmware/esp-idf-waveshare-sim7670g/logger",
}


def descriptor_sha256(root: Path, board: str) -> str:
    """desc as generated into the board header, never a copy in this test."""
    text = (root / BOARDS[board] / "telemetry_descriptor_digest.h").read_text()
    found = re.findall(r'kDescriptorSha256 =\s*"([0-9a-f]{64})"', text)
    assert len(found) == 1, board
    return found[0]


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sanitize", action="store_true")
    args = parser.parse_args()
    root = Path(__file__).resolve().parent.parent
    common = root / "firmware/common/src"
    runtime = root / "firmware/common/runtime/src"
    connectivity = root / "firmware/common/connectivity/src"
    with tempfile.TemporaryDirectory(prefix="aq-control-test-") as directory:
        executable = Path(directory) / "control-test"
        command = [
            "clang++", CXX_STD, "-Wall", "-Wextra", "-Werror", "-pthread",
            "-I", str(root / "tools/host_fakes/config"),
            "-I", str(common), "-I", str(runtime), "-I", str(connectivity),
            str(root / "tools/test_control_sync.cpp"),
            str(runtime / "device_config.cpp"), str(runtime / "debug_log.cpp"),
            str(root / "tools/host_fakes/config/os_console.cpp"),
            str(connectivity / "control_sync.cpp"), str(connectivity / "archive_sync.cpp"),
            str(connectivity / "disconnect.cpp"), "-o", str(executable),
        ]
        if args.sanitize:
            command += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
        command += library_flags(root, Path(directory), args.sanitize)
        subprocess.run(command, check=True)
        downloads = Path(directory) / "descriptors"
        downloads.mkdir()
        subprocess.run([str(executable), str(downloads)], check=True)
        for board in BOARDS:
            desc = descriptor_sha256(root, board)
            for link in ("ble", "lan"):
                data = (downloads / f"{board}-{link}.json").read_bytes()
                assert hashlib.sha256(data).hexdigest() == desc, (board, link)
    print("Shared control: configuration secrecy/actions, BLE-only token, 65535-byte log slices, GET_DIAG, idle ABORT, ABORT of a running LIST job and of LIST/LOG_TAIL with stale-marker and per-link checks, GET_DESCRIPTOR download at 512 and 1024 matching desc on both boards, ranges, edges and no heap passed")


if __name__ == "__main__":
    main()
