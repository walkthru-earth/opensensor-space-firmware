"""Exercise real shared configuration/log code against host platform fakes.

Settings persist as one CRC-checked NVS blob. Old per-key values are never read.
Failure tests establish runtime atomicity, fallback and error reporting, not
NVS power-loss durability on hardware.
"""

from __future__ import annotations

import argparse
import json
from pathlib import Path
import subprocess
import tempfile

from test_location import compile_h3
from host_cxx import CXX_STD


# GET_CONFIG frames carry at most 480 bytes of JSON (ble::kMaxJson), and the
# reply must stay below it for the NUL. Every owner key at its maximum with
# every escapable character must still fit (#148).
MAX_JSON = 480


def check_owner_pages(stdout: str) -> None:
    page5, page6 = stdout.splitlines()
    for page in (page5, page6):
        assert len(page.encode()) < MAX_JSON, len(page.encode())
    owner = json.loads(page5)
    assert owner["owner"]["title"] == '"' * 64
    assert owner["owner"]["provider"] == "\\" * 64
    assert owner["owner"]["url"] == "https://" + "u" * 88
    assert owner["thing"]["deployment_id"] == "z" * 32
    assert json.loads(page6) == {"owner": {"description": "\\" * 160}}


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sanitize", action="store_true")
    args = parser.parse_args()
    root = Path(__file__).resolve().parent.parent
    shared = root / "firmware/common/runtime/src"
    with tempfile.TemporaryDirectory(prefix="aq-config-test-") as directory:
        executable = Path(directory) / "config-test"
        command = [
            "clang++", CXX_STD, "-Wall", "-Wextra", "-Werror", "-pthread",
            "-I", str(root / "tools/host_fakes/config"), "-I", str(shared),
            str(root / "tools/test_device_config.cpp"),
            str(shared / "device_config.cpp"), str(shared / "debug_log.cpp"),
            str(root / "tools/host_fakes/config/os_console.cpp"),
            "-o", str(executable),
        ]
        if args.sanitize:
            command += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
        location = root / "firmware/common/location/src"
        gnss = root / "firmware/common/gnss/src"
        command += ["-I", str(Path(directory) / "h3"), "-I", str(location), str(location / "os_location.cpp"),
                    "-I", str(gnss), str(gnss / "os_gnss.cpp"),
                    "-I", str(root / "firmware/common/logger/src"),
                    "-I", str(root / "firmware/common/src"),
                    *compile_h3(root, Path(directory), args.sanitize)]
        subprocess.run(command, check=True)
        for scenario in ("headless", "screen", "migration", "validation",
                         "credentials", "sensor", "nvs", "unavailable", "time-location",
                         "setup", "blob", "old-keys", "torn", "owner",
                         "owner-unavailable"):
            result = subprocess.run([str(executable), scenario],
                                    capture_output=True, text=True)
            if result.returncode:
                raise RuntimeError(f"{scenario}: {result.stderr}")
            if scenario == "credentials":
                document = json.loads(result.stdout)
                assert document["wifi"]["ssid"] == 'fixture"\\name'
                assert document["wifi"]["psk_set"] == 1
                assert "pin" not in document["ble"]
                assert "psk" not in document["wifi"]
                assert "token" not in document["lan"]
            if scenario == "setup":
                # Worst-case GET_CONFIG page 1 (#147) within the 480 byte cap.
                page_one = int(result.stdout)
                assert page_one < 480, page_one
                print(f"GET_CONFIG page 1 worst case {page_one} B of 480")
            if scenario == "owner":
                check_owner_pages(result.stdout)
    print("Shared configuration: defaults, PIN rotation, validation, secrecy, "
          "blob, ignored old keys, owner keys and NVS failures passed")


if __name__ == "__main__":
    main()
