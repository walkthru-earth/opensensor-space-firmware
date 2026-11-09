"""Fail on a retired AQ wire, NVS, Kconfig, serial or build name anywhere in the repo (#93, #111).

The 2026-10-26 rename moved every name the phone, the host tools or a stored
blob depends on to its OpenSensor form, with no backward compatibility. This
check keeps the old names from coming back in code, tools, docs or CI.

Matched names (old form). The failure names the doc that lists the new one,
docs/shared/mobile-migration.md for names the phone or a stored blob sees and
docs/shared/development.md for build and environment names.
  mDNS service           the old _xxsync service type
  LAN handshake          the old four byte magic
  GATT base UUID         the old base prefix
  GATT service name      the old service display name
  NVS namespaces         the old config and coredump namespaces
  Kconfig symbols        the old logger and runtime symbols, with or without CONFIG_
  serial tags            a line tag at a line start, after a quote or backtick,
                         or anywhere inside a string literal
  CMake helpers (#111)   the old firmware/cmake file names, the functions they
                         define and the variables they set, now os_ and OS_
  build variables        the old build, script, signing and toolchain variables
  build manifests        the old build hash and target file names
  toolchain cache        the old SDK cache folder under ~/.cache
  pixi project name      the old workspace name in pixi.toml
  board component (#111) the old CoreS3 and Waveshare board component names
  source file (#111)     the old logger source file names and fixture projects
  common source (#111)   the old location, power, console and sensor file names

History is exempt. The dated bench records, the migration log and the #93
audit name the old forms on purpose. There is no sdkconfig.rename shim, so an
old Kconfig name is retired everywhere else.

Exit 0 when clean, 1 with one path:line per finding, 2 when the repo cannot be
listed. tools/lint.sh fails on anything but 0.

Usage:
  python tools/check_retired_names.py [PATH ...]   # default every tracked file
"""

from __future__ import annotations

from pathlib import Path
import re
import subprocess
import sys

import boards

ROOT = Path(__file__).resolve().parents[1]
# Each old name is split so this file does not match its own patterns.
OLD = "A" + "Q"
RETIRED = (
    ("mDNS service", re.compile("_" + "aq" + "sync")),
    ("LAN handshake", re.compile(OLD + "S1")),
    ("GATT base UUID", re.compile("c0a5" + "e9f0", re.I)),
    ("GATT service name", re.compile(OLD + " Sync")),
    ("NVS namespace", re.compile(r"\b" + "aq" + r"(cfg|dump)\b")),
    ("Kconfig symbol", re.compile(r"\b(CONFIG_)?" + OLD + r"_(LOGGER_WORKER_WDT|LOGGER_BENCH_STALL|LOG_PERSIST_|PM_DFS_)")),
    # A tag at a line start or right after a quote, or anywhere inside a
    # string literal. Inside a literal the word needs 3 letters (or is TF), so
    # the ISO 3166 list with Antarctica ("AO AQ AR") stays quiet.
    (
        "serial tag",
        re.compile("(^|[\"'`])" + OLD + r" [A-Z][A-Z_]+|[\"'`][^\"'`]*\b" + OLD + r" ([A-Z][A-Z_]{2,}|TF)\b"),
    ),
    # CMake helpers renamed in #111 Phase 2a. The files, the functions and the
    # variables they set moved from the aq_ and AQ_ prefix to os_ and OS_.
    (
        "CMake helper",
        re.compile(
            r"\b" + "aq" + r"_(pin_cxx_standard|map_source_paths|map_debug_paths|patch_nimble|enable_stack_usage)\b"
            r"|\b" + "aq" + r"_(cxx_standard|source_paths|nimble_patches|stack_usage)\.cmake\b"
            r"|\b" + "aq" + r"_nimble_patch\b"
        ),
    ),
    ("CMake variable", re.compile(r"\b" + OLD + r"_(SOURCE_ROOT|SOURCE_ROOT_REAL|NIMBLE_PATCH_DIR|PATCH_PROGRAM)\b")),
    # Build, script and toolchain names moved in #111 Phases 2b and 2c, with
    # no alias. The pattern also matches after -D. The Arduino era cache path
    # and the `$M5_TOOLCHAIN_ROOT` prose in dated CoreS3 notes stay, so only
    # the SDK toolchain path is retired, and M5_TOOLCHAIN_ROOT only where code
    # reads or sets it, quotes it as a name, or builds a path from it.
    (
        "build variable",
        re.compile(
            r"(?<![A-Za-z0-9])" + OLD + r"_(REPO|BENCH|VARIANT|SDKCONFIG_OVERLAY|TOOLCHAIN_ROOT|IDF_TOOLS_PATH|IDF_PATH"
            r"|APP_SIGNING_KEY|APP_SIGNING_SCHEME|GATE_TASK_TIMEOUT|FIELDS_INC|SYNC_ROM_CRC|REPRO_KEEP"
            r"|FAKE_SOURCE|FAKE_ESPTOOL_MODE)\b|-D" + OLD + r"_(FIELDS_INC|SYNC_ROM_CRC)\b"
            r"|\bM5_TOOLCHAIN_ROOT(:-|=|\})|[\"']M5_TOOLCHAIN_ROOT[\"']"
            r"|\$\{?M5_TOOLCHAIN_ROOT\}?/(?!toolchains/arduino)"
        ),
    ),
    ("build manifest", re.compile(r"\b" + "aq" + r"-build-(sha256\.json|target\.txt)")),
    (
        "toolchain cache",
        re.compile(r"(\.cache|XDG_CACHE_HOME[^/]*)/m5stack-" + "aq" + r"-parquet(?!/toolchains/arduino)"),
    ),
    ("pixi project name", re.compile(r"^name = \"" + "aq" + r"-parquet\"")),
    # Board components renamed in #111 Phase 2d, plus the Waveshare form that
    # builds the name from OS_VARIANT.
    (
        "board component",
        re.compile(r"\b" + "aq" + r"_(cores3|waveshare_logger|waveshare_diagnostic)\b|\b" + "aq" + r"_waveshare_\$\{"),
    ),
    # Logger sources renamed in #111 Phase 2e, the files, their includes and
    # the compile fixture project, plus the two host fixture projects #122
    # renamed.
    (
        "source file",
        re.compile(r"\b" + "aq" + r"_logger\w*|\b" + "aq" + r"_(connectivity_fixture|psram_probe)\b"),
    ),
    # Common sources renamed in #111 Phase 2e, the files and their includes.
    ("common source", re.compile(r"\b" + "aq" + r"_(location|battery_health|console|ltr553|max17048)\b")),
)
# Build and environment names. Their new form is in the development guide,
# not the mobile migration log, which tracks what the phone can observe.
BUILD_KINDS = frozenset(
    {
        "CMake helper",
        "CMake variable",
        "build variable",
        "build manifest",
        "toolchain cache",
        "pixi project name",
        "board component",
        "source file",
        "common source",
    }
)
# History that names the old forms on purpose.
EXEMPT = {
    "docs/shared/mobile-migration.md",
    "docs/shared/stac-contract/firmware-audit.md",
    "docs/shared/stac-contract/firmware-audit-review.md",
}
SKIP_SUFFIXES = {".lock", ".png", ".jpg", ".jpeg", ".gif", ".bin", ".elf", ".parquet", ".pdf", ".zip"}


