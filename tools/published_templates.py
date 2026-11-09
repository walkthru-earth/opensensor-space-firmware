#!/usr/bin/env python3
"""Published board templates (#86) in docs/shared/templates, written and checked.

Each board publishes exactly the kTemplateJson bytes of its generated
telemetry_template.h as <board>/<dict sha256>.json, with no trailing newline,
so the SHA-256 of the file is the board desc. index.json maps each dict to the
board, the schema name, the template href and desc under templates, and each
desc to the board, dict and href under by_desc. Only the current dict of
each board is published (no backward compatibility, owner decision 14), so a
file for any other dict is stale. The contract tests call write() after
--write-template regenerates the headers, so one command keeps the headers and
the published files in sync. check() is the gate, run by
tools/test_published_templates.py.
"""

from __future__ import annotations

import argparse
from dataclasses import dataclass
import hashlib
import json
from pathlib import Path
import re

import board_template
import boards
import check_board_template
import live_key_fixture

ROOT = Path(__file__).resolve().parents[1]
TEMPLATES = Path("docs/shared/templates")
INDEX = "index.json"
README = "README.md"



@dataclass(frozen=True)
class Source:
    board: boards.Board
    raw: bytes  # kTemplateJson without its NUL
    desc: str
    dict: str
    schema: str

    @property
    def href(self) -> str:
        return f"{self.board.slug}/{self.dict}.json"


def board_for(firmware: Path, root: Path = ROOT) -> boards.Board:
    matches = [board for board in boards.boards(root) if (root / board.app).resolve() == firmware.resolve()]
    require(len(matches) == 1, f"{firmware} is not a published board")
    return matches[0]


def published_path(firmware: Path, dictionary_sha256: str, root: Path = ROOT) -> Path:
    return root / TEMPLATES / board_for(firmware, root).slug / f"{dictionary_sha256}.json"


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def _one_digest(path: Path) -> str:
    found = re.findall(r'"([0-9a-f]{64})"', path.read_text())
    require(len(found) == 1, f"{path.name} holds one SHA-256")
    return found[0]


def source(root: Path, board: boards.Board) -> Source:
    """The board template bytes and digests from the checked-in headers."""
    firmware = root / board.app
    schema = re.search(r'constexpr const char \*kSchemaName = "([^"]+)";', (firmware / "telemetry_contract.h").read_text())
    require(schema is not None, f"{board.slug} kSchemaName not found")
    return Source(board, check_board_template.template_bytes(firmware),
                  _one_digest(firmware / "telemetry_descriptor_digest.h"),
                  _one_digest(firmware / "telemetry_dictionary_digest.h"), schema[1])


def sources(root: Path) -> list[Source]:
    return [source(root, board) for board in boards.boards(root)]


def index_entries(found: list[Source]) -> dict:
    """templates keyed by dict, and by_desc keyed by desc for the app, which resolves by desc first."""
    return {"templates": {s.dict: {"board": s.board.slug, "schema": s.schema, "href": s.href, "desc": s.desc}
                          for s in found},
            "by_desc": {s.desc: {"board": s.board.slug, "dict": s.dict, "href": s.href} for s in found}}


def index_bytes(found: list[Source]) -> bytes:
    index = index_entries(found)
    return (json.dumps(index, indent=2, sort_keys=True) + "\n").encode("ascii")


def expected(found: list[Source]) -> dict[str, bytes]:
    """Every file the templates directory must hold besides README.md, by relative path."""
    files = {s.href: s.raw for s in found}
    files[INDEX] = index_bytes(found)
    return files


def write(root: Path = ROOT) -> None:
    """Publish the current header bytes of every board, rewrite index.json, remove everything else.

    Anything under docs/shared/templates but README.md and the expected files
    is removed, retired board folders included, the same set check_no_stale
    rejects.
    """
    found = sources(root)
    directory = root / TEMPLATES
    files = expected(found)
    keep = set(files) | {README}
    for path in sorted(directory.rglob("*"), reverse=True):
        if path.is_dir():
            if not any(path.iterdir()):
                path.rmdir()
        elif path.relative_to(directory).as_posix() not in keep:
            path.unlink()
    for relative, raw in files.items():
        path = directory / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(raw)
    print(f"WROTE {len(found)} published templates and {INDEX} in {TEMPLATES}", flush=True)


def check_bytes(published: bytes, s: Source) -> None:
    require(published == s.raw, f"{s.href} bytes differ from kTemplateJson")


def check_desc(published: bytes, s: Source) -> None:
    require(hashlib.sha256(published).hexdigest() == s.desc, f"{s.href} SHA-256 is not desc {s.desc}")


def check_no_newline(published: bytes, s: Source) -> None:
    require(not published.endswith((b"\n", b"\r")), f"{s.href} ends with a newline")


