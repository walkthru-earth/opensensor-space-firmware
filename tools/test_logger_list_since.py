"""Check the LIST_SINCE ring, plan and incremental walk on a real directory tree."""
from __future__ import annotations

import argparse
from pathlib import Path
import re
import subprocess
import tempfile
from host_cxx import CXX_STD
from test_logger_codec_store import strip_comments


def check_wiring(source: str) -> None:
    """os_logger.cpp feeds the Tracker, the ring and the marker at the right points.

    The helpers run in the C++ test below. Here the live code must call them,
    outside comments, where a missed file would otherwise slip through.
    """
    code = " ".join(strip_comments(source).split())
    # clang-format may break a line after "(" or before ")".
    code = code.replace("( ", "(").replace(" )", ")")
    rules = {
        "a dated file lowers the floor before its folder is made":
            r"target\.dated = true; since_state\.dated\([^;]*\); \} else \{ .*?"
            r"note_unsynced_boot\(\); \} char directory\[192\];.*?"
            r"if \(!make_directories\(directory\)\)",
        "the unsynced boot joins the ring, or gets a marker, before its folder":
            r"void note_unsynced_boot\(\) \{ if \(boot_ring\.appended\(\)\) return; "
            r"boot_ring\.note_unsynced\(ring_store\); char marker\[192\] = \{\}; "
            r"const bool named = list_since::marker_path\(marker, kDirectory, "
            r"thing_text, boot_text\); if \(boot_ring\.appended\(\)\) \{.*?"
            r"if \(!boot_ring\.stale_marker\(\)\) return;.*?"
            r"if \(::rmdir\(marker\) != 0\).*?"
            r"else boot_ring\.marker_removed\(\); return; \}.*?"
            r"if \(!boot_ring\.needs_marker\(\)\) return;.*?"
            r"if \(named && make_directories\(marker\)\) boot_ring\.marker_made\(\);",
        "a renamed unsynced file moves the cursor seq":
            r"::rename\(target\.partial, ready\) == 0;.*?\+\+finalized; "
            r"unlisted_files = true; since_state\.finalized\(target\.benchmark, "
            r"target\.dated, target\.last\);",
        "LIST_SINCE plans from the tracker when its walk starts":
            r"if \(start\) \{ cancel\(\); if \(!since_\) \{ errno = ENOTSUP; return fail\(\); \} "
            r"auto in = since_state\.inputs\(cursor, "
            r"mount_epoch\.load\(\), utc_yesterday\(\), boot_ring, ring_store\); "
            r"in\.card_listable = card_listable\(\); in\.force_full = [^;]*; "
            r"const auto plan = list_since::plan\(in\); result = plan\.result;",
        "a widened walk reports unsynced_full":
            r"if \(since_->widened\(\)\) result\.flags \|= ossync::kListSinceUnsyncedFull;",
        "the ring knows this boot":
            r"if \(list_since::parse_boot\(boot_text, boot\)\) boot_ring\.set_current\(boot\);",
        "a remount forgets the mount epoch":
            r"archive_session\.remounted\(\);.*?mount_epoch = 0;",
        "only the radio list owns the incremental walk":
            r"ArchiveList archive_list\{&archive_since\};",
        "list_all keeps the incremental walk off the worker stack":
            r"bool list_all\(FileEmitter emit, void \*context, unsigned &partials\) \{ ArchiveList list;",
        "STATUS reports the mount epoch":
            r"view\.mount_epoch = mount_epoch\.load\(\);",
    }
    for what, pattern in rules.items():
        assert len(re.findall(pattern, code)) == 1, f"os_logger.cpp wiring: {what}"
    calls = re.findall(r"since_state\.(\w+)\(", code)
    assert sorted(calls) == ["dated", "finalized", "inputs"], f"since_state calls {calls}"


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sanitize", action="store_true")
    args = parser.parse_args()
    root = Path(__file__).resolve().parent.parent
    common = root / "firmware/common"
    check_wiring((common / "logger/src/os_logger.cpp").read_text())
    with tempfile.TemporaryDirectory(prefix="aq-logger-list-since-") as directory:
        executable = Path(directory) / "list-since-test"
        command = ["clang++", CXX_STD, "-Wall", "-Wextra", "-Werror"]
        for library in ["src", "runtime/src", "connectivity/src", "logger/src"]:
            command += ["-I", str(common / library)]
        command += [str(common / "logger/tests/test_list_since.cpp"), "-o", str(executable)]
        if args.sanitize:
            command += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
        subprocess.run(command, check=True)
        subprocess.run([str(executable), directory], check=True)
    print("LIST_SINCE checks passed: dates, names, cursor wire, boot ring, NVS failure fallback, plan, bounded incremental walk on a 5000+ file tree, new files across boots and clock moves, ringless boots, logger wiring")


if __name__ == "__main__":
    main()