def exempt() -> set[str]:
    """EXEMPT plus the dated bench record docs/boards/<slug>/bench-verified.md of each board.json board (#141)."""
    return EXEMPT | {f"docs/boards/{board.slug}/bench-verified.md" for board in boards.boards(ROOT)}


def tracked(root: Path) -> list[str] | None:
    """Every tracked path, relative to root, or None when git cannot list them."""
    try:
        result = subprocess.run(["git", "-C", str(root), "ls-files", "-z"], capture_output=True, check=True)
    except (OSError, subprocess.CalledProcessError):
        return None
    return [name for name in result.stdout.decode().split("\0") if name]


def scan_text(text: str) -> list[tuple[int, str]]:
    """Line number and kind of every retired name in one file's text."""
    findings = []
    for number, line in enumerate(text.splitlines(), 1):
        for kind, pattern in RETIRED:
            if pattern.search(line):
                findings.append((number, kind))
    return findings


def findings(root: Path, names: list[str]) -> list[str]:
    out = []
    skip = exempt()
    for name in sorted(names):
        if name in skip or Path(name).suffix in SKIP_SUFFIXES:
            continue
        path = root / name
        if not path.is_file():
            continue
        text = path.read_text(encoding="utf-8", errors="replace")
        out += [f"{name}:{number}: retired {kind}" for number, kind in scan_text(text)]
    return out


def doc_for(finding: str) -> str:
    """The doc that lists the new name for one finding's kind."""
    kind = finding.rsplit(": retired ", 1)[-1]
    return "docs/shared/development.md" if kind in BUILD_KINDS else "docs/shared/mobile-migration.md"


def main(argv: list[str]) -> int:
    names = argv or tracked(ROOT)
    if names is None:
        print("!!! retired name check cannot list tracked files", file=sys.stderr)
        return 2
    try:
        found = findings(ROOT, names)
    except boards.BoardError as error:
        print(f"!!! retired name check cannot read the board registry, {error}", file=sys.stderr)
        return 2
    for finding in found:
        print(f"!!! {finding}, use the OpenSensor name ({doc_for(finding)})")
    return 1 if found else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
