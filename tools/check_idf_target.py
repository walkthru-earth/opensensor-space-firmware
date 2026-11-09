#!/usr/bin/env python3
"""Validate pinned native SDK, board target, legacy partitions and flash artifacts."""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import re
import shlex
import struct
import subprocess

import boards

ROOT = Path(__file__).resolve().parents[1]
FLASH_BYTES = 16 * 1024 * 1024
PARTITIONS = [
    ("nvs", 1, 2, 0x9000, 0x5000, 0),
    ("otadata", 1, 0, 0xE000, 0x2000, 0),
    ("app0", 0, 0x10, 0x10000, 0x600000, 0),
    ("app1", 0, 0x11, 0x610000, 0x600000, 0),
    ("coredump", 1, 3, 0xC10000, 0x10000, 0),
    ("spiffs", 1, 0x82, 0xC20000, 0x3E0000, 0),
]


def require(condition: bool, message: str) -> None:
    if not condition:
        raise ValueError(message)


def sdk_lock() -> dict[str, str]:
    return dict(re.findall(r"^(ESP_IDF_[A-Z]+)=(\S+)$",
                          (ROOT / "tools/idf-dependencies.lock").read_text(), re.M))


def git(sdk: Path, *arguments: str) -> str:
    return subprocess.check_output(["git", "-C", str(sdk), *arguments], text=True).rstrip()


def check_sdk(sdk: Path, allow_missing: bool = False) -> str:
    expected = sdk_lock()["ESP_IDF_COMMIT"]
    require((sdk / "tools/idf.py").is_file(), "ESP-IDF checkout is incomplete")
    require(git(sdk, "rev-parse", "HEAD") == expected, "ESP-IDF commit differs from lock")
    # Ignore untracked build/download files, but never consume edited SDK code.
    require(not git(sdk, "status", "--porcelain", "--untracked-files=no",
                    "--ignore-submodules=none"), "ESP-IDF or a submodule has local changes")
    modules = git(sdk, "submodule", "status", "--recursive").splitlines()
    require(all(line.startswith(" ") or (allow_missing and line.startswith("-"))
                for line in modules), "ESP-IDF submodule missing or at a different commit")
    return expected


def partitions(data: bytes) -> list[tuple]:
    entries = []
    for offset in range(0, len(data), 32):
        entry = data[offset:offset + 32]
        if entry[:2] == b"\xeb\xeb":
            require(len(entry) == 32 and entry[16:] == hashlib.md5(data[:offset]).digest(),
                    "partition table MD5 mismatch")
            return entries
        if entry[:2] == b"\xff\xff":
            return entries
        require(len(entry) == 32 and entry[:2] == b"\xaa\x50", "invalid partition table")
        _, kind, subtype, start, size, label, flags = struct.unpack("<HBBII16sI", entry)
        entries.append((label.split(b"\0", 1)[0].decode(), kind, subtype, start, size, flags))
    raise ValueError("unterminated partition table")


# The C++ dialect is written in four places. They must name the same standard.
DIALECT_SOURCES = (
    ("firmware/cmake/os_cxx_standard.cmake", r"-std=gnu\+\+(\d+)"),
    ("tools/host_cxx.py", r'CXX_STD = "-std=c\+\+(\d+)"'),
    ("pixi.toml", r"clang\+\+ -std=c\+\+(\d+)"),
    ("tools/lint.sh", r"--std=c\+\+(\d+)"),
)


def check_cxx_dialect_sources() -> str:
    """Return the pinned firmware -std flag after checking every source agrees."""
    found = {}
    for relative, pattern in DIALECT_SOURCES:
        matches = set(re.findall(pattern, (ROOT / relative).read_text()))
        require(len(matches) == 1, f"{relative} must name exactly one C++ standard, found {sorted(matches)}")
        found[relative] = matches.pop()
    require(len(set(found.values())) == 1, f"C++ dialect sources disagree: {found}")
    # Host harnesses take the C++ standard from host_cxx.CXX_STD. C standards such as -std=c99 stay allowed.
    hardcoded = sorted(path.name for path in (ROOT / "tools").glob("test_*.py")
                       if re.search(r"-std=(?:c|gnu)\+\+\d+", path.read_text()))
    require(not hardcoded, f"import CXX_STD from host_cxx instead of a C++ -std in {hardcoded}")
    return "-std=gnu++" + next(iter(found.values()))


