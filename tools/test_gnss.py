"""Host tests for the board-neutral GNSS components, firmware/common/gnss
and firmware/common/gnss_reader.

Covers the SIMCom CGNSSINFO dialect, fix to H3 cell, freshness, the per row
location source rule and that a GNSS cell never splits files. Compiles the
real os_gnss.cpp, device_config.cpp and the pinned H3 library. It does not
prove a receiver fix, UART timing or antenna placement on a board.
"""

from __future__ import annotations

import argparse
from pathlib import Path
import re
import subprocess
import tempfile

from host_cxx import CXX_STD
from test_location import compile_h3


def check_wiring(root: Path) -> None:
    """The logger keeps the GNSS cell out of the location snapshot, so it
    cannot split files, and common code names no board or pin."""
    logger = (root / "firmware/common/logger/src/os_logger.cpp").read_text()
    body = logger[logger.index("void collect(std::int64_t now, std::int64_t scheduled)"):]
    body = body[:body.index("\n}\n")]
    assert body.count("hooks.gnss_fix(") == 1, "collect calls the GNSS hook once"
    assert "select_location(row.location, fix, now)" in body, "collect applies the source rule"
    assigned = re.findall(r"\brow\.location(?:\.\w+)*\s*=(?!=)\s*([^;]*);", body)
    assert assigned == ["::config::get().location"], \
        "nothing but the settings snapshot is assigned to row.location"
    # Page 1 location.gnss_cell (#147) goes through the same board-neutral
    # hook and the res 8 readback, only when the board has a receiver.
    assert "control_replies.gnss_cell = hooks.gnss_fix ? gnss_readback : nullptr;" in logger
    readback = logger[logger.index("std::uint64_t gnss_readback(void *)"):]
    readback = readback[:readback.index("\n}\n")]
    assert "readback_cell(fix, esp_timer_get_time())" in readback, \
        "page 1 shows only a fresh res 8 readback"
    for component in ("gnss", "gnss_reader"):
        directory = root / "firmware/common" / component
        for path in sorted((directory / "src").glob("*")):
            text = path.read_text()
            assert not re.search(r"GPIO_NUM_\d|UART_NUM_\d|waveshare|cores3|m5stack", text, re.I), \
                f"{path.name} names a board, pin or UART"
        cmake = (directory / "CMakeLists.txt").read_text()
        assert "os_pin_cxx_standard(${COMPONENT_LIB})" in cmake, f"{component} pins the C++ standard"
        assert "os_map_source_paths(${COMPONENT_LIB})" in cmake, f"{component} maps source paths"
    # The logger links the pure rules only, so a board without a receiver
    # never compiles the reader task.
    logger_cmake = (root / "firmware/common/logger/CMakeLists.txt").read_text()
    assert "gnss_reader" not in logger_cmake, "the logger does not require the reader task"

def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sanitize", action="store_true")
    args = parser.parse_args()
    root = Path(__file__).resolve().parent.parent
    check_wiring(root)
    common = root / "firmware/common"
    with tempfile.TemporaryDirectory(prefix="os-gnss-test-") as temporary:
        directory = Path(temporary)
        objects = compile_h3(root, directory, args.sanitize)
        executable = directory / "gnss-test"
        command = ["clang++", CXX_STD, "-Wall", "-Wextra", "-Werror", "-pthread",
                   "-I", str(root / "tools/host_fakes/config"),
                   "-I", str(common / "gnss/src"),
                   "-I", str(common / "location/src"),
                   "-I", str(common / "logger/src"),
                   "-I", str(common / "runtime/src"),
                   "-I", str(common / "src"),
                   "-I", str(directory / "h3"),
                   str(root / "tools/test_gnss.cpp"),
                   str(common / "gnss/src/os_gnss.cpp"),
                   str(common / "location/src/os_location.cpp"),
                   str(common / "runtime/src/device_config.cpp"),
                   str(common / "runtime/src/debug_log.cpp"),
                   str(root / "tools/host_fakes/config/os_console.cpp"),
                   *objects, "-o", str(executable)]
        if args.sanitize:
            command += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
        subprocess.run(command, check=True)
        subprocess.run([str(executable)], check=True)
    print("GNSS: SIMCom dialect, fix to cell, freshness, source rule and no file split passed")


if __name__ == "__main__":
    main()
