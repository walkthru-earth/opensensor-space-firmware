#!/usr/bin/env python3
"""Pinned dictionary and template URIs (#89), checked against git and written.

Each board's telemetry_contract.h holds two raw GitHub URLs at a full 40 hex
commit.

- kDictionaryUri names the board telemetry_fields.inc. The footer carries it
  and the template describedby link is the same string. Its bytes hash to dict.
- kTemplateHref names docs/shared/templates/<board>/<dict>.json. The footer
  carries it from #88 and its bytes hash to desc. It sits outside the template, so
  pinning it never moves desc.

check() proves with git that each URL has exactly that form, that the commit is
an ancestor of HEAD, so a squash or a branch cannot lose it, and that the bytes
at the commit hash to the digest. It needs full history (CI checks out with
fetch-depth 0) and fails on a shallow clone, it never skips.

write() (pixi run pin-uris) pins each URL to the commit that last changed its
path, after checking those bytes against the digest. It refuses while any
file it reads or writes has uncommitted changes, the pinned files, the digest
headers, telemetry_template.h and telemetry_contract.h. A commit URL cannot name
the commit it is in, so a PR that moves dict or desc lands in two steps. The
content commit holds the new bytes, then pin-uris and a pin commit point the
URLs at it. That PR lands with a merge commit, so the content commit stays on
main. Every other PR squashes.
"""

from __future__ import annotations

import argparse
from dataclasses import dataclass
import hashlib
import json
from pathlib import Path
import re
import subprocess

import boards
import published_templates

ROOT = published_templates.ROOT
OWNER, NAME = "walkthru-earth", "opensensor-space-firmware"
REPO = f"{OWNER}/{NAME}"
RAW = f"https://raw.githubusercontent.com/{REPO}/"
# RAW is longer than one clang-format line, so literal() writes it in two parts.
RAW_PARTS = (f"https://raw.githubusercontent.com/{OWNER}/", f"{NAME}/")
PINNED = re.compile(re.escape(RAW) + r"([0-9a-f]{40})/(.+)")
RULE = "PRs that move dict or desc land with a merge commit, everything else squashes"
FIX = f"Run pixi run pin-uris after the content commit and commit the result. {RULE}."
DICTIONARY = "kDictionaryUri"
TEMPLATE = "kTemplateHref"


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


@dataclass(frozen=True)
class Pin:
    board: str
    name: str  # kDictionaryUri or kTemplateHref
    path: str  # repository path the URL must name
    digest: str  # dict or desc, the SHA-256 of the bytes at that path


def git(root: Path, *args: str, check: bool = True) -> subprocess.CompletedProcess[bytes]:
    return subprocess.run(["git", "-C", str(root), *args], capture_output=True, check=check)


def _literal(name: str) -> re.Pattern[str]:
    return re.compile(rf'constexpr const char \*{name} =\s*((?:"[^"\n]*"\s*)+);')


def read_constant(contract: Path, name: str) -> str:
    found = _literal(name).findall(contract.read_text())
    require(len(found) == 1, f"{contract.name} declares {name} once")
    return "".join(re.findall(r'"([^"\n]*)"', found[0]))


def pins(root: Path) -> list[Pin]:
    result = []
    for s in published_templates.sources(root):
        result.append(Pin(s.board.slug, DICTIONARY, f"{s.board.app}/telemetry_fields.inc", s.dict))
        result.append(Pin(s.board.slug, TEMPLATE, (published_templates.TEMPLATES / s.href).as_posix(), s.desc))
    return result


def contract_path(root: Path, board: str) -> Path:
    matches = [b for b in boards.boards(root) if b.slug == board]
    require(len(matches) == 1, f"{board} is not a published board")
    return root / matches[0].app / "telemetry_contract.h"


def check_history(root: Path) -> None:
    shallow = git(root, "rev-parse", "--is-shallow-repository").stdout.decode().strip()
    require(shallow == "false", "shallow clone, the pinned URI gate needs full history (fetch-depth 0)")


def check_form(pin: Pin, uri: str) -> str:
    """The pinned commit, or a failure naming the moving ref."""
    match = PINNED.fullmatch(uri)
    require(match is not None,
            f"{pin.board} {pin.name} is not a raw URL at a full 40 hex commit, {uri}. A main, branch or tag URL "
            f"or a short sha moves. {FIX}")
    require(match[2] == pin.path, f"{pin.board} {pin.name} names {match[2]}, not {pin.path}")
    return match[1]


def check_commit(root: Path, pin: Pin, commit: str) -> None:
    require(git(root, "cat-file", "-e", f"{commit}^{{commit}}", check=False).returncode == 0,
            f"{pin.board} {pin.name} pins {commit}, which is not in this clone. {FIX}")
    require(git(root, "merge-base", "--is-ancestor", commit, "HEAD", check=False).returncode == 0,
            f"{pin.board} {pin.name} pins {commit}, which is not an ancestor of HEAD. A squash or rebase merge "
            f"drops the content commit from main. {FIX}")


def check_bytes(root: Path, pin: Pin, commit: str) -> None:
    shown = git(root, "show", f"{commit}:{pin.path}", check=False)
    require(shown.returncode == 0, f"{pin.board} {pin.name} pins {commit}, which has no {pin.path}. {FIX}")
    digest = hashlib.sha256(shown.stdout).hexdigest()
    require(digest == pin.digest,
            f"{pin.board} {pin.name} pins {commit} whose bytes hash to {digest}, not {pin.digest}. {FIX}")


