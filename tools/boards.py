#!/usr/bin/env python3
"""Board registry (#141). Every board is one board.json, nothing else.

Each ESP-IDF project under firmware/esp-idf-*/ holds exactly one telemetry
app with a board.json. The manifest carries the footer slug in `board` and
the host tool facts in its `tooling` object, validated by the plugin board
schema (docs/shared/plugins/v0.1.0/board.json). Host tools and shell scripts
read the board list from here and never name a board themselves.

The registry fails closed. A project without a board.json, a second board.json
in one project, malformed JSON, a missing or wrong `tooling` field and a name
that two boards share each raise BoardError naming the file. A name nobody
declares is an error too. There is no default board.

Shell scripts resolve a board name and an optional variant with

    eval "$(python tools/boards.py resolve -- <name> [variant])"

which sets alias, slug, project, app, variant, telemetry, backup_prefix and
backup_prefixes, or prints the reason and exits 2. The `--` keeps a name such
as -h a board name, so argparse never prints help into the eval. A name is the short alias
(cores3), the footer slug (m5stack-cores3) or the first backup prefix
(waveshare-sim7670g-v2).
"""

from __future__ import annotations

import argparse
from dataclasses import dataclass
import json
from pathlib import Path
import re
import shlex
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
PROJECT_GLOB = "firmware/esp-idf-*"
BOARD_GLOB = f"{PROJECT_GLOB}/*/board.json"
# The one flash capacity the backup, restore and flash helpers support. A
# board with another capacity extends the schema enum and the helpers first.
FLASH_BYTES = 16 * 1024 * 1024

SLUG = re.compile(r"^[a-z0-9]+(-[a-z0-9]+)*$")
# No dash, CI derives the stack budget image key as ${leg name%%-*}/variant.
ALIAS = re.compile(r"^[a-z][a-z0-9]{0,31}$")
VARIANT = re.compile(r"^[a-z][a-z0-9_]{0,31}$")
COMPONENT = re.compile(r"^[A-Za-z0-9_.-]{1,64}$")
SDKCONFIG = re.compile(r"^(CONFIG_[A-Z0-9_]+=\S.*|# CONFIG_[A-Z0-9_]+ is not set)$")
PSRAM_MODES = ("QUAD", "OCT")
TOOLING_KEYS = frozenset({"alias", "variants", "backup_prefixes", "flash_bytes", "psram_mode", "sdkconfig",
                          "components_required", "components_forbidden"})


class BoardError(ValueError):
    """The registry or a board name is wrong. The message names the file or the name."""


@dataclass(frozen=True)
class Board:
    slug: str  # footer thing.board, docs/boards/<slug>, docs/shared/templates/<slug>
    alias: str  # pixi tasks, build/<variant> targets, CI legs, stack budget images, LIVE fixture
    project: str  # ESP-IDF project, relative posix path such as firmware/esp-idf-cores3
    variants: tuple[str, ...]  # app directories the project builds, the telemetry app first
    backup_prefixes: tuple[str, ...]  # the first names new backups, every one restores
    flash_bytes: int
    psram_mode: str  # CONFIG_SPIRAM_MODE_<psram_mode> in the telemetry app
    sdkconfig: tuple[str, ...]  # lines every variant's sdkconfig holds
    components_required: tuple[str, ...]  # build components every variant includes
    components_forbidden: tuple[str, ...]  # case insensitive substrings no component name may hold
    manifest: str  # relative posix path of the board.json

    @property
    def telemetry_variant(self) -> str:
        return self.variants[0]

    @property
    def app(self) -> str:
        """The telemetry app directory, which holds board.json and the generated headers."""
        return f"{self.project}/{self.telemetry_variant}"

    @property
    def fixture(self) -> str:
        """tools/fixtures/live_keys/<fixture>.json"""
        return self.alias

    @property
    def backup_prefix(self) -> str:
        return self.backup_prefixes[0]

    @property
    def names(self) -> tuple[str, ...]:
        """Every name that resolves to this board."""
        return tuple(dict.fromkeys((self.alias, self.slug, self.backup_prefix)))


