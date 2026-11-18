"""Check the SD space governor states, hysteresis, backoff and no-delete rule."""
from __future__ import annotations

import argparse
from pathlib import Path
import re
import subprocess
import tempfile
from host_cxx import CXX_STD


def function_body(text: str, signature: str) -> str:
    start = text.index(signature)
    open_brace = text.index("{", start)
    depth = 0
    for index in range(open_brace, len(text)):
        depth += {"{": 1, "}": -1}.get(text[index], 0)
        if depth == 0:
            return text[open_brace:index + 1]
    raise AssertionError(f"unbalanced body for {signature}")


def presence_wiring_errors(logger: str, boards: dict[str, str]) -> list[str]:
    """Problems in the card presence check wiring (#165)."""
    errors = []
    try:
        body = " ".join(function_body(logger, "void service_presence(bool storage_ready)").split())
    except (ValueError, AssertionError):
        return ["service_presence is missing"]
    for needle in ("if (!hooks.storage_present) return;",
                   "storage::presence_due(previous, storage_ready, now, presence_checked)",
                   "BusLock lock; present = hooks.storage_present(hooks.context);",
                   "governor.write_failed(now); publish_storage(previous);"):
        if needle not in body:
            errors.append(f"service_presence lacks {needle}")
    try:
        loop = " ".join(function_body(logger, "void storage_worker(void *)").split())
    except (ValueError, AssertionError):
        return errors + ["storage_worker is missing"]
    if "service_storage(count, storage_ready); service_presence(storage_ready);" not in loop:
        errors.append("the worker loop does not call service_presence after service_storage")
    for name, text in boards.items():
        if ".storage_present = [](void *)" not in text:
            errors.append(f"{name} does not wire Hooks::storage_present")
    return errors


def check_presence_wiring(root: Path) -> None:
    logger = (root / "firmware/common/logger/src/os_logger.cpp").read_text()
    boards = {name: (root / path).read_text() for name, path in (
        ("cores3", "firmware/esp-idf-cores3/bringup/telemetry_logger.cpp"),
        ("waveshare", "firmware/esp-idf-waveshare-sim7670g/logger/main.cpp"),
        ("crowpanel", "firmware/esp-idf-crowpanel-epaper/logger/main.cpp"))}
    errors = presence_wiring_errors(logger, boards)
    assert not errors, errors
    # Each mutant must be caught, so the check above cannot pass vacuously.
    mutants = {
        "no bus lock": (logger.replace("BusLock lock;\n    present = hooks.storage_present",
                                       "present = hooks.storage_present", 1), boards),
        "no write failure": (logger.replace("  governor.write_failed(now);\n  publish_storage(previous);\n}\n\n// Explicit",
                                            "  publish_storage(previous);\n}\n\n// Explicit", 1), boards),
        "not called": (logger.replace("    service_presence(storage_ready);\n", "", 1), boards),
        "board unwired": (logger, {**boards, "crowpanel": boards["crowpanel"].replace(".storage_present", ".unused", 1)}),
    }
    for name, (text, wired) in mutants.items():
        assert presence_wiring_errors(text, wired), f"presence wiring mutant survived: {name}"
    helper = (root / "firmware/common/src/fat_volume.h").read_text()
    assert "return card && sdmmc_get_status(card) == ESP_OK;" in helper, "card_answers must send CMD13 only"


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sanitize", action="store_true")
    args = parser.parse_args()
    root = Path(__file__).resolve().parent.parent
    common = root / "firmware/common"
    header = common / "logger/src/storage_governor.h"
    # The governor only reports state. It must never reach a file API that
    # removes, renames or truncates owner data.
    forbidden = re.compile(r"\b(remove|unlink|rename|rmdir|truncate|ftruncate|f_unlink|f_rename|fopen)\s*\(")
    match = forbidden.search(header.read_text())
    assert match is None, f"storage governor calls {match.group(0)}"
    check_presence_wiring(root)
    with tempfile.TemporaryDirectory(prefix="aq-storage-governor-") as directory:
        executable = Path(directory) / "storage-governor-test"
        command = ["clang++", CXX_STD, "-Wall", "-Wextra", "-Werror",
                   "-I", str(common / "logger/src"),
                   str(common / "logger/tests/test_storage_governor.cpp"), "-o", str(executable)]
        if args.sanitize:
            command += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
        subprocess.run(command, check=True)
        subprocess.run([str(executable)], check=True)
    print("SD governor thresholds, hysteresis, backoff, recovery and the card presence check passed with no delete path, 4 wiring mutations fail")


if __name__ == "__main__":
    main()