def check_pin(root: Path, pin: Pin, uri: str) -> None:
    commit = check_form(pin, uri)
    check_commit(root, pin, commit)
    check_bytes(root, pin, commit)


def check_describedby(template: bytes, board: str, dictionary_uri: str) -> None:
    links = [link for link in json.loads(template)["links"] if link["rel"] == "describedby"]
    require(len(links) == 1 and links[0]["href"] == dictionary_uri,
            f"{board} template describedby href is not {DICTIONARY}, run --write-template")


def check(root: Path = ROOT) -> list[tuple[Pin, str]]:
    check_history(root)
    found = []
    for pin in pins(root):
        uri = read_constant(contract_path(root, pin.board), pin.name)
        check_pin(root, pin, uri)
        found.append((pin, uri))
    for s in published_templates.sources(root):
        check_describedby(s.raw, s.board.slug, read_constant(root / s.board.app / "telemetry_contract.h",
                                                             DICTIONARY))
    return found


def literal(pin: Pin, commit: str) -> str:
    """The clang-format stable declaration pin-uris writes."""
    head, tail = pin.path.rsplit("/", 1)
    path = [f"{head}/", tail] if pin.name == TEMPLATE else [pin.path]
    parts = [*RAW_PARTS, f"{commit}/", *path]
    return f"constexpr const char *{pin.name} =\n" + "\n".join(f'    "{p}"' for p in parts) + ";"


def last_commit(root: Path, path: str) -> str:
    commit = git(root, "log", "-1", "--format=%H", "--", path).stdout.decode().strip()
    require(re.fullmatch(r"[0-9a-f]{40}", commit) is not None, f"{path} is not committed")
    return commit


def write(root: Path = ROOT) -> list[str]:
    """Pin each URL to the commit that last changed its path. Return the changed pins."""
    check_history(root)
    # The pins and headers below come from the registry, so a board.json
    # added, edited or removed on disk would steer them. Refuse that first.
    manifests = git(root, "status", "--porcelain", "--untracked-files=all", "--", boards.BOARD_GLOB).stdout.decode()
    require(not manifests.strip(), f"pin-uris needs every board.json committed, commit it first, {manifests.strip()}")
    todo = pins(root)
    # Every file pin-uris reads or writes must be committed. The pinned bytes
    # and their digests, so the commit that last changed a path holds what is
    # on disk. The template header the digests describe, and the contract this
    # rewrites, so a pin never lands on top of an uncommitted edit.
    headers = [f"{b.app}/{name}" for b in boards.boards(root)
               for name in ("telemetry_dictionary_digest.h", "telemetry_descriptor_digest.h",
                            "telemetry_template.h", "telemetry_contract.h")]
    dirty = git(root, "status", "--porcelain", "--", *(p.path for p in todo), *headers).stdout.decode().strip()
    require(not dirty, f"pin-uris needs the pinned files committed, commit the content first, {dirty}")
    changed: list[str] = []
    # The template embeds kDictionaryUri, so the dictionary pins come first.
    for name in (DICTIONARY, TEMPLATE):
        for pin in (p for p in todo if p.name == name):
            commit = last_commit(root, pin.path)
            try:
                check_bytes(root, pin, commit)
            except AssertionError as error:
                raise AssertionError(f"{error}. Commit the regenerated files, then run pin-uris again") from error
            contract = contract_path(root, pin.board)
            text = contract.read_text()
            declaration = _literal(pin.name).search(text)
            require(declaration is not None, f"{contract.name} declares {pin.name}")
            if read_constant(contract, pin.name) == f"{RAW}{commit}/{pin.path}":
                continue
            contract.write_text(text[:declaration.start()] + literal(pin, commit) + text[declaration.end():])
            changed.append(f"{pin.board} {pin.name} -> {commit}")
        if name == DICTIONARY and changed:
            # A new dict also renames the published template. The contract
            # static_asserts that kTemplateHref names <board>/<dict>.json, so
            # point it at the new name under its old commit until the next run
            # pins the content commit. Without this --write-template cannot
            # build. pinned-uris-test fails on this provisional pin by design.
            for pin in (p for p in todo if p.name == TEMPLATE):
                contract = contract_path(root, pin.board)
                current = PINNED.fullmatch(read_constant(contract, TEMPLATE))
                if current is None or current[2] == pin.path:
                    continue
                text = contract.read_text()
                declaration = _literal(TEMPLATE).search(text)
                contract.write_text(text[:declaration.start()] + literal(pin, current[1])
                                    + text[declaration.end():])
                changed.append(f"{pin.board} {TEMPLATE} provisional -> {pin.path}")
            print("kDictionaryUri moved. The template embeds it, so run the board contract test with "
                  "--write-template, commit, then run pixi run pin-uris again", flush=True)
            return changed
    return changed


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--write", action="store_true", help="pin the URLs to the commits that hold their bytes")
    args = parser.parse_args()
    if args.write:
        changed = write()
        for line in changed:
            print(f"PINNED {line}", flush=True)
        if not changed:
            print("pins already current", flush=True)
        return
    for pin, uri in check():
        print(f"PASS {pin.board} {pin.name} {uri}", flush=True)


if __name__ == "__main__":
    main()