def _strings(value: object, pattern: re.Pattern[str], where: str, *, empty: bool) -> tuple[str, ...]:
    if not isinstance(value, list) or (not empty and not value):
        raise BoardError(f"{where} must be a {'' if empty else 'non empty '}list of strings")
    if len(set(map(str, value))) != len(value):
        raise BoardError(f"{where} repeats an entry")
    for item in value:
        if not isinstance(item, str) or pattern.fullmatch(item) is None:
            raise BoardError(f"{where} entry {item!r} does not match {pattern.pattern}")
    return tuple(value)


def load(root: Path, path: Path) -> Board:
    """One board from its board.json. Raise BoardError naming the file on any defect."""
    relative = path.relative_to(root).as_posix()
    try:
        manifest = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeDecodeError, json.JSONDecodeError) as error:
        raise BoardError(f"{relative}: not readable JSON, {error}") from error
    if not isinstance(manifest, dict):
        raise BoardError(f"{relative}: not a JSON object")
    slug = manifest.get("board")
    if not isinstance(slug, str) or SLUG.fullmatch(slug) is None:
        raise BoardError(f"{relative}: board {slug!r} is not a slug matching {SLUG.pattern}")
    tooling = manifest.get("tooling")
    if not isinstance(tooling, dict):
        raise BoardError(f"{relative}: no tooling object, see docs/shared/board-porting.md")
    unknown = sorted(set(tooling) - TOOLING_KEYS)
    missing = sorted(TOOLING_KEYS - set(tooling))
    if unknown or missing:
        raise BoardError(f"{relative}: tooling keys, unknown {unknown}, missing {missing}")
    where = f"{relative}: tooling"
    alias = tooling["alias"]
    if not isinstance(alias, str) or ALIAS.fullmatch(alias) is None:
        raise BoardError(f"{where}.alias {alias!r} does not match {ALIAS.pattern}")
    variants = _strings(tooling["variants"], VARIANT, f"{where}.variants", empty=False)
    if variants[0] != path.parent.name:
        raise BoardError(f"{where}.variants starts with {variants[0]!r}, not the board.json directory "
                         f"{path.parent.name!r}")
    prefixes = _strings(tooling["backup_prefixes"], SLUG, f"{where}.backup_prefixes", empty=False)
    flash = tooling["flash_bytes"]
    if type(flash) is not int or flash != FLASH_BYTES:
        raise BoardError(f"{where}.flash_bytes {flash!r} is not {FLASH_BYTES}, the only capacity the helpers support")
    psram = tooling["psram_mode"]
    if psram not in PSRAM_MODES:
        raise BoardError(f"{where}.psram_mode {psram!r} is not one of {', '.join(PSRAM_MODES)}")
    return Board(
        slug=slug,
        alias=alias,
        project=path.parent.parent.relative_to(root).as_posix(),
        variants=variants,
        backup_prefixes=prefixes,
        flash_bytes=flash,
        psram_mode=psram,
        sdkconfig=_strings(tooling["sdkconfig"], SDKCONFIG, f"{where}.sdkconfig", empty=True),
        components_required=_strings(tooling["components_required"], COMPONENT,
                                     f"{where}.components_required", empty=True),
        components_forbidden=_strings(tooling["components_forbidden"], COMPONENT,
                                      f"{where}.components_forbidden", empty=True),
        manifest=relative,
    )


