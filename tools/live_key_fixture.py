"""Check a board's rendered LIVE key map against frozen fixtures keyed by digest.

The phone picks its LIVE profile by dictionary SHA-256, so one digest must
always mean one LIVE key set. A key change without a digest change fails here,
and so does a new digest without a frozen fixture entry. Only current digests
are listed. A retired digest is removed, not kept as history (no backward
compatibility, owner decision 14).

Each build also records which dictionary field every key carries. A LIVE key
never changes meaning (docs/shared/ble-sync-protocol.md#live-key-stability), so
across every digest of every board a key maps to one field.
"""

from __future__ import annotations

import copy
import json
import re
from decimal import Decimal
from pathlib import Path

import boards

FIXTURE_DIR = Path("tools/fixtures/live_keys")
FIXTURES = Path(__file__).resolve().parents[1] / FIXTURE_DIR
PROTOCOL = Path(__file__).resolve().parents[1] / "docs/shared/ble-sync-protocol.md"
LIVE_HEADER = Path(__file__).resolve().parents[1] / "firmware/common/logger/src/os_logger_live.h"


def _max_live_decimals() -> int:
    """oslogger::kMaxLiveDecimals from the shared LIVE header, so host tests and firmware share one bound."""
    match = re.search(r"constexpr std::uint8_t kMaxLiveDecimals = (\d+);", LIVE_HEADER.read_text())
    if match is None:
        raise AssertionError(f"kMaxLiveDecimals not found in {LIVE_HEADER}")
    return int(match.group(1))


# Most digits after the point a Float field may print in LIVE.
MAX_LIVE_DECIMALS = _max_live_decimals()


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def unique_pairs(items: list[tuple[str, object]]) -> dict[str, object]:
    """JSON object hook that refuses a duplicate name instead of keeping the last one."""
    names = [name for name, _ in items]
    require(len(names) == len(set(names)), f"duplicate JSON name in {names}")
    return dict(items)


def fixture_names() -> tuple[str, ...]:
    """The LIVE fixture of every board in the registry (#141), one per board.json."""
    return tuple(board.fixture for board in boards.boards())


def load(board: str, directory: Path = FIXTURES) -> dict:
    fixture = json.loads((directory / f"{board}.json").read_text(), object_pairs_hook=unique_pairs)
    require(fixture["board"] == board, "fixture board")
    return fixture


def build_keys(build: dict) -> list[str]:
    """LIVE keys of one build in emit order, from its ordered key to field map."""
    return list(build["fields"])


def rendered_keys(payload: str) -> list[str]:
    """Keys of one LIVE JSON object in emit order. Duplicates fail."""
    pairs = json.loads(payload, object_pairs_hook=lambda items: items)
    keys = [key for key, _ in pairs]
    require(len(keys) == len(set(keys)), "duplicate LIVE key")
    return keys


def check(board: str, digest: str, firmware: str, payload: str) -> list[str]:
    digests = load(board)["digests"]
    for known, entry in digests.items():
        builds = entry["builds"]
        require(len(known) == 64 and builds, f"fixture entry {known[:8]}")
        require("collision" not in entry, f"{board} digest {known[:8]} carries a retired collision flag")
        sets = {frozenset(build_keys(build)) for build in builds}
        # A digest names one key set.
        require(len(sets) == 1, f"{board} digest {known[:8]} lists different LIVE key sets")
    entry = digests.get(digest)
    require(entry is not None,
            f"{board} digest {digest[:8]} has no frozen LIVE fixture in tools/fixtures/live_keys/{board}.json")
    current = entry["builds"][-1]
    keys = rendered_keys(payload)
    require(current["firmware"] == firmware, f"{board} fixture firmware {current['firmware']} != {firmware}")
    require(keys == build_keys(current),
            f"{board} LIVE keys changed under digest {digest[:8]}: {keys} != {build_keys(current)}")
    require(payload == current["payload"], f"{board} LIVE sample payload changed under digest {digest[:8]}")
    return keys


def parse_pairs(payload: str) -> list[tuple[str, object]]:
    """Ordered key and value pairs of one LIVE object. Floats stay Decimal so 1.00 keeps its digits."""
    return json.loads(payload, object_pairs_hook=lambda items: items, parse_float=Decimal)


def render(pairs: list[tuple[str, object]]) -> str:
    """Rebuild one compact LIVE JSON object from ordered key and value pairs."""
    def value(item: object) -> str:
        return str(item) if isinstance(item, Decimal) else json.dumps(item)
    return "{" + ",".join(f"{json.dumps(key)}:{value(item)}" for key, item in pairs) + "}"


