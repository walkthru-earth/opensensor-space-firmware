"""Host tests for the board-neutral GNSS components, firmware/common/gnss
and firmware/common/gnss_reader.

Covers the SIMCom CGNSSINFO dialect, fix to H3 cell, freshness, the per row
location source rule, that a GNSS cell never splits files and that a
deployment id change does (#148). Source checks, each with mutants, keep a
raw receiver line and position out of the panic core dump (#172). Compiles the real os_gnss.cpp,
device_config.cpp and the pinned H3 library. It does not prove a receiver
fix, UART timing or antenna placement on a board.
"""

from __future__ import annotations

import argparse
import json
from pathlib import Path
import re
import subprocess
import tempfile

import check_idf_target
from host_cxx import CXX_STD
from h3_library import library_flags


def check_wiring(root: Path) -> None:
    """The logger keeps the GNSS cell out of the location snapshot, so it
    cannot split files, and common code names no board or pin."""
    logger = (root / "firmware/common/logger/src/os_logger.cpp").read_text()
    body = logger[logger.index("void collect(std::int64_t now, std::int64_t scheduled)"):]
    body = body[:body.index("\n}\n")]
    assert body.count("hooks.gnss_fix(") == 1, "collect calls the GNSS hook once"
    assert "select_location(row.location, fix, now)" in body, "collect applies the source rule"
    assigned = re.findall(r"\brow\.location(?:\.\w+)*\s*=(?!=)\s*([^;]*);", body)
    assert assigned == ["labels.location"], \
        "nothing but the settings snapshot is assigned to row.location"
    assert body.count("const auto labels = ::config::sample_labels();") == 1, \
        "collect takes the location from one settings snapshot"
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

def function_body(text: str, signature: str) -> str:
    start = text.index(signature)
    return text[start:text.index("\n}\n", start)]


def check_reader_dump(reader: str) -> None:
    """The panic core dump saves task stacks, not .bss or the heap (#172).
    The reply line stays in one .bss buffer, each parsed Reading is wiped on
    exit, and the dead parse and H3 frames are scrubbed before the task
    blocks again. tools/stack_budget.py proves the scrub frame is deep
    enough on a board build."""
    buffers = re.findall(r"std::array<char,\s*kLineBytes>\s*(&?)\s*(\w+)", reader)
    assert sorted(buffers) == [("", "line_buffer"), ("&", "buffer")], \
        f"the reply line has one .bss home, found {buffers}"
    assert re.search(r"^std::array<char, kLineBytes> line_buffer\{\};$", reader, re.M), \
        "line_buffer is declared at namespace scope"
    scrub = function_body(reader, "void scrub_stack() {")
    assert "[[gnu::noinline]] void scrub_stack() {" in reader, "the scrub keeps its own frame"
    assert re.search(r"std::array<unsigned char, kScrubBytes> (\w+);\n\s*wipe_bytes\(\1\);", scrub), \
        "the scrub zeroes a local array of kScrubBytes"
    assert "asm volatile" in scrub, "the scrub array is kept"
    # Every parse runs in a frame its caller scrubs right after.
    assert reader.count("parse_fix(") == 2, "a parse outside the scrubbed paths"
    poll = function_body(reader, "bool poll_fix(const Dialect &dialect, State &shown) {")
    stream = function_body(reader, "stream_line(const Dialect &dialect, std::size_t length) {")
    for name, body in (("poll_fix", poll), ("stream_line", stream)):
        assert "parse_fix(" in body, f"{name} parses the line"
        assert re.search(r"auto reading = dialect\.parse_fix\([^;]*\);\n\s*const WipeOnExit "
                         r"wipe_reading\(reading\);", body), f"{name} wipes its Reading on exit"
    assert "[[gnu::noinline]] std::optional<std::optional<Fix>>\nstream_line(" in reader, \
        "stream_line keeps the parse below stream_fix"
    exchange = function_body(reader, "Reply exchange(std::string_view command")
    assert re.search(r"on_line\(line\);\n\s*wipe_bytes\(line_buffer\);\n\s*scrub_stack\(\);", exchange), \
        "exchange scrubs after every line"
    loop = function_body(reader, "void stream_fix(const Dialect &dialect, State &shown) {")
    assert re.search(r"= stream_line\(dialect, length\);\n\s*wipe_bytes\(line_buffer\);\n"
                     r"\s*scrub_stack\(\);", loop), "stream_fix scrubs after every line"
    for body in (exchange, loop):
        assert "const WipeOnExit wipe_buffer(line_buffer);" in body, "the line is wiped on every exit"


