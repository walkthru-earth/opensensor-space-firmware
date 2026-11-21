"""The integer encoding switch CONFIG_OS_LOGGER_DELTA_INTEGERS (#135), off and on.

1. Kconfig keeps it default n, no shipped sdkconfig defaults set it, only the
   bench overlay that OS_BENCH=delta appends turns it on, os_logger.cpp
   passes the switch to Writer::begin and prints it on PARQUET BEGIN, and a
   board that decodes its own files with parquet_columns.h, which reads
   DELTA_BINARY_PACKED since #229, no longer refuses the switch.
2. firmware/common/logger/tests/test_encoding.cpp is built twice, as the
   default build and with -DCONFIG_OS_LOGGER_DELTA_INTEGERS=1, and writes
   files through the real writer with and without LZ4_RAW.
3. Off, every file is byte for byte the file the writer default writes.
   On, only INT32 and INT64 chunks say DELTA_BINARY_PACKED, and PyArrow and
   DuckDB read values equal to the off file.
"""
from __future__ import annotations

import argparse
from pathlib import Path
import re
import subprocess
import sys
import tempfile

import duckdb
import pyarrow.parquet as pq

sys.path.insert(0, str(Path(__file__).resolve().parent))
import parquet_footer as footer  # noqa: E402
from host_cxx import CXX_STD  # noqa: E402
from test_logger_codec_store import strip_comments  # noqa: E402

GUARD = "CONFIG_OS_LOGGER_DELTA_INTEGERS"
OVERLAY = "tools/idf-delta-sdkconfig.defaults"
PLAIN, RLE, DELTA_BINARY_PACKED = 0, 3, 5
INT32, INT64 = 1, 2


def check_wiring(root: Path) -> None:
    logger = root / "firmware/common/logger"
    kconfig = (logger / "Kconfig").read_text()
    entry = re.search(r"config OS_LOGGER_DELTA_INTEGERS\n(.*?)(?=\nconfig |\nendmenu)", kconfig, re.S)
    assert entry, "Kconfig lost OS_LOGGER_DELTA_INTEGERS"
    assert re.search(r"^\s+bool ", entry.group(1), re.M), "OS_LOGGER_DELTA_INTEGERS must be a bool"
    assert re.search(r"^\s+default n$", entry.group(1), re.M), "OS_LOGGER_DELTA_INTEGERS must default to n"

    code = " ".join(strip_comments((logger / "src/os_logger.cpp").read_text()).split())
    assert code.count('#include "os_logger_encoding.h"') == 1, "os_logger.cpp must include os_logger_encoding.h"
    begin = re.findall(r"target\.writer\.begin\((.*?)\);", code)
    assert len(begin) == 1, "os_logger.cpp must open every file through one Writer::begin call"
    assert begin[0].rstrip().endswith("encoding::kIntegers"), "Writer::begin must take encoding::kIntegers"
    assert "IntegerEncoding::" not in code, "os_logger.cpp must take the encoding only from the switch"
    assert GUARD not in code, "os_logger.cpp reads the switch only through os_logger_encoding.h"
    banner = re.search(r'"PARQUET BEGIN .*?\\n",(.*?)\);', code)
    assert banner and "integers=%s\\n" in banner.group(0), "PARQUET BEGIN must end with integers=%s"
    assert banner.group(1).rstrip().endswith("encoding::name(encoding::kIntegers)"), \
        "PARQUET BEGIN must print encoding::name(encoding::kIntegers) last"

    # Turning it on for a board is the decision record 0002 step after the
    # board measurement (#61). Until then only the bench overlay sets it.
    shipped = [root / "tools/idf-sdkconfig.defaults", root / "tools/idf-connectivity-sdkconfig.defaults",
               *root.glob("firmware/**/sdkconfig.defaults")]
    for path in shipped:
        if "/build/" in str(path) or "managed_components" in str(path):
            continue
        assert GUARD not in path.read_text(), f"{path.relative_to(root)} must not set {GUARD} before DR 0002 decides"
    overlay = (root / OVERLAY).read_text().splitlines()
    settings = [line for line in overlay if line.strip() and not line.startswith("#")]
    assert settings == [f"{GUARD}=y"], f"{OVERLAY} must only turn on the DELTA switch"
    for script in ("build-idf-board.sh", "flash-idf-board.sh"):
        text = (root / "tools" / script).read_text()
        assert re.search(r'^\s+delta\)\n(?:.*\n){0,4}?\s+build_dir="\$\{build_dir\}-bench-delta"',
                         text, re.M), f"{script} must build OS_BENCH=delta in its own directory"
    build = (root / "tools/build-idf-board.sh").read_text()
    assert "OS_SDKCONFIG_OVERLAY=${repo_root}/" + OVERLAY in build, "OS_BENCH=delta must append the delta overlay"

    # parquet_columns.h decodes PLAIN and DELTA_BINARY_PACKED integers (#229),
    # so a board that reads finalized files back on device (the CrowPanel
    # chart history restore) builds with the switch on or off. The DELTA
    # streams themselves are proven by pixi run reader-test.
    readers = [path for path in root.glob("firmware/esp-idf-*/**/*.cpp")
               if "/build" not in str(path) and "managed_components" not in str(path)
               and "parquet_columns.h" in path.read_text()]
    assert readers, "no board reads Parquet back on device any more, drop this guard"
    for path in readers:
        assert not re.search(rf"#if .*{GUARD}.*\n#error ", path.read_text()), \
            f"{path.relative_to(root)} must not refuse {GUARD}, parquet_columns.h reads DELTA"
    columns = (root / "firmware/common/src/parquet_columns.cpp").read_text()
    assert "kDeltaBinaryPacked = 5" in columns, "parquet_columns.cpp must decode DELTA_BINARY_PACKED"