def assert_rejects_drift(board: str, digest: str, firmware: str, payload: str) -> None:
    """The check itself must refuse a renamed, dropped or reordered key and an unknown digest.

    Cases come from the rendered keys, so they stay meaningful after any legitimate rename.
    """
    fixture = load(board)
    pairs = parse_pairs(payload)
    require(len(pairs) >= 3, f"{board} LIVE payload too short for drift cases")
    require(render(pairs) == payload, f"{board} LIVE payload does not round trip through render")
    keys = [key for key, _ in pairs]
    renamed = [(keys[0] + "_renamed", pairs[0][1])] + pairs[1:]
    dropped = pairs[:1] + pairs[2:]
    swapped = pairs[:1] + [pairs[2], pairs[1]] + pairs[3:]
    cases = [(digest, render(case)) for case in (renamed, dropped, swapped)]
    # A digest with no fixture entry, as a retired or unknown digest would be.
    unknown = next(c * 64 for c in "0123456789abcdef" if c * 64 not in fixture["digests"])
    cases.append((unknown, payload))
    for case_digest, case_payload in cases:
        require(case_payload != payload or case_digest != digest, "drift case is not a change")
        try:
            check(board, case_digest, firmware, case_payload)
        except AssertionError:
            continue
        raise AssertionError(f"{board} LIVE fixture check accepted drift under {case_digest[:8]}")


def key_fields(fixtures: dict[str, dict] | None = None) -> dict[str, str]:
    """Each LIVE key maps to one dictionary field across every digest of every board.

    Returns the key to field map.
    """
    fixtures = fixtures if fixtures is not None else {board: load(board) for board in fixture_names()}
    seen: dict[str, dict[str, list[str]]] = {}
    for board, fixture in fixtures.items():
        for digest, entry in fixture["digests"].items():
            for build in entry["builds"]:
                where = f"{board} {digest[:8]} {build['firmware']}"
                for key, field in build["fields"].items():
                    require(isinstance(field, str) and re.fullmatch(r"[a-z][a-z0-9_]*", field) is not None,
                            f"{where} LIVE key {key} names no dictionary field")
                    seen.setdefault(key, {}).setdefault(field, []).append(where)
    for key, by_field in seen.items():
        require(len(by_field) == 1, f"LIVE key {key} maps to more than one dictionary field: {by_field}")
    return {key: next(iter(by_field)) for key, by_field in seen.items()}


def check_fields(board: str, digest: str, dictionary_fields: list[dict]) -> None:
    """The current build's key to field map is the one the compiled dictionary declares."""
    current = load(board)["digests"][digest]["builds"][-1]
    expected = {field["live"]: field["name"] for field in dictionary_fields if field["live"]}
    require(current["fields"] == expected and build_keys(current) == list(expected),
            f"{board} fixture key to field map under digest {digest[:8]} differs from the dictionary")


def documented_keys(text: str) -> set[str]:
    """Keys named in the first column of the LIVE table in ble-sync-protocol.md."""
    start = text.find("### `live` (read, notify)")
    end = text.find("#### LIVE key stability", start)
    require(0 <= start < end, "LIVE section not found in ble-sync-protocol.md")
    keys: set[str] = set()
    for line in text[start:end].splitlines():
        if line.startswith("| `"):
            keys.update(re.findall(r"`([a-z0-9_]+)`", line.split("|")[1]))
    return keys


def check_documented(keys: list[str], text: str) -> None:
    missing = [key for key in keys if key not in documented_keys(text)]
    require(not missing, f"LIVE keys missing from the LIVE table in docs/shared/ble-sync-protocol.md: {missing}")


def check_key_meaning(board: str, digest: str, dictionary_fields: list[dict]) -> None:
    """Run the key to field and documentation checks, and prove each one rejects a mutation."""
    key_fields()
    check_fields(board, digest, dictionary_fields)
    keys = [field["live"] for field in dictionary_fields if field["live"]]
    text = PROTOCOL.read_text()
    check_documented(keys, text)
    fixtures = {name: load(name) for name in fixture_names()}
    # A current key reused for another key's field, on this board and across boards.
    reused = copy.deepcopy(fixtures)
    fields = reused[board]["digests"][digest]["builds"][-1]["fields"]
    fields[keys[0]] = fields[keys[1]]
    others = [name for name in fixtures if name != board]
    require(bool(others), f"{board} cross board LIVE key check needs a second board in the registry")
    other = others[0]
    crossed = copy.deepcopy(fixtures)
    other_build = next(iter(crossed[other]["digests"].values()))["builds"][-1]
    shared = next(key for key in keys if key in other_build["fields"])
    other_build["fields"][shared] = "relabelled_field"
    for case in (reused, crossed):
        try:
            key_fields(case)
        except AssertionError:
            continue
        raise AssertionError(f"{board} LIVE key to field check accepted a reused mapping")
    undocumented = "\n".join(line for line in text.splitlines()
                             if not (line.startswith("| `") and f"`{keys[0]}`" in line.split("|")[1]))
    try:
        check_documented(keys, undocumented)
    except AssertionError:
        return
    raise AssertionError(f"{board} LIVE documentation check accepted a table without {keys[0]}")
