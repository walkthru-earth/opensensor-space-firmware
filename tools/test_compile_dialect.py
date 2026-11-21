"""Host tests for the fixture dialect check, `check_idf_target.py --compile-dialect`.

The SDK fixtures (logger compile test, connectivity and PSRAM probe) have no
board target, so they run only the per-file -std and source path map check on
their compiler database. This test feeds that check crafted databases and makes sure every
fixture entry point calls it. It needs no SDK.
"""

from __future__ import annotations

import json
from pathlib import Path
import re
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SCRIPT = ROOT / "tools/check_idf_target.py"
PINNED = re.search(r"-std=gnu\+\+\d+", (ROOT / "firmware/cmake/os_cxx_standard.cmake").read_text())[0]
# The ESP-IDF 6.1.1 default. Spelled in two parts because check_cxx_dialect_sources
# rejects a literal C++ -std in a tools/test_*.py.
SDK_DEFAULT = "-std=gnu++" + "26"
REPO_CPP = ROOT / "tools/fixtures/connectivity_compile/main.cpp"
REPO_C = ROOT / "firmware/common/vendor/lz4/lz4.c"
# Every SDK fixture build must run the check on its own build directory.
FIXTURE_ENTRY_POINTS = (
    ("tools/test_logger_build.sh", 'fixture="${repo_root}/firmware/common/logger/tests/compile"',
     '--compile-dialect "${fixture}/build"'),
    ("tools/build_connectivity_fixture.sh", 'fixture="${repo_root}/tools/fixtures/connectivity_compile"',
     '--compile-dialect "${fixture}/build"'),
    ("pixi.toml", "waveshare-psram-build = ",
     "--compile-dialect tools/fixtures/waveshare_psram_probe/build"),
)


# A lone `&` backgrounds the call. `&&` is fine, it only runs on success, and
# so are redirections such as `2>&1`, `>&2` and `&>log`.
BACKGROUND = re.compile(r"(?<![&<>])&(?![&>])")
# `!` before the call inverts its exit status.
INVERTED = re.compile(r"(?:^|[\s;&|(])!\s")


def calls_check(text: str, call: str) -> bool:
    """True when a live line runs the check with this call and keeps its exit status.

    A commented out call, or one followed by `||`, a pipe, `;` or a lone `&`,
    would let a failing check pass, so it does not count. Pixi tasks run
    without `set -e`, so `call; true` exits 0. A `set +e` on the line or a `!`
    before the call is rejected for the same reason."""
    for line in text.splitlines():
        code = line.strip()
        if code.startswith("#") or "tools/check_idf_target.py" not in code or call not in code:
            continue
        before, rest = code.split("tools/check_idf_target.py", 1)[0], code.split(call, 1)[1]
        if "||" in code or "|" in rest or ";" in rest or BACKGROUND.search(rest) or "#" in before:
            continue
        if "set +e" in code or INVERTED.search(before):
            continue
        return True
    return False


# What os_map_source_paths() adds to every repo-owned component.
SOURCE_MAP = f"-ffile-prefix-map={ROOT}=."


def entry(source: Path, *flags: str, mapped: bool = True) -> dict[str, str]:
    flags = (SOURCE_MAP, *flags) if mapped else flags
    return {"directory": "/tmp", "file": str(source),
            "command": " ".join(["xtensa-esp32s3-elf-g++", *flags, "-c", str(source)])}


def run(build: Path) -> subprocess.CompletedProcess[str]:
    return subprocess.run([sys.executable, str(SCRIPT), "--compile-dialect", str(build)],
                          capture_output=True, text=True, timeout=60, check=False)