def expand_response_files(arguments: list[str], directory: Path) -> list[str]:
    """Inline @file arguments so a -std moved into a toolchain response file still counts."""
    expanded = []
    for argument in arguments:
        if argument.startswith("@"):
            path = Path(argument[1:])
            path = path if path.is_absolute() else directory / path
            expanded += expand_response_files(shlex.split(path.read_text()), directory) if path.is_file() else [argument]
        else:
            expanded.append(argument)
    return expanded


def maps_source_root(arguments: list[str]) -> bool:
    """True when a -ffile-prefix-map from os_map_source_paths() maps the repo root to "."."""
    for argument in arguments:
        old, separator, new = argument.removeprefix("-ffile-prefix-map=").rpartition("=")
        if argument.startswith("-ffile-prefix-map=") and separator and new == "." and \
                Path(old).resolve() == ROOT:
            return True
    return False


def check_compile_dialect(commands: list[dict], firmware_std: str) -> int:
    """Every repo-owned C++ file must end on the pinned -std, and repo C files get no C++ -std.

    Every repo-owned C and C++ file must also map the repo root, so the image
    holds no checkout path."""
    checked = 0
    for entry in commands:
        source = Path(entry["file"]).resolve()
        if not source.is_relative_to(ROOT):
            continue
        parts = source.relative_to(ROOT).parts
        if "managed_components" in parts or "build" in parts:
            continue
        directory = Path(entry.get("directory", "."))
        arguments = expand_response_files(entry.get("arguments") or shlex.split(entry["command"]), directory)
        standards = [argument for argument in arguments if argument.startswith("-std=")]
        relative = source.relative_to(ROOT)
        if source.suffix == ".cpp":
            require(bool(standards) and standards[-1] == firmware_std,
                    f"{relative} builds with {standards[-1] if standards else 'no -std'}, "
                    f"expected {firmware_std}. Call os_pin_cxx_standard() in its component")
            checked += 1
        elif source.suffix == ".c":
            require(not any("++" in standard for standard in standards), f"{relative} got a C++ -std")
        if source.suffix in {".c", ".cpp"}:
            require(maps_source_root(arguments),
                    f"{relative} does not map the repo root with -ffile-prefix-map. "
                    "Call os_map_source_paths() in its component")
    require(checked > 0, "no repo-owned C++ files in the compiler database")
    return checked


def check_fixture_dialect(build: Path) -> int:
    """Per-file dialect check for an SDK fixture build, which has no board target to verify."""
    commands = json.loads(build_file(build, "compile_commands.json").read_text())
    require(isinstance(commands, list) and bool(commands), "empty compiler database")
    return check_compile_dialect(commands, check_cxx_dialect_sources())


def build_file(build: Path, name: str) -> Path:
    path = (build / name).resolve()
    require(path.is_relative_to(build) and path.is_file(), "missing/outside-build artifact: " + name)
    return path


CONFIG_LINE = re.compile(r"^(?:(CONFIG_\w+)=.*|# (CONFIG_\w+) is not set)$")
COREDUMP_SETTINGS = ["CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH=y", "CONFIG_ESP_COREDUMP_ENABLE=y",
                     "CONFIG_ESP_COREDUMP_CHECK_BOOT=y", "CONFIG_ESP_COREDUMP_STACK_SIZE=2560",
                     "# CONFIG_ESP_COREDUMP_CAPTURE_DRAM is not set"]
