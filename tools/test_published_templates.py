#!/usr/bin/env python3
"""Gate and mutation tests of the published board templates (#86).

The checked-in docs/shared/templates must pass tools/published_templates.py
check(). Each check is then proved on a scratch copy of the repository files
it reads. The targeted check must reject its mutation with its own message,
and the full check must reject it too.
"""

from __future__ import annotations

import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import tempfile

import board_template
import boards
import live_key_fixture
import published_templates as gate

ROOT = gate.ROOT


def rejects(action, expected: str, mutation: str) -> None:
    try:
        action()
    except AssertionError as error:
        if expected not in str(error):
            raise AssertionError(f"{mutation} failed for another reason, {error}") from error
        return
    raise AssertionError(f"{mutation} passed the gate")


def scratch_copy(scratch: Path) -> Path:
    copy = scratch / "repo"
    shutil.copytree(ROOT / gate.TEMPLATES, copy / gate.TEMPLATES)
    for board in boards.boards(ROOT):
        for name in ("board.json", "telemetry_template.h", "telemetry_descriptor_digest.h",
                     "telemetry_dictionary_digest.h", "telemetry_contract.h", "telemetry_fields.inc"):
            target = copy / board.app / name
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy(ROOT / board.app / name, target)
    shutil.copytree(ROOT / live_key_fixture.FIXTURE_DIR, copy / live_key_fixture.FIXTURE_DIR)
    return copy


def third_board(scratch: Path) -> None:
    """A throwaway third board.json is published with no code edit (#141).

    The copy gains firmware/esp-idf-third/app, the first registry board's
    headers under a new slug, alias and dict, plus a LIVE fixture for that
    dict. write() publishes it and check() passes it like the real boards.
    """
    copy = scratch_copy(scratch)
    try:
        model = boards.boards(ROOT)[0]
        app = copy / "firmware/esp-idf-third/app"
        shutil.copytree(copy / model.app, app)
        manifest = json.loads((app / "board.json").read_text())
        manifest["board"] = "example-third-board"
        manifest["tooling"] |= {"alias": "third", "variants": ["app"], "backup_prefixes": ["example-third-board"]}
        (app / "board.json").write_text(json.dumps(manifest, indent=2) + "\n")
        old = gate.source(copy, model)
        new_dict = hashlib.sha256(b"example-third-board").hexdigest()
        template = json.loads(old.raw)
        template["firmware"]["dict"] = new_dict
        raw = json.dumps(template, separators=(",", ":")).encode()
        header = app / "telemetry_template.h"
        header.write_text(header.read_text().replace(board_template.c_literal(old.raw), board_template.c_literal(raw)))
        for name, before, after in (("telemetry_descriptor_digest.h", old.desc, hashlib.sha256(raw).hexdigest()),
                                    ("telemetry_dictionary_digest.h", old.dict, new_dict)):
            (app / name).write_text((app / name).read_text().replace(before, after))
        fixture = live_key_fixture.load(model.fixture)
        fixture["board"] = "third"
        fixture["digests"] = {new_dict: fixture["digests"][old.dict]}
        (copy / live_key_fixture.FIXTURE_DIR / "third.json").write_text(json.dumps(fixture, indent=2) + "\n")
        gate.write(copy)
        found = gate.check(copy)
        slugs = sorted(s.board.slug for s in found)
        gate.require(len(found) == len(boards.boards(ROOT)) + 1 and "example-third-board" in slugs,
                     f"the third board is published, {slugs}")
        gate.require((copy / gate.TEMPLATES / "example-third-board" / f"{new_dict}.json").read_bytes() == raw,
                     "the third board template holds its header bytes")
        # A project left without its board.json fails closed and names the project.
        (app / "board.json").unlink()
        try:
            gate.check(copy)
        except boards.BoardError as error:
            gate.require("firmware/esp-idf-third: no */board.json" in str(error), f"names the project, {error}")
        else:
            raise AssertionError("a board project without board.json passed the gate")
    finally:
        shutil.rmtree(copy)
    print("PASS a third board.json is published without a code edit", flush=True)