def main() -> None:
    for relative, anchor, call in FIXTURE_ENTRY_POINTS:
        text = (ROOT / relative).read_text()
        assert anchor in text, f"{relative} no longer has {anchor}"
        # For pixi.toml only the PSRAM task line counts.
        lines = [line for line in text.splitlines() if anchor in line or relative != "pixi.toml"]
        assert calls_check("\n".join(lines), call), f"{relative} must run check_idf_target.py {call}"

    good = 'python "${repo_root}/tools/check_idf_target.py" --compile-dialect "${fixture}/build"'
    call = '--compile-dialect "${fixture}/build"'
    assert calls_check(good, call)
    assert calls_check('x = "tools/idf.sh build && python tools/check_idf_target.py ' + call + '"', call)
    assert not calls_check("# " + good, call), "a commented out call must not count"
    assert not calls_check(good + " || true", call), "a call that swallows its exit must not count"
    assert not calls_check(good + " || :", call)
    assert not calls_check(good + " | tee log", call)
    assert not calls_check("true # " + good, call)
    assert not calls_check(good + "; true", call), "pixi runs tasks without set -e, so ; true hides a failure"
    assert not calls_check(good + " ; exit 0", call)
    assert not calls_check(good + " &", call), "a backgrounded call must not count"
    assert not calls_check(good + " & wait", call)
    assert not calls_check(good + " 2>&1 &", call), "a redirection must not hide a later lone &"
    assert calls_check(good + " 2>&1", call), "a redirection keeps the exit status"
    assert calls_check(good + " >&2", call)
    assert calls_check(good + " &>check.log", call)
    assert calls_check(good + " 2>&1 && echo done", call)
    assert not calls_check("set +e; " + good, call)
    assert not calls_check("! " + good, call), "an inverted call must not count"
    assert not calls_check("build && ! " + good, call)
    assert calls_check(good + " && echo done", call), "&& keeps the exit status"
    assert calls_check("tools/idf.sh build && " + good, call)

    cases = {
        "pinned": ([entry(REPO_CPP, SDK_DEFAULT, PINNED)], True),
        "pinned with a C file": ([entry(REPO_CPP, PINNED), entry(REPO_C, "-std=gnu17")], True),
        "SDK default wins": ([entry(REPO_CPP, PINNED, SDK_DEFAULT)], False),
        "no std": ([entry(REPO_CPP, "-Og")], False),
        "C file with a C++ std": ([entry(REPO_CPP, PINNED), entry(REPO_C, PINNED)], False),
        "only third party": ([entry(Path("/opt/esp/idf/components/log/log.cpp"), SDK_DEFAULT)], False),
        "third party without a map": ([entry(REPO_CPP, PINNED),
                                       entry(Path("/opt/esp/idf/components/log/log.cpp"), mapped=False)], True),
        "C++ file without a map": ([entry(REPO_CPP, PINNED, mapped=False)], False),
        "C file without a map": ([entry(REPO_CPP, PINNED), entry(REPO_C, "-std=gnu17", mapped=False)], False),
        "map of a subdirectory": ([entry(REPO_CPP, PINNED, f"-ffile-prefix-map={ROOT / 'firmware'}=.",
                                         mapped=False)], False),
        "map to an absolute name": ([entry(REPO_CPP, PINNED, f"-ffile-prefix-map={ROOT}=/src",
                                           mapped=False)], False),
        "debug only map": ([entry(REPO_CPP, PINNED, f"-fdebug-prefix-map={ROOT}=.", mapped=False)], False),
        "empty database": ([], False),
    }
    with tempfile.TemporaryDirectory() as raw:
        for name, (commands, ok) in cases.items():
            build = Path(raw) / name.replace(" ", "-")
            build.mkdir()
            (build / "compile_commands.json").write_text(json.dumps(commands))
            result = run(build)
            if ok:
                assert result.returncode == 0, f"{name}: {result.stderr}"
                assert PINNED in result.stdout, name
            else:
                assert result.returncode != 0, f"{name}: {result.stdout}"
                assert "native target rejected" in result.stderr, f"{name}: {result.stderr}"
        missing = Path(raw) / "missing"
        missing.mkdir()
        result = run(missing)
        assert result.returncode != 0 and "compile_commands.json" in result.stderr, result.stderr

    print("check_idf_target.py --compile-dialect host tests passed")


if __name__ == "__main__":
    main()
