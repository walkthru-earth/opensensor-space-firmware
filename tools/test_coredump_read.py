"""Offline tests for tools/coredump_read.py. No serial port is opened."""

from __future__ import annotations

import json
import os
from pathlib import Path
import stat
import sys
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parent))
import boards  # noqa: E402
import coredump_read as cd  # noqa: E402

TABLE = """# Name, Type, SubType, Offset, Size, Flags
nvs,        data, nvs,     0x9000,  0x5000,
otadata,    data, ota,     0xe000,  0x2000,
app0,       app,  ota_0,   0x10000, 0x600000,
app1,       app,  ota_1,   0x610000,0x600000,
coredump,   data, coredump,0xc10000, 0x10000,
storage,    data, fat,     0xc20000, 0x3e0000,
"""


def layout(root: Path, board: str = "cores3", variant: str = "bringup",
           table: str = TABLE, target: str | None = None) -> None:
    found = boards.find(board)
    folder = root / found.project
    # The registry under root needs the board.json of the telemetry app (#141).
    (root / found.manifest).parent.mkdir(parents=True, exist_ok=True)
    (root / found.manifest).write_bytes((boards.ROOT / found.manifest).read_bytes())
    (folder / variant).mkdir(parents=True, exist_ok=True)
    (folder / variant / "partitions.csv").write_text(table)
    build = folder / "build" / variant
    build.mkdir(parents=True)
    (build / "os-build-target.txt").write_text((target or f"{board}/{variant}") + "\n")
    (build / "project_description.json").write_text(json.dumps({"app_elf": "app.elf"}))
    (build / "app.elf").write_bytes(b"\x7fELF")


def expect_error(action, text: str) -> None:
    try:
        action()
    except cd.PlanError as error:
        assert text in str(error), error
        return
    raise AssertionError(f"expected PlanError {text}")


def test_plan() -> None:
    with tempfile.TemporaryDirectory(prefix="os-coredump-") as name:
        root = Path(name)
        layout(root)
        # Read only by default. The partition stays on the board.
        assert len(cd.plan("cores3", None, "/dev/cu.x", "py", root=root)) == 1
        read, erase = cd.plan("cores3", None, "/dev/cu.x", "py", root=root,
                              erase=True)
        assert read[:3] == ["py", "-m", "esp_coredump"]
        assert read[read.index("--off") + 1] == str(0xC10000)
        assert read[-1].endswith("app.elf")
        # The raw dump never reaches the disk and memory is never printed.
        for flag in ("--save-core", "-s", "--print-mem", "-m", "--core", "-c"):
            assert flag not in read[3:], flag
        # Erase exactly the coredump partition, nothing else.
        assert erase[:3] == ["py", "-m", "esptool"]
        assert erase[-3:] == ["erase-region", "0xc10000", "0x10000"]
        assert "erase-flash" not in erase and "write-flash" not in erase
        assert cd.plan("cores3", None, "/dev/cu.x", "py", root=root) == [read]


def test_plan_refusals() -> None:
    with tempfile.TemporaryDirectory(prefix="os-coredump-") as name:
        root = Path(name)
        layout(root, target="waveshare/logger")
        expect_error(lambda: cd.plan("cores3", None, "/dev/x", "py", root=root),
                     "build the selected firmware first")
        expect_error(lambda: cd.plan("cores3", None, "", "py", root=root),
                     "checked serial port")
        expect_error(lambda: cd.plan("cores3", "logger", "/dev/x", "py", root=root),
                     "invalid board variant cores3/logger")
        expect_error(lambda: cd.plan("m5", None, "/dev/x", "py", root=root),
                     "unknown board 'm5'")
    with tempfile.TemporaryDirectory(prefix="os-coredump-") as name:
        root = Path(name)
        layout(root, board="waveshare", variant="diagnostic")
        steps = cd.plan("waveshare", "diagnostic", "/dev/x", "py", root=root,
                        erase=True)
        assert steps[1][-2:] == ["0xc10000", "0x10000"]
        # The footer slug names the same board as the alias (#141).
        assert cd.plan("waveshare-esp32-s3-sim7670g", "diagnostic", "/dev/x", "py",
                       root=root, erase=True) == steps
        # No variant means the telemetry app, which has no build here.
        expect_error(lambda: cd.plan("waveshare", None, "/dev/x", "py", root=root),
                     "build the selected firmware first")


def table_error(table: str, text: str) -> None:
    with tempfile.TemporaryDirectory(prefix="os-coredump-") as name:
        path = Path(name) / "partitions.csv"
        path.write_text(table)
        expect_error(lambda: cd.coredump_partition(path), text)


def test_partition_rules() -> None:
    table_error(TABLE.replace("coredump,   data, coredump,0xc10000, 0x10000,\n", ""),
                "found 0")
    table_error(TABLE + "coredump2, data, coredump, 0xd00000, 0x10000,\n", "found 2")
    table_error(TABLE.replace("data, coredump,0xc10000", "data, nvs,0xc10000"),
                "wrong type")
    table_error(TABLE.replace("0xc10000, 0x10000", "0xc10800, 0x10000"),
                "sector aligned")
    table_error(TABLE.replace("0xc10000, 0x10000", "0xc10000, 0x800"),
                "sector aligned")
    table_error(TABLE.replace("0xc10000, 0x10000", ", 0x10000"), "explicit offset")
    with tempfile.TemporaryDirectory(prefix="os-coredump-") as name:
        path = Path(name) / "partitions.csv"
        path.write_text(TABLE.replace("0xc10000, 0x10000", "0xc10000, 64K"))
        assert cd.coredump_partition(path) == (0xC10000, 0x10000)


def test_run_order() -> None:
    steps = [["read"], ["erase"]]
    calls: list[str] = []

    def runner(status: dict[str, int]):
        def call(step):
            calls.append(step[0])
            return status.get(step[0], 0)
        return call

    assert cd.run(steps, runner({})) == 0 and calls == ["read", "erase"]
    calls.clear()
    # A failed read keeps the dump so it can be read again.
    assert cd.run(steps, runner({"read": 1})) == 1 and calls == ["read"]
    calls.clear()
    assert cd.run(steps, runner({"erase": 2})) == 2 and calls == ["read", "erase"]
    calls.clear()
    assert cd.run([["read"]], runner({})) == 0 and calls == ["read"]


def test_private_tmp() -> None:
    """esp_coredump leaves its temp file behind on a failed decode."""
    for status in (0, 1):
        seen: list[str] = []

        def runner(step, env):
            folder = env["TMPDIR"]
            assert env["TMP"] == folder and env["TEMP"] == folder
            assert stat.S_IMODE(os.stat(folder).st_mode) == 0o700
            assert env["KEEP"] == "1"
            Path(folder, "tmpdump").write_bytes(b"raw stacks")
            seen.append(folder)
            return status

        assert cd.run_private([["read"], ["erase"]], runner,
                              {"KEEP": "1", "TMPDIR": "/tmp"}) == status
        assert seen and all(not os.path.exists(folder) for folder in seen), seen
        assert len(seen) == (2 if status == 0 else 1)

    seen.clear()

    def boom(step, env):
        seen.append(env["TMPDIR"])
        raise OSError("gdb died")

    try:
        cd.run_private([["read"]], boom, {})
    except OSError:
        pass
    else:
        raise AssertionError("expected OSError")
    assert seen and not os.path.exists(seen[0])


def main() -> int:
    for test in (test_plan, test_plan_refusals, test_partition_rules, test_run_order,
                 test_private_tmp):
        test()
    print("coredump-read tests passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
