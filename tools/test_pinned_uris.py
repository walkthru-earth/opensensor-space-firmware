#!/usr/bin/env python3
"""Gate and mutation tests of the pinned dictionary and template URIs (#89).

The checked-in pins must pass tools/pinned_uris.py check() against this clone.
Each check is then proved on a scratch git repository built from the board
files, with no network. A main URL, a branch URL, a tag, a short sha, a blob
URL, another path, a commit absent from the clone, a commit off HEAD, a commit
whose bytes differ, a shallow clone and a describedby link that is not the
dictionary URI must each fail with their own message. pin-uris must pin the
commits that hold the bytes, refuse an uncommitted change to any file it reads
or writes (pinned files, digest headers, telemetry_template.h and
telemetry_contract.h) and change nothing on a second run. The firmware static_assert helpers in pinned_uri.h are compiled
with the same accepted and rejected URLs.
"""

from __future__ import annotations

from pathlib import Path
import shutil
import subprocess
import tempfile

import boards
from host_cxx import CXX_STD
import pinned_uris as gate
import published_templates

ROOT = gate.ROOT


def rejects(action, expected: str, mutation: str) -> None:
    try:
        action()
    except AssertionError as error:
        if expected not in str(error):
            raise AssertionError(f"{mutation} failed for another reason, {error}") from error
        print(f"PASS rejects {mutation}", flush=True)
        return
    raise AssertionError(f"{mutation} passed the gate")


def run(repo: Path, *args: str) -> str:
    return subprocess.run(["git", "-C", str(repo), *args], check=True, capture_output=True,
                          text=True).stdout.strip()


def commit(repo: Path, message: str) -> str:
    run(repo, "add", "-A")
    run(repo, "commit", "-q", "--allow-empty", "-m", message)
    return run(repo, "rev-parse", "HEAD")


def scratch_repo(scratch: Path) -> Path:
    """A git repository with the board files the gate reads, at the current bytes."""
    repo = scratch / "repo"
    shutil.copytree(ROOT / published_templates.TEMPLATES, repo / published_templates.TEMPLATES)
    for board in boards.boards(ROOT):
        for name in ("board.json", "telemetry_template.h", "telemetry_descriptor_digest.h",
                     "telemetry_dictionary_digest.h", "telemetry_contract.h", "telemetry_fields.inc"):
            target = repo / board.app / name
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy(ROOT / board.app / name, target)
    run(repo, "init", "-q", "-b", "main")
    run(repo, "config", "user.email", "gate@example.invalid")
    run(repo, "config", "user.name", "gate")
    run(repo, "config", "commit.gpgsign", "false")
    return repo


def check_static_asserts(scratch: Path) -> None:
    sha = "0123456789abcdef0123456789abcdef01234567"
    dict_ = "4f80c26cde8d7ea78c2160225d98386009a8651d9a049b4bf68a75da96015e94"
    path = "firmware/esp-idf-cores3/bringup/telemetry_fields.inc"
    raw = gate.RAW
    good = f"{raw}{sha}/{path}"
    template = f"{raw}{sha}/docs/shared/templates/m5stack-cores3/{dict_}.json"
    bad = {
        "main URL": f"{raw}main/{path}",
        "branch URL": f"{raw}fix/89-pin-uris/{path}",
        "tag URL": f"{raw}v6.19/{path}",
        "short sha": f"{raw}{sha[:7]}/{path}",
        "upper case sha": f"{raw}{sha.upper()}/{path}",
        "blob URL": f"https://github.com/walkthru-earth/opensensor-space-firmware/blob/{sha}/{path}",
        "other repository": f"https://raw.githubusercontent.com/walkthru-earth/other/{sha}/{path}",
        "other path": f"{raw}{sha}/firmware/esp-idf-cores3/bringup/telemetry_semantics.inc",
    }
    lines = ["#include <pinned_uri.h>", "using namespace opensensor::pinned;",
             f'static_assert(names("{good}", "{path}"));',
             f'static_assert(names_template("{template}", "m5stack-cores3", "{dict_}"));',
             f'static_assert(!names_template("{template}", "m5stack-cores3", "{"0" * 64}"));',
             f'static_assert(!names_template("{template}", "waveshare-esp32-s3-sim7670g", "{dict_}"));',
             f'static_assert(!names_template("{raw}main/docs/shared/templates/m5stack-cores3/{dict_}.json", '
             f'"m5stack-cores3", "{dict_}"));']
    lines += [f'static_assert(!names("{uri}", "{path}"), "{name}");' for name, uri in bad.items()]
    source = scratch / "pinned_uri_test.cpp"
    source.write_text("\n".join(lines) + "\n")
    subprocess.run(["clang++", CXX_STD, "-fsyntax-only", "-Wall", "-Wextra", "-Werror",
                    f"-I{ROOT / 'firmware/common/src'}", str(source)], check=True)
    print(f"PASS pinned_uri.h accepts a full commit and rejects {', '.join(bad)}, another dict and another board",
          flush=True)


