"""Build and run the LAN OTA transfer rule tests on the host."""

from __future__ import annotations

import argparse
from pathlib import Path
import re
import subprocess
import tempfile
from host_cxx import CXX_STD

# The device glue must route its stale owner decisions through the pure
# helpers the C++ test covers, so dropping a call cannot pass unseen.
GLUE_CALLS = (
    re.compile(r"const auto generation = rules::owner_generation\(owner, ble::kMaxLanClients,\s*lan::connection_generation\);"),
    re.compile(r"const auto keep =\s*rules::stale_action\(progress, owner, generation, now_us,\s*owner_seen_us\);"),
    re.compile(r"owner_seen_us = rules::touch_seen\(upload\.progress\(\), upload\.owner\(\), owner,\s*now_us, owner_seen_us\);"),
    re.compile(r"return rules::lan_owner\(request\.peer, request\.link_generation\);"),
)


def check_glue(root: Path) -> None:
    text = (root / "firmware/common/connectivity/src/ota_lan.cpp").read_text(encoding="utf-8")
    missing = [call.pattern for call in GLUE_CALLS if not call.search(text)]
    if missing:
        raise SystemExit(f"ota_lan.cpp no longer calls the tested OTA helpers, {missing}")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sanitize", action="store_true")
    args = parser.parse_args()
    root = Path(__file__).resolve().parent.parent
    check_glue(root)
    with tempfile.TemporaryDirectory(prefix="os-ota-transfer-") as directory:
        executable = Path(directory) / "ota-transfer-test"
        command = ["clang++", CXX_STD, "-Wall", "-Wextra", "-Werror",
                   "-I", str(root / "firmware/common/src"),
                   str(root / "tools/test_ota_transfer.cpp"),
                   str(root / "firmware/common/src/ota_transfer.cpp"),
                   "-o", str(executable)]
        if args.sanitize:
            command += ["-fsanitize=address,undefined", "-fno-sanitize-recover=all"]
        subprocess.run(command, check=True)
        subprocess.run([str(executable)], check=True)
    print("OTA begin size and slot cap, contiguous chunks, header chip and "
          "project match, SHA-256 before activate, abort on mismatch, "
          "ownership, stale owner glue and STATE frame passed")


if __name__ == "__main__":
    main()
