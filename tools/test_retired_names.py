"""Host tests for check_retired_names.py and the way tools/lint.sh reads its exit code (#93)."""

from __future__ import annotations

from pathlib import Path
import subprocess
import sys
import tempfile

import check_retired_names as rn

OLD = "A" + "Q"
FIRES = [
    "mdns_service_add(nullptr, \"_" + "aq" + "sync\", \"_tcp\", 47390);",
    "constexpr char kMagic[] = \"" + OLD + "S1\";",
    "BASE = \"C0A5" + "E9F0-{:04x}-4b1a-9c3e-2d7f8a6b4e01\"",
    "| Service | " + OLD + " Sync |",
    "nvs_open(\"" + "aq" + "cfg\", NVS_READWRITE, &handle);",
    "The `" + "aq" + "dump` namespace holds one record.",
    "CONFIG_" + OLD + "_LOGGER_WORKER_WDT=y",
    "config " + OLD + "_PM_DFS_MIN_MHZ",
    "std::printf(\"" + OLD + " STATUS ok\\n\");",
    "if line.startswith('" + OLD + " LIVE'):",
    "Look for `" + OLD + " BOOT` in the log.",
    OLD + " WIFI connected",
    "std::printf(\"%s " + OLD + " READY\\n\", prefix);",
    "ESP_LOGI(kTag, \"boot " + OLD + " TF mounted\");",
    "log('[' + now + '] " + OLD + " OWNER_PIN set')",
    "aq" + "_pin_cxx_standard(${COMPONENT_LIB})",
    "aq" + "_map_source_paths(${COMPONENT_LIB})",
    "aq" + "_map_debug_paths()",
    "aq" + "_patch_nimble()",
    "aq" + "_enable_stack_usage()",
    "include(\"${CMAKE_CURRENT_LIST_DIR}/../cmake/" + "aq" + "_cxx_standard.cmake\")",
    "See firmware/cmake/" + "aq" + "_source_paths.cmake for the map.",
    "include(../cmake/" + "aq" + "_nimble_patches.cmake)",
    "include(../cmake/" + "aq" + "_stack_usage.cmake)",
    "set(stage \"${CMAKE_BINARY_DIR}/" + "aq" + "_nimble_patch\")",
    "-ffile-prefix-map=${" + OLD + "_SOURCE_ROOT}=.",
    "if(NOT " + OLD + "_SOURCE_ROOT_REAL STREQUAL root)",
    "set(" + OLD + "_NIMBLE_PATCH_DIR dir)",
    "find_program(" + OLD + "_PATCH_PROGRAM patch REQUIRED)",
    OLD + "_BENCH=stall pixi run cores3-build",
    "idf_args=(-D \"" + OLD + "_VARIANT=${variant}\")",
    "set(" + OLD + "_SDKCONFIG_OVERLAY \"\" CACHE FILEPATH \"x\")",
    "printf '" + OLD + "_APP_SIGNING_KEY=k.pem' > secrets/.env",
    "KEY_VAR = \"" + OLD + "_APP_SIGNING_SCHEME\"",
    "task_timeout=${" + OLD + "_GATE_TASK_TIMEOUT:-300}",
    "export IDF_TOOLS_PATH=\"${" + OLD + "_IDF_TOOLS_PATH:-${HOME}/.espressif}\"",
    "f'-D" + OLD + "_FIELDS_INC=x'",
    "(\"rom\", [\"-D" + OLD + "_SYNC_ROM_CRC\"])",
    "toolchain_root=\"${M5_" + "TOOLCHAIN_ROOT:-x}\"",
    "idf_path=\"$M5_" + "TOOLCHAIN_ROOT/esp-idf-v6.1\"",
    "idf_path=\"${M5_" + "TOOLCHAIN_ROOT}/esp-idf-v6.1\"",
    "root = os.environ[\"M5_" + "TOOLCHAIN_ROOT\"]",
    "root = os.environ.get('M5_" + "TOOLCHAIN_ROOT')",
    "root=\"${XDG_CACHE_HOME}/m5stack-" + "aq" + "-parquet/toolchains\"",
    "root=\"${XDG_CACHE_HOME:-x}/m5stack-" + "aq" + "-parquet\"",
    "cp \"${PROJECT}/build/" + "aq" + "-build-sha256.json\" size/",
    "rm -f build/" + "aq" + "-build-target.txt",
    "~/.cache/m5stack-" + "aq" + "-parquet/toolchains/esp-idf-v6.1",
    "name = \"" + "aq" + "-parquet\"",
    "set(COMPONENTS " + "aq" + "_cores3)",
    "[manifest](components/" + "aq" + "_waveshare_logger/idf_component.yml)",
    "components/" + "aq" + "_waveshare_diagnostic/CMakeLists.txt",
    "set(COMPONENTS " + "aq" + "_waveshare_${OS_VARIANT})",
    "#include \"" + "aq" + "_logger.h\"",
    "#include <" + "aq" + "_logger_live.h>",
    "    SRCS \"src/" + "aq" + "_logger_status.cpp\"",
    "project(" + "aq" + "_logger_fixture)",
    "project(" + "aq" + "_connectivity_fixture)",
    "project(" + "aq" + "_psram_probe)",
    "#include \"" + "aq" + "_location.h\"",
    "#include <" + "aq" + "_battery_health.h>",
    "    SRCS \"src/" + "aq" + "_console.cpp\"",
    "#include \"../firmware/common/sensors/src/" + "aq" + "_ltr553.h\"",
    "#include <" + "aq" + "_max17048.h>",
]
QUIET = [
    "mdns_service_add(nullptr, \"_ossync\", \"_tcp\", 47390);",
    "constexpr char kMagic[] = \"OSS1\";",
    "nvs_open(\"oscfg\", NVS_READWRITE, &handle);",
    "CONFIG_OS_LOGGER_WORKER_WDT=y",
    "std::printf(\"OS STATUS ok\\n\");",
    "\"AD AE AF AG AI AL AM AO " + OLD + " AR AS AT\"",
    "for (const char *code : {\"EG\", \"US\", \"" + OLD + "\"})",
    "PARQUET LIST ok",
    "os_logger.cpp stays, only wire and build names moved.",
    "#ifndef OS_SHARED_H",
    "OS_BENCH=stall pixi run cores3-build",
    "cp build/os-build-sha256.json size/",
    "toolchain_root=\"${OS_TOOLCHAIN_ROOT:-${HOME}/.cache/opensensor-space-firmware/toolchains}\"",
    "the tool lives under `~/.cache/m5stack-" + "aq" + "-parquet/toolchains/arduino/data/packages`",
    "Setup keeps SDKs below `$M5_TOOLCHAIN_ROOT`, which defaults outside the repository.",
    "the tool lives under `$M5_TOOLCHAIN_ROOT/toolchains/arduino/data/packages`",
    "\"created_by\": \"m5stack-" + "aq" + "-parquet version 0.1\"",
    "name = \"opensensor-space-firmware\"",
    "constexpr char kIso[] = \"AN AO " + OLD + " AR AS\";",
    "Tested near " + OLD + " READY state in prose without a literal.",
    "printf(\"%s OS READY\\n\", prefix);",
    "os_pin_cxx_standard(${COMPONENT_LIB})",
    "include(../cmake/os_nimble_patches.cmake)",
    "-ffile-prefix-map=${OS_SOURCE_ROOT}=.",
    "set(stage \"${CMAKE_BINARY_DIR}/os_nimble_patch\")",
    "set(COMPONENTS os_cores3)",
    "set(COMPONENTS os_waveshare_${OS_VARIANT})",
    "artifacts/reflash-20261011/" + "aq" + "_waveshare.elf",
    "#include \"os_logger_live.h\"",
    "project(os_logger_fixture)",
    "#include <os_battery_health.h>",
    "str(root / \"tools/host_fakes/config/os_console.cpp\"),",
    "aq_location_cell is not a name we use.",
]