def main() -> None:
    for pin, uri in gate.check():
        print(f"PASS {pin.board} {pin.name} {uri}", flush=True)

    with tempfile.TemporaryDirectory(prefix="pinned-uris-") as temporary:
        scratch = Path(temporary)
        check_static_asserts(scratch)
        repo = scratch_repo(scratch)
        pins = gate.pins(repo)
        dictionary, template = pins[0], pins[1]
        gate.require((dictionary.name, template.name) == (gate.DICTIONARY, gate.TEMPLATE),
                     "first board pins are the dictionary then the template")

        # C0 holds other bytes at every pinned path, C1 the current bytes.
        for pin in pins:
            (repo / pin.path).write_bytes((repo / pin.path).read_bytes() + b" ")
        wrong = commit(repo, "other bytes")
        for pin in pins:
            shutil.copy(ROOT / pin.path, repo / pin.path)
        content = commit(repo, "content")

        # pin-uris pins the dictionary first and stops, since the template embeds it.
        changed = gate.write(repo)
        gate.require(changed and all(gate.DICTIONARY in line and content in line for line in changed),
                     f"pin-uris pins kDictionaryUri to the content commit first, {changed}")
        commit(repo, "dictionary pin")
        changed = gate.write(repo)
        gate.require(changed and all(gate.TEMPLATE in line and content in line for line in changed),
                     f"pin-uris then pins kTemplateHref to the content commit, {changed}")
        commit(repo, "template pin")
        gate.require(gate.write(repo) == [], "pin-uris changes nothing on a second run")
        print("PASS pin-uris pins the content commit in order and is idempotent", flush=True)
        for pin in gate.pins(repo):
            gate.check_pin(repo, pin, gate.read_constant(gate.contract_path(repo, pin.board), pin.name))
        print("PASS the pins written by pin-uris pass check_pin", flush=True)

        side_branch = run(repo, "rev-parse", "--abbrev-ref", "HEAD")
        run(repo, "checkout", "-q", "-b", "side")
        side = commit(repo, "same bytes, off HEAD")
        run(repo, "checkout", "-q", side_branch)
        absent = "f" * 40

        for pin in (dictionary, template):
            good = f"{gate.RAW}{content}/{pin.path}"
            gate.check_pin(repo, pin, good)
            cases = (
                ("main URL", f"{gate.RAW}main/{pin.path}", "is not a raw URL at a full 40 hex commit"),
                ("branch URL", f"{gate.RAW}fix/89-pin-uris/{pin.path}", "is not a raw URL at a full 40 hex commit"),
                ("tag URL", f"{gate.RAW}v6.19/{pin.path}", "is not a raw URL at a full 40 hex commit"),
                ("short sha", f"{gate.RAW}{content[:12]}/{pin.path}", "is not a raw URL at a full 40 hex commit"),
                ("blob URL", f"https://github.com/{gate.REPO}/blob/{content}/{pin.path}",
                 "is not a raw URL at a full 40 hex commit"),
                ("other path", f"{gate.RAW}{content}/{pin.path}.old", "names"),
                ("commit absent from the clone", f"{gate.RAW}{absent}/{pin.path}", "which is not in this clone"),
                ("commit off HEAD", f"{gate.RAW}{side}/{pin.path}", "which is not an ancestor of HEAD"),
                ("pinned commit whose bytes differ", f"{gate.RAW}{wrong}/{pin.path}", "whose bytes hash to"),
            )
            for mutation, uri, expected in cases:
                rejects(lambda uri=uri, pin=pin: gate.check_pin(repo, pin, uri), expected,
                        f"{pin.board} {pin.name} {mutation}")
            # Every failure that a re-pin fixes names the merge commit rule.
            try:
                gate.check_pin(repo, pin, f"{gate.RAW}{side}/{pin.path}")
            except AssertionError as error:
                gate.require(gate.RULE in str(error), "the gate failure states the merge commit rule")

        source = published_templates.source(repo, boards.boards(repo)[0])
        rejects(lambda: gate.check_describedby(source.raw, source.board.slug, f"{gate.RAW}{content}/x"),
                "describedby href is not kDictionaryUri", "describedby link that is not kDictionaryUri")

        # A template changed on disk but not committed cannot be pinned.
        (repo / template.path).write_bytes((repo / template.path).read_bytes() + b" ")
        rejects(lambda: gate.write(repo), "needs the pinned files committed", "pin-uris with uncommitted content")
        run(repo, "checkout", "-q", "--", template.path)
        # Nor with any other file pin-uris reads or writes left uncommitted.
        for board in boards.boards(ROOT):
            for name in ("telemetry_contract.h", "telemetry_template.h", "telemetry_dictionary_digest.h",
                         "telemetry_descriptor_digest.h"):
                header = repo / board.app / name
                original = header.read_bytes()
                header.write_bytes(original + b"\n")
                rejects(lambda: gate.write(repo), "needs the pinned files committed",
                        f"pin-uris with uncommitted {board.slug} {name}")
                header.write_bytes(original)
        # The registry steers every pin, so a board.json added, edited or
        # removed on disk stops pin-uris before it reads anything.
        manifest = repo / boards.boards(repo)[0].manifest
        original = manifest.read_bytes()
        manifest.write_bytes(original + b"\n")
        rejects(lambda: gate.write(repo), "needs every board.json committed", "pin-uris with an edited board.json")
        manifest.unlink()
        rejects(lambda: gate.write(repo), "needs every board.json committed", "pin-uris with a removed board.json")
        manifest.write_bytes(original)
        added = repo / "firmware/esp-idf-new-board/app/board.json"
        added.parent.mkdir(parents=True)
        added.write_bytes(original)
        rejects(lambda: gate.write(repo), "needs every board.json committed", "pin-uris with an added board.json")
        shutil.rmtree(repo / "firmware/esp-idf-new-board")
        gate.require(gate.write(repo) == [], "pin-uris runs once the headers are clean again")
        (repo / template.path).write_bytes((repo / template.path).read_bytes() + b" ")
        # Committed bytes that do not hash to desc are refused, never pinned.
        stale = commit(repo, "stale template")
        rejects(lambda: gate.write(repo), "Commit the regenerated files", "pin-uris on bytes that miss desc")
        run(repo, "reset", "-q", "--hard", f"{stale}~1")

        shallow = scratch / "shallow"
        subprocess.run(["git", "clone", "-q", "--depth", "1", f"file://{repo}", str(shallow)], check=True)
        rejects(lambda: gate.check(shallow), "shallow clone", "shallow clone")

    print("PASS pinned URIs are raw at a commit on HEAD with the right bytes, and each check rejects its mutation",
          flush=True)


if __name__ == "__main__":
    main()