def check_dump_config(settings: list[str], defaults: dict[str, str], budgets: dict) -> None:
    """A dump that captured DRAM would save .bss, line_buffer included."""
    assert "# CONFIG_ESP_COREDUMP_CAPTURE_DRAM is not set" in settings, \
        "check_idf_target.py fails a board build that captures DRAM"
    for name, text in defaults.items():
        assert not re.search(r"^CONFIG_ESP_COREDUMP_CAPTURE_DRAM=y", text, re.M), \
            f"{name} captures DRAM in the core dump"
    scrub = budgets["tasks"]["os-gnss"].get("scrub", {})
    assert scrub.get("function", "").endswith("scrub_stack()"), "stack_budget.json checks the scrub"
    assert any("parse_fix" in item for item in scrub.get("covers", [])), "the scrub covers the parse"
    assert any("to_fix" in item for item in scrub.get("covers", [])), "the scrub covers the H3 conversion"


def must_fail(check, *args) -> None:
    try:
        check(*args)
    except AssertionError:
        return
    raise AssertionError(f"{check.__name__} accepted a mutant")


def check_core_dump(root: Path) -> None:
    reader = (root / "firmware/common/gnss_reader/src/os_gnss_reader.cpp").read_text()
    check_reader_dump(reader)
    mutants = [
        reader.replace("  const WipeOnExit wipe_buffer(line_buffer);\n",
                       "  std::array<char, kLineBytes> buffer{};\n  const WipeOnExit wipe_buffer(line_buffer);\n", 1),
        reader.replace("    on_line(line);\n    wipe_bytes(line_buffer);\n    scrub_stack();\n",
                       "    on_line(line);\n    wipe_bytes(line_buffer);\n", 1),
        reader.replace("    const auto fix = stream_line(dialect, length);\n    wipe_bytes(line_buffer);\n"
                       "    scrub_stack();\n", "    const auto fix = stream_line(dialect, length);\n"
                       "    wipe_bytes(line_buffer);\n", 1),
        reader.replace("[[gnu::noinline]] void scrub_stack()", "void scrub_stack()", 1),
        reader.replace("  wipe_bytes(frame);\n", "", 1),
        reader.replace("    const WipeOnExit wipe_reading(reading);\n", "", 1),
        reader.replace("[[gnu::noinline]] std::optional<std::optional<Fix>>", "std::optional<std::optional<Fix>>", 1),
        reader.replace("  std::size_t length = 0;\n", "  std::size_t length = 0;\n"
                       "  const auto extra = settings.dialect->parse_fix({});\n", 1),
    ]
    for mutant in mutants:
        assert mutant != reader, "a mutant did not apply"
        must_fail(check_reader_dump, mutant)

    defaults = {str(path.relative_to(root)): path.read_text()
                for path in [*root.glob("tools/*sdkconfig.defaults"), *root.glob("firmware/**/sdkconfig.defaults*"),
                             *root.glob("tools/fixtures/**/sdkconfig.defaults*")]}
    assert "tools/idf-sdkconfig.defaults" in defaults, "the shared sdkconfig defaults are scanned"
    budgets = json.loads((root / "tools/stack_budget.json").read_text())
    settings = list(check_idf_target.COREDUMP_SETTINGS)
    check_dump_config(settings, defaults, budgets)
    must_fail(check_dump_config, [item for item in settings if "CAPTURE_DRAM" not in item], defaults, budgets)
    must_fail(check_dump_config, settings, {**defaults, "x": "CONFIG_ESP_COREDUMP_CAPTURE_DRAM=y\n"}, budgets)
    dropped = json.loads(json.dumps(budgets))
    del dropped["tasks"]["os-gnss"]["scrub"]
    must_fail(check_dump_config, settings, defaults, dropped)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sanitize", action="store_true")
    args = parser.parse_args()
    root = Path(__file__).resolve().parent.parent
    check_wiring(root)
    check_core_dump(root)
    common = root / "firmware/common"
    with tempfile.TemporaryDirectory(prefix="os-gnss-test-") as temporary:
        directory = Path(temporary)
        library = library_flags(root, directory, args.sanitize)
        executable = directory / "gnss-test"
        command = ["clang++", CXX_STD, "-Wall", "-Wextra", "-Werror", "-pthread",
                   "-I", str(root / "tools/host_fakes/config"),
                   "-I", str(common / "gnss/src"),
                   "-I", str(common / "logger/src"),
                   "-I", str(common / "runtime/src"),
                   "-I", str(common / "src"),
                   str(root / "tools/test_gnss.cpp"),
                   str(common / "gnss/src/os_gnss.cpp"),
                   str(common / "runtime/src/device_config.cpp"),
                   str(common / "runtime/src/debug_log.cpp"),
                   str(root / "tools/host_fakes/config/os_console.cpp"),
                   *library, "-o", str(executable)]
        if args.sanitize:
            command += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
        subprocess.run(command, check=True)
        subprocess.run([str(executable)], check=True)
    print("GNSS: SIMCom dialect, fix to cell, freshness, source rule, no file split and core dump "
          "exposure passed")


if __name__ == "__main__":
    main()