def check_present(directory: Path, s: Source) -> bytes:
    path = directory / s.href
    require(path.is_file(), f"no published template for the current {s.board.slug} dict {s.dict}")
    return path.read_bytes()


def check_no_stale(directory: Path, found: list[Source]) -> None:
    allowed = set(expected(found)) | {README}
    present = {p.relative_to(directory).as_posix() for p in directory.rglob("*") if p.is_file()}
    stale = sorted(present - allowed)
    require(not stale, f"stale files in {TEMPLATES}, {stale}")


def check_identity(published: bytes, s: Source) -> None:
    template = json.loads(published, object_pairs_hook=live_key_fixture.unique_pairs)
    require(template["firmware"]["dict"] == s.dict, f"{s.href} firmware.dict is not the file name dict")
    require(template["id"] == template["firmware"]["schema"] == s.schema, f"{s.href} id is not kSchemaName")


def check_schema(published: bytes, s: Source) -> None:
    """The pinned stac-opensensor v0.1.0 template schema, as in the contract tests (#85)."""
    errors = list(board_template.validator().iter_errors(json.loads(published)))
    require(not errors, f"{s.href} template schema, {errors[0].message if errors else ''}")


def live_key_order(root: Path, s: Source) -> list[tuple[str, str]]:
    """(field, LIVE key) pairs in field order from the board field table."""
    inc = (root / s.board.app / "telemetry_fields.inc").read_text()
    rows = re.findall(r'^FIELD\((\w+), .*, "([a-z0-9_]*)", [0-9]\)$', inc, re.M)
    require(rows, f"{s.board.slug} field table has no FIELD rows")
    return [(name, key) for name, key in rows if key]


def check_live_keys(published: bytes, s: Source, table: list[tuple[str, str]], fixture: dict) -> None:
    keys = list(json.loads(published)["firmware"]["live_keys"].items())
    require(keys == table, f"{s.href} live_keys differ from the {s.board.slug} field table order")
    digest = fixture["digests"].get(s.dict)
    require(digest is not None, f"no frozen LIVE fixture for the current {s.board.slug} dict {s.dict}")
    for build in digest["builds"]:
        require(keys == [(field, key) for key, field in build["fields"].items()],
                f"{s.href} live_keys differ from the frozen LIVE fixture of {build['firmware']}")


def check_index(directory: Path, found: list[Source]) -> None:
    path = directory / INDEX
    require(path.is_file(), f"no {INDEX}")
    raw = path.read_bytes()
    index = json.loads(raw, object_pairs_hook=live_key_fixture.unique_pairs)
    require(sorted(index) == ["by_desc", "templates"], f"{INDEX} holds templates and by_desc")
    entries = index["templates"]
    require(sorted(entries) == sorted(s.dict for s in found), f"{INDEX} lists exactly the current dict of each board")
    for s in found:
        entry = entries[s.dict]
        require(entry == {"board": s.board.slug, "schema": s.schema, "href": s.href, "desc": s.desc},
                f"{INDEX} entry for {s.dict} differs from the {s.board.slug} headers")
        target = directory / entry["href"]
        require(target.is_file() and hashlib.sha256(target.read_bytes()).hexdigest() == entry["desc"],
                f"{INDEX} desc for {s.dict} is not the SHA-256 of its href")
    by_desc = index["by_desc"]
    require(sorted(by_desc) == sorted(s.desc for s in found), f"{INDEX} by_desc lists exactly the current desc of each board")
    for s in found:
        require(by_desc[s.desc] == {"board": s.board.slug, "dict": s.dict, "href": s.href},
                f"{INDEX} by_desc entry for {s.desc} differs from the {s.board.slug} headers")
    require(raw == index_bytes(found), f"{INDEX} is not the deterministic output, run --write-template")


def check(root: Path = ROOT) -> list[Source]:
    directory = root / TEMPLATES
    found = sources(root)
    require(len({s.dict for s in found}) == len(found), "one dict per board")
    for s in found:
        published = check_present(directory, s)
        check_bytes(published, s)
        check_desc(published, s)
        check_no_newline(published, s)
        check_identity(published, s)
        check_schema(published, s)
        check_live_keys(published, s, live_key_order(root, s),
                        live_key_fixture.load(s.board.fixture, root / live_key_fixture.FIXTURE_DIR))
    check_no_stale(directory, found)
    check_index(directory, found)
    return found


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--write", action="store_true", help="publish from the current headers instead of checking")
    args = parser.parse_args()
    if args.write:
        write()
        return
    for s in check():
        print(f"PASS {TEMPLATES / s.href}, {len(s.raw)} bytes, desc {s.desc}", flush=True)


if __name__ == "__main__":
    main()
