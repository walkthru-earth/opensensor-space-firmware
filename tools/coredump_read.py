#!/usr/bin/env python3
"""Read the core dump over USB serial and print it, erase only on request (#40).

The firmware saves a crash to the `coredump` flash partition
(CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH). A raw dump is a copy of task stacks and
can hold the Wi-Fi PSK, the LAN bearer token or the device PIN. So this tool
never keeps the raw dump on the host. It runs `esp_coredump info_corefile`
with TMPDIR set to a private 0700 folder, because esp_coredump writes the
flash read to a temporary file and leaves it behind when decoding fails. That
folder is removed on every path. The tool prints the crashed task, its
registers and the backtraces of all tasks, without `--print-mem`. The
partition is left alone unless `--erase` is given. Then it erases that one
partition with `esptool erase-region`, only after a good print, and nothing
else. There is no LAN or BLE core dump endpoint on purpose.

Run it through the pixi task, which loads the pinned ESP-IDF Python and GDB

    pixi run coredump-read <board> <checked-port> [variant]

The board is any name in a board.json (#141), such as cores3 or
m5stack-cores3. No variant means the board's telemetry app.

Build the same image first (`pixi run cores3-build` or `pixi run
waveshare-build <variant>`), since the dump is decoded with that ELF.
`--dry-run` prints the commands and opens no port. `--erase` clears the
partition after a good print, once you are done with the crash.
"""

from __future__ import annotations

import argparse
import csv
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
from typing import Callable, Sequence

import boards

ROOT = Path(__file__).resolve().parents[1]
SECTOR = 0x1000


class PlanError(ValueError):
    pass


def number(text: str) -> int:
    text = text.strip().lower()
    scale = 1
    if text.endswith("k"):
        scale, text = 1024, text[:-1]
    elif text.endswith("m"):
        scale, text = 1024 * 1024, text[:-1]
    return int(text, 0) * scale


def coredump_partition(partitions: Path) -> tuple[int, int]:
    """Offset and size of the only data/coredump row of a partitions.csv."""
    found = []
    with partitions.open(newline="") as stream:
        for row in csv.reader(stream):
            cells = [cell.strip() for cell in row]
            if not cells or not cells[0] or cells[0].startswith("#"):
                continue
            if len(cells) < 5:
                raise PlanError(f"short partition row {row}")
            if cells[2] == "coredump" or cells[0] == "coredump":
                if cells[1] != "data" or cells[2] != "coredump":
                    raise PlanError(f"coredump row has the wrong type {row}")
                if not cells[3] or not cells[4]:
                    raise PlanError("coredump row needs an explicit offset and size")
                found.append((number(cells[3]), number(cells[4])))
    if len(found) != 1:
        raise PlanError(f"expected one coredump partition, found {len(found)}")
    offset, size = found[0]
    if offset % SECTOR or size % SECTOR or size <= 0 or offset < 0x10000:
        raise PlanError("coredump partition is not sector aligned")
    return offset, size


def plan(board: str, variant: str | None, port: str, python: str,
         root: Path = ROOT, erase: bool = False) -> list[list[str]]:
    try:
        found, variant = boards.resolve(board, variant, root)
    except boards.BoardError as error:
        raise PlanError(str(error)) from error
    if not port:
        raise PlanError("checked serial port required")
    trial = root / found.project
    build = trial / "build" / variant
    target = build / "os-build-target.txt"
    # The build scripts write <alias>/<variant>, whichever board name was given.
    if not target.is_file() or target.read_text().strip() != f"{found.alias}/{variant}":
        raise PlanError("build the selected firmware first")
    description = json.loads((build / "project_description.json").read_text())
    elf = build / description["app_elf"]
    if not elf.is_file():
        raise PlanError(f"missing {elf}")
    offset, size = coredump_partition(trial / variant / "partitions.csv")
    common = ["--chip", "esp32s3", "--port", port]
    steps = [[python, "-m", "esp_coredump", *common, "info_corefile",
              "--off", str(offset), str(elf)]]
    if erase:
        steps.append([python, "-m", "esptool", *common, "--before",
                      "default-reset", "--after", "hard-reset", "erase-region",
                      hex(offset), hex(size)])
    return steps


Runner = Callable[[Sequence[str]], int]


def run(steps: list[list[str]], runner: Runner) -> int:
    """Erase only after the dump printed, so a failed read can be retried."""
    status = runner(steps[0])
    if status != 0:
        print("COREDUMP read failed, partition kept, nothing erased",
              file=sys.stderr)
        return status
    if len(steps) == 1:
        print("COREDUMP kept on the board, rerun with --erase to clear it",
              file=sys.stderr)
        return 0
    status = runner(steps[1])
    print("COREDUMP erased" if status == 0 else "COREDUMP erase failed",
          file=sys.stderr)
    return status


EnvRunner = Callable[[Sequence[str], dict], int]


def run_private(steps: list[list[str]], runner: EnvRunner,
                environ: dict | None = None) -> int:
    """run() with every temporary file in a private folder removed after."""
    private = tempfile.mkdtemp(prefix="os-coredump-")
    try:
        env = {**(os.environ if environ is None else environ),
               "TMPDIR": private, "TMP": private, "TEMP": private}
        return run(steps, lambda step: runner(step, env))
    finally:
        shutil.rmtree(private, ignore_errors=True)


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("board", help="any board name in a board.json, such as cores3 or m5stack-cores3")
    parser.add_argument("port")
    parser.add_argument("variant", nargs="?")
    parser.add_argument("--dry-run", action="store_true")
    parser.add_argument("--erase", action="store_true",
                        help="erase the coredump partition after a good print")
    args = parser.parse_args(argv)
    try:
        steps = plan(args.board, args.variant, args.port, sys.executable,
                     erase=args.erase)
    except (PlanError, OSError, KeyError, ValueError) as error:
        print(f"COREDUMP ERROR {error}", file=sys.stderr)
        return 2
    if args.dry_run:
        for step in steps:
            print(" ".join(step))
        return 0
    if not os.path.exists(args.port):
        print("COREDUMP ERROR checked serial port required", file=sys.stderr)
        return 2
    return run_private(steps, lambda step, env: subprocess.run(
        list(step), env=env, check=False).returncode)


if __name__ == "__main__":
    sys.exit(main())
