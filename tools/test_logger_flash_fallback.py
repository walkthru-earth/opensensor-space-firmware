"""Check the flash fallback archive decisions of decision record 0004 on the host.

The archive target choice, the copy back step machine and the LIST merge of
the flash root are pure headers in firmware/common/logger/src. Each has a C++
test built with sanitizers. The target choice and the LIST merge only decide,
so a source check proves they never reach a file API that changes data.
"""
from __future__ import annotations

import argparse
from pathlib import Path
import re
import subprocess
import tempfile
from host_cxx import CXX_STD

# Each test, the headers that must stay free of any data changing call, and
# the extra sources it links.
TESTS = {
    "archive-target": ("test_archive_target.cpp", ["os_logger_archive_target.h"], []),
    "flash-copy": ("test_flash_copy.cpp", [], ["src/sha256_stream.cpp"]),
    "list-merge": ("test_list_merge.cpp", ["os_logger_list_merge.h"], []),
    "roots": ("test_roots.cpp", ["os_logger_roots.h"], []),
}

FORBIDDEN = re.compile(
    r"\b(remove|unlink|rename|rmdir|truncate|ftruncate|f_unlink|f_rename|fopen|open_write)\s*\(")


def check_read_only(header: Path) -> None:
    match = FORBIDDEN.search(header.read_text())
    assert match is None, f"{header.name} calls {match.group(0)}"


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sanitize", action="store_true")
    args = parser.parse_args()
    root = Path(__file__).resolve().parent.parent
    common = root / "firmware/common"
    logger = common / "logger"
    with tempfile.TemporaryDirectory(prefix="os-flash-fallback-") as directory:
        for name, (source, read_only, extra) in TESTS.items():
            for header in read_only:
                check_read_only(logger / "src" / header)
            executable = Path(directory) / f"{name}-test"
            command = ["clang++", CXX_STD, "-Wall", "-Wextra", "-Werror",
                       "-I", str(logger / "src"), "-I", str(common / "src"),
                       str(logger / "tests" / source),
                       *[str(common / path) for path in extra],
                       "-o", str(executable)]
            if args.sanitize:
                command += ["-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                            "-fno-omit-frame-pointer"]
            subprocess.run(command, check=True)
            subprocess.run([str(executable)], check=True)
    print("flash fallback checks passed: archive target for every input and the "
          "reserve boundary, verified copy back with resets, conflicts and "
          "refused renames, the LIST merge of the flash root and the fallback root "
          "overlap rules")


if __name__ == "__main__":
    main()