# ESP-IDF 6.1 only writes ELF dumps with a SHA-256 checksum. Its Kconfig still
# selects ESP_COREDUMP_DATA_FORMAT_ELF and ESP_COREDUMP_CHECKSUM_SHA256, but
# neither symbol is defined or written to sdkconfig, so prove both by symbol.
# Only global symbols, which survive any optimization level. The checksum one
# links only with core_dump_sha.c, and esp_core_dump_image_check only with the
# flash backend (esp_core_dump_init alone has a weak fallback).
COREDUMP_SYMBOLS = {"esp_core_dump_init", "esp_core_dump_write", "esp_core_dump_write_elf",
                    "esp_core_dump_checksum_finish", "esp_core_dump_image_check"}


def kconfig_names(node: object, names: set[str]) -> set[str]:
    if isinstance(node, dict):
        if isinstance(node.get("name"), str):
            names.add("CONFIG_" + node["name"])
        for value in node.values():
            kconfig_names(value, names)
    elif isinstance(node, list):
        for value in node:
            kconfig_names(value, names)
    return names


def renamed_names(directories: list[str], known: set[str]) -> set[str]:
    """Deprecated names in the IDF root and built components' sdkconfig.rename files.

    kconfgen still maps these to their new symbol, so accept one only when its
    new name is a known symbol of this build."""
    names = set()
    for directory in directories:
        for rename in (Path(directory) / "sdkconfig.rename",
                       Path(directory) / "sdkconfig.rename.esp32s3"):
            if not rename.is_file():
                continue
            for line in rename.read_text().splitlines():
                fields = line.split()
                if len(fields) == 2 and fields[0].startswith("CONFIG_") and \
                        fields[1].lstrip("!") in known:
                    names.add(fields[0])
    return names


def check_defaults_known(build: Path, defaults: str, rename_paths: list[str]) -> None:
    """Fail where kconfgen only prints a NOTE, an unknown symbol in a defaults file.

    A symbol is unknown when no built component declares it, for example
    ESP_COREDUMP_* without the espcoredump component, and the setting is
    then silently dropped from the image."""
    known = kconfig_names(json.loads(build_file(build, "config/kconfig_menus.json").read_text()), set())
    known |= renamed_names(rename_paths, known)
    for name in filter(None, defaults.split(";")):
        for line in Path(name).read_text().splitlines():
            match = CONFIG_LINE.match(line.strip())
            symbol = match and (match[1] or match[2])
            require(not symbol or symbol in known,
                    f"unknown or renamed kconfig symbol {symbol} in {Path(name).name}, "
                    "check the name or add the component that declares it")


def elf_symbols(path: Path) -> set[str]:
    """Defined global symbols of an ELF32 little-endian image, enough to prove a component linked.

    Undefined references and weak or local definitions do not count."""
    data = path.read_bytes()
    require(data[:6] == b"\x7fELF\x01\x01", "application is not a 32-bit little-endian ELF")
    offset = struct.unpack_from("<I", data, 0x20)[0]
    size, count = struct.unpack_from("<HH", data, 0x2E)
    sections = [struct.unpack_from("<10I", data, offset + index * size) for index in range(count)]
    names = set()
    for section in sections:
        if section[1] != 2:  # SHT_SYMTAB
            continue
        strings = sections[section[6]][4]
        for entry in range(section[4], section[4] + section[5], 16):
            name, info, index = struct.unpack_from("<I8xBxH", data, entry)
            if info >> 4 != 1 or index == 0:  # not STB_GLOBAL, or SHN_UNDEF
                continue
            start = strings + name
            names.add(data[start:data.index(b"\0", start)].decode())
    return names