def encodings(path: Path) -> dict[str, list[list[int]]]:
    """Column name to the encodings list of each of its chunks, from our own footer parser."""
    result: dict[str, list[list[int]]] = {}
    for group in footer.read_footer(path)[footer.FILE_ROW_GROUPS]:
        for chunk in group[footer.GROUP_COLUMNS]:
            meta = chunk[footer.CHUNK_META]
            result.setdefault(meta[footer.META_PATH][0].decode(), []).append(
                (meta[footer.META_TYPE], meta[footer.META_ENCODINGS]))
    return result


def statistics(path: Path) -> list:
    return [chunk[footer.CHUNK_META].get(footer.META_STATISTICS)
            for group in footer.read_footer(path)[footer.FILE_ROW_GROUPS]
            for chunk in group[footer.GROUP_COLUMNS]]


def duck_rows(path: Path) -> list[tuple]:
    with duckdb.connect() as connection:
        return connection.execute(
            "SELECT * REPLACE (epoch_us(event_time_utc_us) AS event_time_utc_us) FROM read_parquet(?)",
            [str(path)]).fetchall()


def build(root: Path, directory: Path, delta: bool, sanitize: bool) -> Path:
    logger = root / "firmware/common/logger"
    executable = directory / ("encoding-on" if delta else "encoding-off")
    command = ["clang++", CXX_STD, "-Wall", "-Wextra", "-Werror", f"-DOS_EXPECT_DELTA={int(delta)}",
               "-I", str(root / "firmware/common/src"), "-I", str(root / "firmware/common/vendor/lz4"),
               "-I", str(logger / "src"),
               str(logger / "tests/test_encoding.cpp"),
               str(root / "firmware/common/src/parquet_writer.cpp"),
               str(root / "firmware/common/src/lz4_codec.cpp"), "-o", str(executable)]
    if delta:
        command.insert(6, f"-D{GUARD}=1")
    if sanitize:
        command += ["-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-fno-omit-frame-pointer"]
    subprocess.run(command, check=True)
    return executable


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sanitize", action="store_true")
    args = parser.parse_args()
    root = Path(__file__).resolve().parent.parent
    check_wiring(root)
    with tempfile.TemporaryDirectory(prefix="os-encoding-") as temporary:
        directory = Path(temporary)
        outputs = {}
        for delta in (False, True):
            executable = build(root, directory, delta, args.sanitize)
            output = directory / ("on" if delta else "off")
            output.mkdir()
            completed = subprocess.run([str(executable), str(output)], check=True, capture_output=True, text=True)
            name = "DELTA_BINARY_PACKED" if delta else "PLAIN"
            assert completed.stdout == f"encoding switch: integers={name}\n", completed.stdout
            outputs[delta] = output
        for codec in ("none", "lz4"):
            off, on = outputs[False] / f"switch-{codec}.parquet", outputs[True] / f"switch-{codec}.parquet"
            # Off is the writer default, byte for byte, in both builds.
            for build_output in outputs.values():
                assert (build_output / f"default-{codec}.parquet").read_bytes() == off.read_bytes(), \
                    f"{codec}: the switch off must write the writer default bytes"
            assert on.read_bytes() != off.read_bytes(), f"{codec}: the switch on must change the integer pages"
            for path, delta in ((off, False), (on, True)):
                chunks = encodings(path)
                assert len(chunks) == 7 and all(len(c) == 2 for c in chunks.values()), f"{path}: 7 columns x 2 groups"
                for column, kinds in chunks.items():
                    for kind, listed in kinds:
                        value = DELTA_BINARY_PACKED if delta and kind in (INT32, INT64) else PLAIN
                        assert listed == [value, RLE], f"{path.name} {column}: encodings {listed}"
            assert statistics(on) == statistics(off), f"{codec}: statistics must not depend on the encoding"
            left, right = pq.read_table(off), pq.read_table(on)
            assert right.num_rows == 180 and right.equals(left), f"{codec}: PyArrow values equal off and on"
            assert right.column("all_null").null_count == 180, f"{codec}: all null INT64 stays null"
            assert duck_rows(on) == duck_rows(off), f"{codec}: DuckDB values equal off and on"
            print(f"PASS encoding switch {codec}: off is the writer default bytes, on writes DELTA integers "
                  "only, equal statistics, PyArrow and DuckDB values", flush=True)
    print("Logger encoding: default-off Kconfig, no shipped default sets it, bench overlay and scripts, "
          "Writer::begin and PARQUET BEGIN wiring, both switch builds passed"
          + (" (ASan + UBSan)" if args.sanitize else ""))


if __name__ == "__main__":
    main()
