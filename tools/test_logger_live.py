"""Exercise the real shared LIVE formatter: declared keys, order, nulls, decimals, capacity."""
from __future__ import annotations

import argparse
import json
from pathlib import Path
import subprocess
import tempfile
from host_cxx import CXX_STD


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sanitize", action="store_true")
    args = parser.parse_args()
    root = Path(__file__).resolve().parent.parent
    common = root / "firmware/common"
    with tempfile.TemporaryDirectory(prefix="aq-logger-live-") as directory:
        executable = Path(directory) / "live-test"
        command = ["clang++", CXX_STD, "-Wall", "-Wextra", "-Werror"]
        for library in ["src", "runtime/src", "connectivity/src", "logger/src", "power/src"]:
            command += ["-I", str(common / library)]
        command += [str(common / "logger/tests/test_live.cpp"),
                    str(common / "logger/src/os_logger_live.cpp"),
                    "-o", str(executable)]
        if args.sanitize:
            command += ["-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                        "-fno-omit-frame-pointer"]
        subprocess.run(command, check=True)
        result = subprocess.run([str(executable)], capture_output=True, text=True, check=True)
    documents = [json.loads(line, object_pairs_hook=list) for line in result.stdout.splitlines()]
    assert len(documents) == 3
    declared = ["seq", "pm25", "t", "pm1", "utc", "rh"]
    for document in documents:
        assert [key for key, _ in document] == declared
    assert dict(documents[0])["utc"] is None and dict(documents[0])["rh"] is None
    assert dict(documents[1])["utc"] == 2**63 - 1
    assert all(value is None for _, value in documents[2])
    print("Shared LIVE formatter: C++ asserts passed for keys, per field decimals, capacity, unscaled UTC microseconds and "
          "the all-null overflow. Its three printed documents parse as JSON with every declared key in "
          "field-table order and null for no reading")


if __name__ == "__main__":
    main()