def check_target(build: Path, board: boards.Board, variant: str) -> tuple[str, dict[str, str]]:
    """Check one build of a registry board (#141).

    The telemetry app, the board.json directory, runs the logger with PSRAM
    and NimBLE. Every other variant is a bench diagnostic without them.
    """
    require(variant in board.variants, f"{board.alias} has no variant {variant}")
    telemetry = variant == board.telemetry_variant
    trial = ROOT / board.project
    lines = set(build_file(build, "sdkconfig").read_text().splitlines())
    required = ["CONFIG_IDF_TARGET_ESP32S3=y", "CONFIG_ESPTOOLPY_FLASHSIZE_16MB=y",
                "CONFIG_ESPTOOLPY_FLASHMODE_QIO=y", "CONFIG_PARTITION_TABLE_CUSTOM=y",
                "CONFIG_PARTITION_TABLE_OFFSET=0x8000", "CONFIG_FATFS_LFN_HEAP=y",
                "CONFIG_FATFS_MAX_LFN=255", "CONFIG_FREERTOS_HZ=1000",
                'CONFIG_ESPTOOLPY_FLASHFREQ="80m"',
                f'CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="{variant}/partitions.csv"']
    if not telemetry:
        required.append("# CONFIG_SPIRAM is not set")
        require("CONFIG_BT_ENABLED=y" not in lines and "CONFIG_BT_NIMBLE_ENABLED=y" not in lines,
                f"{variant} unexpectedly enables Bluetooth")
    else:
        required += ["CONFIG_SPIRAM=y", f"CONFIG_SPIRAM_MODE_{board.psram_mode}=y",
                     "CONFIG_SPIRAM_SPEED_80M=y", "CONFIG_SPIRAM_USE_MALLOC=y",
                     "CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP=y",
                     "CONFIG_ESP_WIFI_STATIC_TX_BUFFER=y",
                     "CONFIG_ESP_WIFI_STATIC_RX_BUFFER_NUM=16",
                     "CONFIG_ESP_WIFI_STATIC_TX_BUFFER_NUM=8",
                     "CONFIG_ESP_WIFI_DYNAMIC_RX_BUFFER_NUM=32",
                     "CONFIG_ESP_WIFI_CACHE_TX_BUFFER_NUM=32",
                     "CONFIG_LWIP_TCP_SND_BUF_DEFAULT=23040",
                     "CONFIG_LWIP_TCP_WND_DEFAULT=23040",
                     "CONFIG_LWIP_TCP_RECVMBOX_SIZE=18",
                     "CONFIG_LWIP_TCP_MAXRTX=6",
                     "CONFIG_BT_NIMBLE_MEM_ALLOC_MODE_EXTERNAL=y",
                     "CONFIG_PM_ENABLE=y",
                     "CONFIG_ESP_WIFI_RX_BA_WIN=16",
                     "CONFIG_SPIRAM_ALLOW_NOINIT_SEG_EXTERNAL_MEMORY=y",
                     "CONFIG_BT_ENABLED=y", "CONFIG_BT_NIMBLE_ENABLED=y",
                     "CONFIG_BT_NIMBLE_SECURITY_ENABLE=y", "CONFIG_BT_NIMBLE_SM_SC=y",
                     "CONFIG_BT_NIMBLE_SM_SC_ONLY=1", "CONFIG_BT_NIMBLE_NVS_PERSIST=y",
                     "CONFIG_ESP32S3_UNIVERSAL_MAC_ADDRESSES_TWO=y",
                     "CONFIG_ESP_MAC_UNIVERSAL_MAC_ADDRESSES=2",
                     "# CONFIG_BT_NIMBLE_SM_LEGACY is not set",
                     "# CONFIG_BT_NIMBLE_SM_SC_DEBUG_KEYS is not set"]
    # The console and any other per board line, from board.json tooling.sdkconfig.
    required += board.sdkconfig
    # Every image saves a panic core dump to the coredump partition below.
    required += COREDUMP_SETTINGS
    for setting in required:
        require(setting in lines, f"native target missing {setting}")
    require("CONFIG_ESP_CONSOLE_USB_CDC=y" not in lines, "unexpected USB CDC console")
    commands = json.loads(build_file(build, "compile_commands.json").read_text())
    require(bool(commands), "empty compiler database")
    require(any(Path(entry["file"]).resolve() == trial / variant / "main.cpp" for entry in commands),
            "compiler database does not include the selected board application")
    for entry in commands:
        arguments = entry.get("arguments") or shlex.split(entry["command"])
        command = " ".join(arguments)
        require(not re.search(r"(?:^|\s)-D\s*ARDUINO(?:\b|_)", command) and
                "/arduino-esp32/" not in command.lower(), "native compiler includes Arduino")
    check_compile_dialect(commands, check_cxx_dialect_sources())
    description = json.loads(build_file(build, "project_description.json").read_text())
    require(Path(description["project_path"]).resolve() == trial,
            "build belongs to a different board project")
    require(Path(description["build_dir"]).resolve() == build, "build directory provenance mismatch")
    require(Path(description["config_file"]).resolve() == build / "sdkconfig",
            "build configuration provenance mismatch")
    require(description["target"] == "esp32s3", "project target is not ESP32-S3")
    sdk_commit = check_sdk(Path(description["idf_path"]))
    require(description["git_revision"] == "v" + sdk_lock()["ESP_IDF_VERSION"],
            "build used a different SDK revision")
    check_defaults_known(build, description["config_defaults"],
                         [description["idf_path"], *description["build_component_paths"]])
    components = description["build_components"]
    require("espcoredump" in components, "core dump component is not built")
    missing = COREDUMP_SYMBOLS - elf_symbols(build_file(build, description["app_elf"]))
    require(not missing, "application ELF lacks core dump symbols: " + ", ".join(sorted(missing)))
    if not telemetry:
        require("connectivity" not in components and "logger" not in components,
                f"{variant} unexpectedly includes application/radio runtime")
    else:
        require({"logger", "connectivity", "runtime", "common"}.issubset(components),
                "logger runtime component missing")
    require(not any("arduino" in component.lower() for component in components),
            "native target depends on Arduino")
    # Per board components from board.json, such as M5 drivers on CoreS3 only.
    forbidden = sorted(name for name in components for part in board.components_forbidden
                       if part.lower() in name.lower())
    require(not forbidden, f"{board.alias} must not build {', '.join(forbidden)}")
    missing = sorted(set(board.components_required) - set(components))
    require(not missing, f"{board.alias} components missing, {', '.join(missing)}")
    require(partitions(build_file(build, "partition_table/partition-table.bin").read_bytes()) ==
            PARTITIONS, "partition table differs from the existing owner-data layout")
    plan = json.loads(build_file(build, "flasher_args.json").read_text())
    expected = {0: "bootloader/bootloader.bin", 0x8000: "partition_table/partition-table.bin",
                0xE000: "ota_data_initial.bin", 0x10000: description["app_bin"]}
    actual = {int(offset, 0): name for offset, name in plan["flash_files"].items()}
    require(actual == expected, "unexpected flash plan; only bootloader/table/OTA metadata/app0 allowed")
    settings = plan["flash_settings"]
    # IDF deliberately flashes a DIO-readable bootloader even for runtime QIO.
    require(settings["flash_size"] == "16MB" and settings["flash_mode"] in {"dio", "qio"} and settings["flash_freq"] == "80m",
            "unexpected flasher memory settings")
    require(plan["extra_esptool_args"]["chip"] == "esp32s3", "unexpected flasher chip")
    limits = {0: 0x8000, 0x8000: 0x1000, 0xE000: 0x2000, 0x10000: 0x600000}
    for offset, name in actual.items():
        size = build_file(build, name).stat().st_size
        require(0 < size <= limits[offset], "flash artifact exceeds its permitted region: " + name)
    expected_flags = {"--flash-mode": settings["flash_mode"],
                      "--flash-size": settings["flash_size"],
                      "--flash-freq": settings["flash_freq"]}
    json_args = plan["write_flash_args"]
    require(len(json_args) == 6 and dict(zip(json_args[::2], json_args[1::2])) == expected_flags,
            "unexpected flash erase/encryption/options")
    # idf.py flash consumes flash_args; check it agrees with the JSON plan.
    arguments = shlex.split(build_file(build, "flash_args").read_text())
    require(len(arguments) == 14, "unexpected arguments in the actual flash response file")
    flags, images = {}, {}
    for option, value in zip(arguments[::2], arguments[1::2]):
        if option.startswith("--"):
            flags[option] = value
        else:
            images[int(option, 0)] = value
    require(flags == expected_flags and images == expected,
            "flash response file disagrees with the checked flash plan")
    files = set(actual.values()) | {"sdkconfig", "compile_commands.json",
                                  "project_description.json", "flasher_args.json", "flash_args"}
    hashes = {name: hashlib.sha256(build_file(build, name).read_bytes()).hexdigest()
              for name in sorted(files)}
    return sdk_commit, hashes


