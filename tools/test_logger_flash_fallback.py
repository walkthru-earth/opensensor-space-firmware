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
    "copy-candidates": ("test_copy_candidates.cpp", ["os_logger_copy_candidates.h"], []),
    "thing-id": ("test_thing_id.cpp", [], []),
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
        "a probe asks probe_action whether to close the flash file and test the card (#219)":
            r"const auto plan = archive_target::probe_action\("
            r"\{\.space_ok = free_bytes >= governor\.limits\(\)\.reserve, "
            r"\.card_writable = storage::writable\(storage::probe_state\("
            r"previous, free_bytes, governor\.limits\(\)\)\), "
            r"\.rows_waiting = count != 0, "
            r"\.flash_open = on_flash\(writer_state->telemetry\), "
            r"\.flash_takes_rows = commit_to_flash\(\), "
            r"\.writes = card_writes, \.failed_age_s = now - card_failed_at\}\);",
        "only FinalizeWrite closes the open flash file at a probe":
            r"if \(plan == FinalizeWrite\) \{ std::size_t none = 0; "
            r"flash_commit\(none, true, nullptr\); \}",
        "the probe write test records what the card showed":
            r"if \(plan == Write \|\| plan == FinalizeWrite\) \{ next_root = kDirectory; "
            r"if \(commit\(count, false\)\) \{ card_writes = archive_target::CardWrites::Passed; "
            r"ok = measure_free\(free_bytes\); \} else \{ "
            r"card_writes = archive_target::CardWrites::Failed; card_failed_at = now; ok = false; \} "
            r"\} else if \(plan == Defer\) \{ ok = false; \}",
        "a card commit that wrote marks the card as passed":
            r"\*written = ok; if \(ok\) card_writes = archive_target::CardWrites::Passed;",
        "a refused copy write fails an untested card (#218)":
            r"void copy_failed\(flash_copy::Failure failure, int error, std::int64_t now\) \{ "
            r"if \(archive_target::copy_fails_card\(card_writes, "
            r"flash_copy::card_write_failure\(failure, error\)\)\) \{ "
            r"const auto previous = governor\.state\(\); "
            r"card_writes = archive_target::CardWrites::Failed; card_failed_at = now; "
            r"governor\.write_failed\(now\); publish_storage\(previous\); \} \}",
        "every failed copy reaches the card check with the copier error":
            r"worker::copy_failed\(failure, drain\.copier\.error\(\), now\);",
        "a mount shifts the volume serial by the stored copy count, with or "
        "without a flash root (#217)":
            r"std::uint32_t volume_epoch\(std::uint32_t serial\) \{ "
            r"const std::uint32_t epoch = copy_count\.epoch\(copy_count_store, serial\);",
        "every mount takes its epoch from the copy count":
            r"mount_epoch = volume_epoch\(serial\);",
        "the copy count is stored before a file moves":
            r"if \(!worker::reserve_copy_count\(\)\) \{ wait_a_pass\(drain, now\); "
            r"return false; \} const auto begun = drain\.copier\.begin\(",
        "a refused copy count store logs once and holds the file":
            r"bool reserve_copy_count\(\) \{ if \(copy_count\.reserve\(copy_count_store\)\) "
            r"return true; log_copy_count_error\(\); return false; \}",
        "the copy count lives in the osflash namespace":
            r"nvs_set_u32\(flags, opensensor::storage::kFlashCopiesKey, count\)",
        "a committed copy measures card free space before the next pass (#213)":
            r"measure_flash\(\); if \(copied\) \{ "
            r"const auto previous = governor\.state\(\); std::uint64_t free_bytes = 0; "
            r"if \(measure_free\(free_bytes\)\) governor\.observe_free\(free_bytes, now\); "
            r"else governor\.write_failed\(now\); publish_storage\(previous\); \}",
        "a copy back moves the mount epoch":
            r"copy_count\.copied\(\); mount_epoch = list_since::next_epoch\(mount_epoch\.load\(\)\);",
        "a finalized or failed flash file refreshes the copy candidates (#215)":
            r"if \(!ok \|\| !writer_state->telemetry\.open\(\)\) copy_back::flash_changed\(\);",
        "a cancelled copy drops the copy candidates (#215)":
            r"void cancel\(const char \*reason\) \{",
        "the copy candidates are filled from one walk and taken in order (#215)":
            r"drain\.candidates\.offer\(",
        "the copier hashes with its own stream":
            r"return sha\.begin\(\);",
        "the copy back state lives in PSRAM":
            r"heap_caps_calloc\(1, sizeof\(FlashDrain\), MALLOC_CAP_SPIRAM \| MALLOC_CAP_8BIT\)",
        "a sidecar waits for its Parquet":
            r"if \(\*kind == flash_copy::Kind::Sidecar && sidecar_waits\(name\)\) return true;",
        "the boot quarantine also covers the flash root":
            r"quarantine_directory\(flash_root, quarantined\);",
        "a copy conflict sets the flash file aside under its own name (#212)":
            r"const auto aside = drain\.copier\.set_aside\(drain\.io\); if \(aside\) \{ "
            r"oslog\.printf\(\"OS FLASH conflict name=%s moved=%s%s sidecar=%d\\n\", "
            r"relative, flash_copy::kConflictPrefix, relative, \*aside \? 1 : 0\); "
            r"remove_empty_flash_directories\(relative\); drain\.candidates\.clear\(\); "
            r"break; \}",
        "only a committed copy counts as a card write":
            r"const bool copied = outcome == flash_copy::Outcome::Copied;",
        "a moved file moves the epoch, then the governor measures":
            r"worker::file_moved\(copied\); remove_empty_flash_directories\(relative\); "
            r"worker::measure_after_move\(copied, now\);",
        "a set aside that may pass later is retried, any other is held":
            r"if \(flash_copy::aside_retries\(aside\.error\(\)\)\) \{ "
            r"if \(!drain\.failed\.add\(hash\)\) wait_a_pass\(drain, now\); "
            r"\} else if \(!drain\.held\.add\(hash\)\)",
        "a set aside sidecar waits for its Parquet under either name":
            r"if \(flash_copy::set_aside_name\(relative\)\) \{ "
            r"stem\.remove_prefix\(flash_copy::kConflictPrefixLength\); names = 2; \}",
        "the radio LIST walks list STAC sidecars (#143)":
            r"ArchiveList archive_list\{&archive_since, true\};",
        "the serial list and the startup scan stay Parquet only":
            r"bool list_all\(FileEmitter emit, void \*context, unsigned &partials\) "
            r"\{ ArchiveList list;",
        "a sidecar follows its Parquet in LIST":
            r"return emit\(context, exported, static_cast<std::uint32_t>\(info\.st_size\)\) "
            r"&& \(!sidecars \|\| emit_sidecar\(exported\)\);",
        "a sidecar is looked up where OPEN finds it":
            r"list_merge::sidecar\(probe, card_listable\(\) \? kDirectory : nullptr, "
            r"flash_ready \? flash_root : nullptr, json, flash_phase\);",
        "the flash walk marks its Parquet files as on flash":
            r"\.sidecars = sidecars_, \.flash_phase = phase_ == Phase::Flash\};",
        "OPEN checks a sidecar name as a sidecar":
            r"return check_archive_file\(path, file, check\);",
        "a sidecar needs its Parquet on a root that pairs with it":
            r"!list_merge::sidecar_pairs\(sidecar_on_flash, parquet_on_flash\)\) return false;",
        "a crosslink suspect is logged and held once per file":
            r"PARQUET RECOVERY crosslink-suspect file=%s action=none\\n\", relative\); "
            r"if \(!drain\.held\.add\(hash\)\)",
    }
    for what, pattern in rules.items():
        assert len(re.findall(pattern, code)) == 1, f"storage worker wiring: {what}"
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
    # Copy back has its own translation unit since #215, the rules read both.
    check_wiring("\n".join((logger / "src" / name).read_text() for name in
                           ("os_logger.cpp", "os_logger_copy_back.cpp")))
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
