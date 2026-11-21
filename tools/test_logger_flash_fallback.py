"""Check the flash fallback archive decisions of decision record 0004 on the host.

The archive target choice, the copy back step machine and the LIST merge of
the flash root are pure headers in firmware/common/logger/src. Each has a C++
test built with sanitizers. The target choice and the LIST merge only decide,
so a source check proves they never reach a file API that changes data.
"""
from __future__ import annotations

import argparse
from pathlib import Path
import re
import subprocess
import tempfile
from host_cxx import CXX_STD
from test_logger_codec_store import strip_comments

# Each test, the headers that must stay free of any data changing call, and
# the extra sources it links.
TESTS = {
    "archive-target": ("test_archive_target.cpp", ["os_logger_archive_target.h"], []),
    "flash-copy": ("test_flash_copy.cpp", [], ["src/sha256_stream.cpp"]),
    "list-merge": ("test_list_merge.cpp", ["os_logger_list_merge.h"], []),
    "roots": ("test_roots.cpp", ["os_logger_roots.h"], []),
}

FORBIDDEN = re.compile(
    r"\b(remove|unlink|rename|rmdir|truncate|ftruncate|f_unlink|f_rename|fopen|open_write)\s*\(")


def check_read_only(header: Path) -> None:
    match = FORBIDDEN.search(header.read_text())
    assert match is None, f"{header.name} calls {match.group(0)}"


def check_wiring(source: str) -> None:
    """os_logger.cpp uses a file's own root and the decision headers.

    Each rule names live code, outside comments, that the worker integration
    of decision record 0004 depends on. A file keeps its root from create to
    sidecar, so a card file never finishes on flash or the other way round.
    """
    code = " ".join(strip_comments(source).split())
    code = code.replace("( ", "(").replace(" )", ")")
    rules = {
        "create picks the root once, from the chosen target":
            r"target\.root = benchmark \? kBenchDirectory : next_root \? next_root : kDirectory;",
        "create makes the folder below the file's root":
            r"\"%s/%s\", target\.root, partition\);",
        "create names the partial below the file's root":
            r"\"%s/%s-%lu\.partial\", target\.root,",
        "finalize names the Parquet below the file's root":
            r"\"%s/%s-%lld-%lu\.parquet\", target\.root,",
        "the sidecar is written beside the file, below its root":
            r"const std::string_view root\{target\.root\};",
        "a failed file is quarantined below its own root":
            r"quarantine_one\(target\.root, relative, \"same-boot\"\)",
        "the card governor never sees a flash write":
            r"if \(commit_to_flash\(\)\) return flash_commit\(count, finalize, written\);",
        "a flash commit names the flash root for a new file":
            r"bool flash_commit\(std::size_t &count, bool finalize, bool \*written\) "
            r"\{ next_root = flash_root;",
        "an open file keeps its root":
            r"return open\.open\(\) \? on_flash\(open\) : choose_target\(\) == "
            r"archive_target::Target::Flash;",
        "LIST_SINCE falls back to the full listing while flash holds files":
            r"in\.force_full = list_merge::since_needs_full\(flash_holds_files\);",
        "a LIST_SINCE walk without the card hands out epoch 0":
            r"in\.card_listable = card_listable\(\);",
        "LIST and OPEN answer while either root is usable":
            r"archive_session\.handle\(request, archive_ready\(state\.storage_ready\)\)",
        "a probe finalizes an open flash file so the waiting rows test the card":
            r"if \(ok && free_bytes >= governor\.limits\(\)\.reserve\) \{ "
            r"if \(on_flash\(writer_state->telemetry\)\) \{ std::size_t none = 0; "
            r"flash_commit\(none, true, nullptr\); \} if \(count\) \{ "
            r"next_root = kDirectory; ok = commit\(count, false\) && "
            r"measure_free\(free_bytes\); \} \}",
        "a mount shifts the volume serial by the stored copy count":
            r"const std::uint32_t epoch = copy_count\.epoch\(copy_count_store, serial\);",
        "every mount takes its epoch from the copy count":
            r"mount_epoch = volume_epoch\(serial\);",
        "the copy count is stored before a file moves":
            r"if \(!copy_count\.reserve\(copy_count_store\)\) \{ log_copy_count_error\(\); "
            r"drain\.next_at = now \+ kCopyWaitSeconds; return false; \} "
            r"const auto begun = drain\.copier\.begin\(",
        "the copy count lives in the osflash namespace":
            r"nvs_set_u32\(flags, opensensor::storage::kFlashCopiesKey, count\)",
        "a copy back moves the mount epoch":
            r"copy_count\.copied\(\); mount_epoch = list_since::next_epoch\(mount_epoch\.load\(\)\);",
        "the copier hashes with its own stream":
            r"return sha\.begin\(\);",
        "the copy back state lives in PSRAM":
            r"heap_caps_calloc\(1, sizeof\(FlashDrain\), MALLOC_CAP_SPIRAM \| MALLOC_CAP_8BIT\)",
        "a sidecar waits for its Parquet":
            r"if \(\*kind == flash_copy::Kind::Sidecar && sidecar_waits\(name\)\) return true;",
        "the boot quarantine also covers the flash root":
            r"quarantine_directory\(flash_root, quarantined\);",
        "a crosslink suspect is logged and held once per file":
            r"PARQUET RECOVERY crosslink-suspect file=%s action=none\\n\", relative\); "
            r"if \(!drain\.held\.add\(hash\)\)",
    }
    for what, pattern in rules.items():
        assert len(re.findall(pattern, code)) == 1, f"os_logger.cpp wiring: {what}"
    # The copier never borrows the STAC writer's hash state.
    stac_uses = re.findall(r"stac_state->sha\b", code)
    assert len(stac_uses) == 2, f"stac_state->sha uses {len(stac_uses)}"


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sanitize", action="store_true")
    args = parser.parse_args()
    root = Path(__file__).resolve().parent.parent
    common = root / "firmware/common"
    logger = common / "logger"
    check_wiring((logger / "src/os_logger.cpp").read_text())
    with tempfile.TemporaryDirectory(prefix="os-flash-fallback-") as directory:
        for name, (source, read_only, extra) in TESTS.items():
            for header in read_only:
                check_read_only(logger / "src" / header)
            executable = Path(directory) / f"{name}-test"
            command = ["clang++", CXX_STD, "-Wall", "-Wextra", "-Werror",
                       "-I", str(logger / "src"), "-I", str(common / "src"),
                       str(logger / "tests" / source),
                       *[str(common / path) for path in extra],
                       "-o", str(executable)]
            if args.sanitize:
                command += ["-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                            "-fno-omit-frame-pointer"]
            subprocess.run(command, check=True)
            subprocess.run([str(executable)], check=True)
    print("flash fallback checks passed: archive target for every input and the "
          "reserve boundary, verified copy back with resets, conflicts and "
          "refused renames, the LIST merge of the flash root, the fallback root "
          "overlap rules and the storage worker wiring")


if __name__ == "__main__":
    main()
