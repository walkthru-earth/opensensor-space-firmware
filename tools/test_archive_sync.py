"""Synthetic storage/transport tests for shared BLE/LAN immutable-file sync.

This does not establish real radio, SD or power-loss behavior.
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
    with tempfile.TemporaryDirectory(prefix="aq-archive-test-") as directory:
        executable = Path(directory) / "archive-test"
        command = [
            "clang++", CXX_STD, "-Wall", "-Wextra", "-Werror",
            "-I", str(root / "firmware/common/src"), "-I", str(root / "firmware/common/runtime/src"),
            "-I", str(root / "firmware/common/connectivity/src"),
            "-I", str(root / "firmware/common/logger/src"),
            str(root / "tools/test_archive_sync.cpp"),
            str(root / "firmware/common/connectivity/src/archive_sync.cpp"),
            "-o", str(executable),
        ]
        if args.sanitize:
            command += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
        subprocess.run(command, check=True)
        subprocess.run([str(executable), directory], check=True)
    print("Shared BLE/LAN archive framing, bounds, generations, ABORT of LIST and READ, stale markers, 70000-entry u32 LIST_END, LIST_SINCE framing, cursor checks and fallback, STAC sidecar LIST, OPEN and READ with refused names and sizes, bounded resumable LIST steps, LIST preemption, LIST worker glue with the deferred Full remount, the close ending a LIST before its remount and the parked request, busy remount, persistent read descriptors, lazy reopen, remount recheck, battery close preempt of LIST and READ with the Retry remount, EMFILE and read-only tests passed")


if __name__ == "__main__":
    main()