def mutate(scratch: Path, edit, check, expected: str, mutation: str) -> None:
    """Apply edit to a fresh copy, then require check to reject it with expected, and the full gate to reject it."""
    copy = scratch_copy(scratch)
    try:
        edit(copy)
        rejects(lambda: check(copy), expected, mutation)
        # The full gate stops at its first failing check, which may be an earlier one.
        rejects(lambda: gate.check(copy), "", mutation + " (full gate)")
    finally:
        shutil.rmtree(copy)
    print(f"PASS rejects {mutation}", flush=True)


def main() -> None:
    found = gate.check()
    for s in found:
        print(f"PASS {gate.TEMPLATES / s.href}, {len(s.raw)} bytes, desc {s.desc}", flush=True)
    attributes = subprocess.run(["git", "check-attr", "text", "--", *(str(gate.TEMPLATES / s.href) for s in found),
                                 str(gate.TEMPLATES / gate.INDEX)], cwd=ROOT, check=True, capture_output=True,
                                text=True).stdout.splitlines()
    gate.require(len(attributes) == len(found) + 1 and all(line.endswith(": text: unset") for line in attributes),
                 "published templates are -text in .gitattributes")
    print("PASS .gitattributes leaves the published bytes untouched", flush=True)

    with tempfile.TemporaryDirectory(prefix="published-templates-") as temporary:
        scratch = Path(temporary)
        # The scratch copy itself passes, so each rejection below comes from its mutation.
        copy = scratch_copy(scratch)
        gate.check(copy)
        shutil.rmtree(copy)
        third_board(scratch)
        directory = gate.TEMPLATES
        for s in found:
            path = directory / s.href

            def published(copy: Path, s=s) -> bytes:
                return (copy / directory / s.href).read_bytes()

            def flip(copy: Path, path=path) -> None:
                raw = bytearray((copy / path).read_bytes())
                raw[len(raw) // 2] ^= 0x01
                (copy / path).write_bytes(bytes(raw))

            mutate(scratch, flip, lambda copy, s=s: gate.check_bytes(published(copy), s),
                   "bytes differ from kTemplateJson", f"{s.board.slug} byte flip")
            mutate(scratch, flip, lambda copy, s=s: gate.check_desc(published(copy), s),
                   "SHA-256 is not desc", f"{s.board.slug} byte flip against desc")
            mutate(scratch, lambda copy, path=path: (copy / path).write_bytes((copy / path).read_bytes() + b"\n"),
                   lambda copy, s=s: gate.check_no_newline(published(copy), s),
                   "ends with a newline", f"{s.board.slug} trailing newline")
            mutate(scratch, lambda copy, path=path: (copy / path).unlink(),
                   lambda copy, s=s: gate.check_present(copy / directory, s),
                   "no published template for the current", f"{s.board.slug} deleted template")

            def stray(copy: Path, s=s) -> None:
                (copy / directory / s.board.slug / f"{'0' * 64}.json").write_bytes(s.raw)

            mutate(scratch, stray, lambda copy: gate.check_no_stale(copy / directory, gate.sources(copy)),
                   "stale files", f"{s.board.slug} stray template")

            def reorder(copy: Path, s=s) -> None:
                # Swap the first two LIVE keys in the template header and both
                # digests, so only the live_keys check can see it.
                template = json.loads(s.raw)
                keys = list(template["firmware"]["live_keys"].items())
                keys[0], keys[1] = keys[1], keys[0]
                template["firmware"]["live_keys"] = dict(keys)
                raw = json.dumps(template, separators=(",", ":")).encode()
                firmware = copy / s.board.app
                header = firmware / "telemetry_template.h"
                text = header.read_text()
                gate.require(board_template.c_literal(s.raw) in text, "header literal found")
                header.write_text(text.replace(board_template.c_literal(s.raw), board_template.c_literal(raw)))
                desc = hashlib.sha256(raw).hexdigest()
                digest = firmware / "telemetry_descriptor_digest.h"
                digest.write_text(digest.read_text().replace(s.desc, desc))
                gate.write(copy)

            def live(copy: Path, s=s) -> None:
                current = gate.source(copy, s.board)
                gate.check_live_keys((copy / directory / current.href).read_bytes(), current,
                                     gate.live_key_order(copy, current), live_key_fixture.load(s.board.fixture))

            mutate(scratch, reorder, live, "live_keys differ from the", f"{s.board.slug} reordered live_keys")

            def wrong_desc(copy: Path, s=s) -> None:
                index = copy / directory / gate.INDEX
                index.write_text(index.read_text().replace(s.desc, s.desc[::-1]))

            mutate(scratch, wrong_desc, lambda copy: gate.check_index(copy / directory, gate.sources(copy)),
                   f"{gate.INDEX} entry for {s.dict}", f"{s.board.slug} wrong desc in {gate.INDEX}")

            def wrong_dict(copy: Path, s=s) -> None:
                index = copy / directory / gate.INDEX
                entries = json.loads(index.read_text())
                entries["by_desc"][s.desc]["dict"] = s.dict[::-1]
                index.write_text(json.dumps(entries, indent=2, sort_keys=True) + "\n")

            mutate(scratch, wrong_dict, lambda copy: gate.check_index(copy / directory, gate.sources(copy)),
                   f"by_desc entry for {s.desc}", f"{s.board.slug} wrong dict in {gate.INDEX} by_desc")

            def write_prunes(copy: Path, s=s) -> None:
                # Retired board folder, stray non JSON file and stray template.
                root = copy / directory
                (root / "retired-board").mkdir()
                (root / "retired-board" / f"{'1' * 64}.json").write_bytes(s.raw)
                (root / s.board.slug / "notes.txt").write_text("stray")
                (root / f"{'2' * 64}.json").write_bytes(s.raw)
                gate.write(copy)
                gate.check(copy)
                gate.require(not (root / "retired-board").exists(), "write removes a retired board folder")
                gate.require((root / gate.README).is_file(), "write keeps README.md")

            copy = scratch_copy(scratch)
            write_prunes(copy)
            shutil.rmtree(copy)
            print(f"PASS --write-template removes stray files and retired folders, {s.board.slug}", flush=True)

        def dropped(copy: Path) -> None:
            index = copy / directory / gate.INDEX
            entries = json.loads(index.read_text())
            entries["templates"].pop(found[0].dict)
            index.write_text(json.dumps(entries))

        mutate(scratch, dropped, lambda copy: gate.check_index(copy / directory, gate.sources(copy)),
               "lists exactly the current dict", f"entry missing from {gate.INDEX}")

        def dropped_desc(copy: Path) -> None:
            index = copy / directory / gate.INDEX
            entries = json.loads(index.read_text())
            entries["by_desc"].pop(found[0].desc)
            index.write_text(json.dumps(entries, indent=2, sort_keys=True) + "\n")

        mutate(scratch, dropped_desc, lambda copy: gate.check_index(copy / directory, gate.sources(copy)),
               "by_desc lists exactly the current desc", f"desc missing from {gate.INDEX} by_desc")

        def no_by_desc(copy: Path) -> None:
            index = copy / directory / gate.INDEX
            entries = json.loads(index.read_text())
            entries.pop("by_desc")
            index.write_text(json.dumps(entries, indent=2, sort_keys=True) + "\n")

        mutate(scratch, no_by_desc, lambda copy: gate.check_index(copy / directory, gate.sources(copy)),
               "holds templates and by_desc", f"{gate.INDEX} without by_desc")

        def minified(copy: Path) -> None:
            index = copy / directory / gate.INDEX
            index.write_text(json.dumps(json.loads(index.read_text()), sort_keys=True))

        mutate(scratch, minified, lambda copy: gate.check_index(copy / directory, gate.sources(copy)),
               "not the deterministic output", f"{gate.INDEX} reformatted")
    print("PASS published templates are byte exact, complete, indexed, and each check rejects its mutation")


if __name__ == "__main__":
    main()
