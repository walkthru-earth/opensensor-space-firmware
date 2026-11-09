"""Offline publication tests for backup-flash.sh; no serial device is opened."""

from __future__ import annotations

import hashlib
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

import boards


BOARD = "waveshare-sim7670g-v2"
STAMP = "20260930T123456Z"


def make_image(path: Path, marker: bytes) -> None:
    with path.open("wb") as stream:
        stream.write(marker)
        stream.truncate(16_777_216)


def copy_registry(root: Path) -> None:
    """tools/boards.py and every board.json, so the script resolves boards offline (#141)."""
    shutil.copy2(Path(__file__).with_name("boards.py"), root / "tools/boards.py")
    for board in boards.boards():
        target = root / board.manifest
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(boards.ROOT / board.manifest, target)


def run_case(script: Path, fake_bin: Path, mode: str, source: Path, board: str = BOARD,
             stamp: str = STAMP) -> subprocess.CompletedProcess[str]:
    environment = os.environ.copy()
    environment.update(
        PATH=f"{fake_bin}{os.pathsep}{environment['PATH']}",
        OS_FAKE_ESPTOOL_MODE=mode,
        OS_FAKE_SOURCE=str(source),
        OS_FAKE_STAMP=stamp,
        OS_REAL_PYTHON=sys.executable,
    )
    return subprocess.run(
        [str(script), "--board", board, "--port", "/dev/fake-checked-port"],
        env=environment,
        capture_output=True,
        text=True,
        check=False,
    )


def main() -> None:
    source_script = Path(__file__).with_name("backup-flash.sh")
    with tempfile.TemporaryDirectory(prefix="aq-backup-test-") as directory:
        root = Path(directory)
        tools = root / "tools"
        tools.mkdir()
        script = tools / "backup-flash.sh"
        shutil.copy2(source_script, script)
        copy_registry(root)
        fake_bin = root / "fake-bin"
        fake_bin.mkdir()
        (fake_bin / "date").write_text("#!/bin/sh\nprintf '%s\\n' \"$OS_FAKE_STAMP\"\n")
        # The board registry runs on the real interpreter, esptool never does.
        (fake_bin / "python").write_text(
            "#!/bin/sh\n"
            "case \"$1\" in */tools/boards.py) exec \"$OS_REAL_PYTHON\" \"$@\" ;; esac\n"
            "for arg do output=$arg; done\n"
            "case \"$OS_FAKE_ESPTOOL_MODE\" in\n"
            "  fail) printf partial > \"$output\"; exit 8 ;;\n"
            "  short) printf short > \"$output\"; exit 0 ;;\n"
            "  complete) cp \"$OS_FAKE_SOURCE\" \"$output\" ;;\n"
            "  *) exit 9 ;;\n"
            "esac\n"
        )
        for name in ("date", "python"):
            (fake_bin / name).chmod(0o755)

        first = root / "first.bin"
        second = root / "second.bin"
        make_image(first, b"original")
        make_image(second, b"changed")
        backup = root / "backup"
        published = backup / f"{BOARD}-flash-{STAMP}.bin"

        for mode in ("fail", "short"):
            result = run_case(script, fake_bin, mode, first)
            assert result.returncode != 0, result
            assert not published.exists(), result
            assert not list(backup.glob(".*")), list(backup.iterdir())

        result = run_case(script, fake_bin, "complete", first)
        assert result.returncode == 0, result
        assert published.stat().st_size == 16_777_216
        original_hash = hashlib.sha256(first.read_bytes()).hexdigest()
        assert hashlib.sha256(published.read_bytes()).hexdigest() == original_hash
        assert original_hash in result.stdout and "backup verified" in result.stdout
        assert not list(backup.glob(".*")), list(backup.iterdir())

        collision = run_case(script, fake_bin, "complete", second)
        assert collision.returncode != 0, collision
        assert "already exists" in collision.stderr, collision
        assert hashlib.sha256(published.read_bytes()).hexdigest() == original_hash
        assert not list(backup.glob(".*")), list(backup.iterdir())

        # The alias and the footer slug name the image by the first backup prefix.
        for index, name in enumerate(("waveshare", "waveshare-esp32-s3-sim7670g")):
            stamp = f"20260930T12350{index}Z"
            result = run_case(script, fake_bin, "complete", first, name, stamp)
            assert result.returncode == 0, result
            assert (backup / f"{BOARD}-flash-{stamp}.bin").stat().st_size == 16_777_216, result
        # A name no board.json declares, or no name, fails before esptool with no image.
        before = sorted(backup.iterdir())
        for name in ("waveshare-v3", ""):
            result = run_case(script, fake_bin, "unexpected", first, name, "20260930T123599Z")
            assert result.returncode == 2, result
            assert ("unknown board 'waveshare-v3'" if name else "usage") in result.stderr, result
            assert sorted(backup.iterdir()) == before, list(backup.iterdir())
    print("PASS backup publication: failed, short, verified and same-name collision readbacks, "
          "board alias and slug, unknown board refused")


if __name__ == "__main__":
    main()