def check_backup(backup: Path, board: boards.Board) -> None:
    require(backup.name.startswith(board.backup_prefix + "-flash-") and backup.suffix == ".bin",
            f"backup does not match the selected board, expected {board.backup_prefix}-flash-*.bin")
    require(backup.stat().st_size == board.flash_bytes == FLASH_BYTES, "backup must be exactly 16777216 bytes")
    with backup.open("rb") as source:
        source.seek(0x8000)
        previous = partitions(source.read(0xC00))
    # The backup may predate the app partition sizes; preserve the persistent
    # namespace region in every supported native firmware transition.
    require([entry for entry in previous if entry[1:3] == (1, 2)] == [PARTITIONS[0]],
            "backup NVS region differs; native migration needs explicit layout planning")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("build", type=Path, nargs="?")
    parser.add_argument("--board", help="any board name tools/boards.py resolves, from board.json (#141)")
    parser.add_argument("--variant", help="an app directory listed in the board's tooling.variants")
    parser.add_argument("--sdk-only", type=Path)
    parser.add_argument("--cxx-dialect", action="store_true",
                        help="only check that every C++ dialect source names the same standard")
    parser.add_argument("--compile-dialect", type=Path, metavar="BUILD",
                        help="only check the -std and the source path map of every repo C and C++ file "
                             "in a fixture build's compile_commands.json")
    parser.add_argument("--allow-missing-submodules", action="store_true")
    parser.add_argument("--backup", type=Path)
    manifests = parser.add_mutually_exclusive_group()
    manifests.add_argument("--write-manifest", action="store_true")
    manifests.add_argument("--verify-manifest", action="store_true")
    args = parser.parse_args()
    try:
        if args.cxx_dialect:
            print(f"C++ dialect sources agree on {check_cxx_dialect_sources()}")
            return
        if args.compile_dialect:
            checked = check_fixture_dialect(args.compile_dialect.resolve())
            print(f"{checked} repo C++ files in {args.compile_dialect} build with {check_cxx_dialect_sources()} "
                  "and every repo C and C++ file maps the repo root")
            return
        if args.sdk_only:
            check_sdk(args.sdk_only.resolve(), args.allow_missing_submodules)
            print("Verified pinned ESP-IDF SDK and submodule identity")
            return
        require(args.build is not None and args.board is not None and args.variant is not None,
                "build, --board and --variant are required")
        build = args.build.resolve()
        board, variant = boards.resolve(args.board, args.variant)
        commit, hashes = check_target(build, board, variant)
        # The manifest names the alias, the build target os-build-target.txt names.
        manifest = {"board": board.alias, "variant": variant, "sdk_commit": commit, "files": hashes}
        manifest_path = build / "os-build-sha256.json"
        if args.verify_manifest:
            require(json.loads(manifest_path.read_text()) == manifest,
                    "reviewed build artifacts changed; build and verify again")
        if args.write_manifest:
            temporary = manifest_path.with_suffix(".json.partial")
            temporary.write_text(json.dumps(manifest, indent=2) + "\n")
            temporary.replace(manifest_path)
        if args.backup:
            check_backup(args.backup, board)
        print(f"Verified native ESP32-S3 {board.alias}/{variant}: "
              "SDK, memory, console, components, C++ dialect, source path map, core dump, partitions and flash plan")
    except (ValueError, OSError, KeyError, subprocess.CalledProcessError) as error:
        raise SystemExit(f"native target rejected: {error}") from error


if __name__ == "__main__":
    main()