def check_patterns() -> None:
    for line in FIRES:
        assert len(rn.scan_text(line)) == 1, line
    for line in QUIET:
        assert rn.scan_text(line) == [], line


def check_files() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        (root / "docs/shared").mkdir(parents=True)
        (root / "docs/shared/mobile-migration.md").write_text(FIRES[0] + "\n")
        (root / "docs/shared/new.md").write_text("ok\n" + FIRES[1] + "\n")
        (root / "pixi.lock").write_text(FIRES[0] + "\n")
        names = ["docs/shared/mobile-migration.md", "docs/shared/new.md", "pixi.lock", "gone.md"]
        # History and lock files are exempt, a missing path is skipped.
        assert rn.findings(root, names) == ["docs/shared/new.md:2: retired LAN handshake"]


def check_repo() -> None:
    # The tracked tree is clean, and the checker scans itself without a hit.
    names = rn.tracked(rn.ROOT)
    assert names is not None and "tools/check_retired_names.py" in names
    assert rn.findings(rn.ROOT, names) == []
    # Every board.json board's dated bench record is history, and exists (#141).
    records = {name for name in rn.exempt() if name.startswith("docs/boards/")}
    assert records == {f"docs/boards/{b.slug}/bench-verified.md" for b in rn.boards.boards()}, records
    assert records and all(name in names for name in records), records


def check_exit_codes() -> None:
    script = Path(__file__).resolve().parent / "check_retired_names.py"
    with tempfile.TemporaryDirectory() as tmp:
        bad = Path(tmp) / "bad.md"
        bad.write_text(FIRES[4] + "\n")
        result = subprocess.run([sys.executable, str(script), str(bad)], capture_output=True, text=True)
        assert result.returncode == 1 and "retired NVS namespace" in result.stdout, result
        # A name the phone or a stored blob sees points to the migration log,
        # a build or environment name to the development guide.
        assert "(docs/shared/mobile-migration.md)" in result.stdout, result
        build = Path(tmp) / "build.sh"
        build.write_text(OLD + "_BENCH=stall\n" + "aq" + "_pin_cxx_standard(x)\n")
        result = subprocess.run([sys.executable, str(script), str(build)], capture_output=True, text=True)
        lines = result.stdout.splitlines()
        assert result.returncode == 1 and len(lines) == 2, result
        assert all(line.endswith("(docs/shared/development.md)") for line in lines), result
        good = Path(tmp) / "good.md"
        good.write_text(QUIET[0] + "\n")
        result = subprocess.run([sys.executable, str(script), str(good)], capture_output=True, text=True)
        assert result.returncode == 0 and result.stdout == "", result
    # Outside any repo git cannot list files, which must not read as clean.
    with tempfile.TemporaryDirectory() as tmp:
        assert rn.tracked(Path(tmp)) is None


def main() -> None:
    check_patterns()
    check_files()
    check_repo()
    check_exit_codes()
    print(f"PASS retired names, {len(FIRES)} fire, {len(QUIET)} quiet, history exempt, exit codes 0 and 1, an unlisted repo is never clean")


if __name__ == "__main__":
    main()