def boards(root: Path = ROOT) -> tuple[Board, ...]:
    """Every board under root, sorted by slug. Raise BoardError on any registry defect."""
    root = root.resolve()
    projects = sorted(p for p in root.glob(PROJECT_GLOB) if p.is_dir())
    if not projects:
        raise BoardError(f"no board project matches {PROJECT_GLOB} under {root}")
    found: list[Board] = []
    for project in projects:
        manifests = sorted(project.glob("*/board.json"))
        relative = project.relative_to(root).as_posix()
        if not manifests:
            raise BoardError(f"{relative}: no */board.json, every board project has one telemetry app with a board.json")
        if len(manifests) > 1:
            listed = ", ".join(m.relative_to(root).as_posix() for m in manifests)
            raise BoardError(f"{relative}: more than one board.json, {listed}")
        found.append(load(root, manifests[0]))
    owner: dict[str, Board] = {}
    for board in found:
        for name in {*board.names, *board.backup_prefixes}:
            other = owner.get(name)
            if other is not None:
                raise BoardError(f"{board.manifest}: name {name!r} is already used by {other.manifest}")
            owner[name] = board
    # Restore and check_idf_target match <prefix>-flash-*.bin. Two backup
    # names could match two prefixes only when one prefix starts with the
    # other plus a dash, so that is refused and every name has one owner.
    prefixes = [(prefix, board) for board in found for prefix in board.backup_prefixes]
    for prefix, board in prefixes:
        for longer, other in prefixes:
            if longer.startswith(f"{prefix}-"):
                raise BoardError(f"{other.manifest}: backup prefix {longer!r} starts with {prefix}- of "
                                 f"{board.manifest}, one backup name could match both")
    return tuple(sorted(found, key=lambda b: b.slug))


def find(name: str, root: Path = ROOT) -> Board:
    """The board one name resolves to. A name nobody declares is an error, never a default."""
    every = boards(root)
    for board in every:
        if name in board.names:
            return board
    known = ", ".join(n for b in every for n in b.names)
    raise BoardError(f"unknown board {name!r}, expected one of {known}")


def resolve(name: str, variant: str | None = None, root: Path = ROOT) -> tuple[Board, str]:
    """The board and the variant to build. No variant means the board's telemetry app."""
    board = find(name, root)
    chosen = variant or board.telemetry_variant
    if chosen not in board.variants:
        raise BoardError(f"invalid board variant {board.alias}/{chosen}, expected one of {', '.join(board.variants)}")
    if not (root / board.project / chosen).is_dir():
        raise BoardError(f"{board.manifest}: variant directory {board.project}/{chosen} is missing")
    return board, chosen


def tracked_sources(board: Board, root: Path = ROOT) -> tuple[list[str], list[str]]:
    """Tracked C++ sources of a board project, (telemetry app, every other file)."""
    try:
        listed = subprocess.run(["git", "-C", str(root), "ls-files", "-z", "--", f"{board.project}/*.cpp"],
                                capture_output=True, check=True).stdout.decode()
    except (OSError, subprocess.CalledProcessError) as error:
        raise BoardError(f"git ls-files failed for {board.project}, {error}") from error
    names = sorted(n for n in listed.split("\0") if n)
    telemetry = [n for n in names if n.startswith(f"{board.app}/")]
    if not telemetry:
        raise BoardError(f"{board.manifest}: no tracked .cpp in {board.app}")
    return telemetry, [n for n in names if n not in telemetry]


def shell_assignments(board: Board, variant: str) -> str:
    values = {
        "alias": board.alias,
        "slug": board.slug,
        "project": board.project,
        "app": board.app,
        "variant": variant,
        "telemetry": "1" if variant == board.telemetry_variant else "0",
        "backup_prefix": board.backup_prefix,
        "backup_prefixes": " ".join(board.backup_prefixes),
    }
    return "".join(f"{key}={shlex.quote(value)}\n" for key, value in values.items())


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    commands = parser.add_subparsers(dest="command", required=True)
    resolve_parser = commands.add_parser("resolve", help="print shell assignments for one board and variant")
    resolve_parser.add_argument("name")
    resolve_parser.add_argument("variant", nargs="?", default="")
    commands.add_parser("list", help="print every board, slug alias project variants")
    sources_parser = commands.add_parser("sources", help="print tracked C++ sources of every board, one per line")
    sources_parser.add_argument("kind", choices=("telemetry", "other"))
    args = parser.parse_args(argv)
    try:
        if args.command == "resolve":
            board, variant = resolve(args.name, args.variant or None)
            sys.stdout.write(shell_assignments(board, variant))
        elif args.command == "list":
            for board in boards():
                print(board.slug, board.alias, board.project, ",".join(board.variants))
        else:
            for board in boards():
                telemetry, other = tracked_sources(board)
                for name in telemetry if args.kind == "telemetry" else other:
                    print(name)
    except BoardError as error:
        print(f"board registry: {error}", file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main())
