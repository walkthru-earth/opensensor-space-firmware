#!/usr/bin/env python3
"""Plugin contract gate (#17), docs/shared/plugins/README.md.

Module and board manifests describe each board's columns as ordered module
segments. This gate validates every manifest against the plugin v0.1.0
schemas, which resolve offline against the vendored stac-opensensor v0.1.0
schemas. It then expands each board's segments and requires (Option A) that

- the rendered FIELD rows equal the board telemetry_fields.inc FIELD rows in
  order, byte for byte,
- the rendered SEM rows equal the telemetry_semantics.inc SEM rows,
- the class names and nodata flags equal the telemetry_classes.inc rows,
- each rendered template column, title, keywords, instruments, LIVE keys and
  unit labels equal the published board template at today's dict,
- the board shipped.lock matches the file and the manifests, is append only
  against its own history and against the base branch lock, and names only
  releases that docs/shared/mobile-migration.md names.

The board .inc files stay frozen. The gate never rewrites them.
--write-board writes them once for a new board only (section 11 step 7). It
renders telemetry_fields.inc, telemetry_semantics.inc, telemetry_classes.inc
and a first shipped.lock from the new board.json, and refuses when any of
the four already exists. From then on they are frozen like any other.

Each shared and board local module manifest also has a checked-in C++ header
beside it, <id>_module.h, with the module's Column enum, kColumns, status
constants and InitError (README section 10). The gate requires every header
to equal what module_header() renders, byte for byte, and no header without
a manifest. --write-headers rewrites them from the manifests, and removes an
orphan only when it starts with the generated preamble.
"""

from __future__ import annotations

import argparse
from dataclasses import dataclass, field
import hashlib
import json
import math
import os
from pathlib import Path
import re
import subprocess
import sys
from typing import Callable

from jsonschema import Draft7Validator, FormatChecker
from referencing import Registry, Resource

ROOT = Path(__file__).resolve().parents[1]
PLUGINS = Path("docs/shared/plugins")
SCHEMAS = PLUGINS / "v0.1.0"
EXAMPLES = PLUGINS / "examples"
STAC = Path("tools/stac_opensensor/v0.1.0")
SCALES = Path("tools/stac_opensensor/registry/scales.json")
TEMPLATES = Path("docs/shared/templates")
MIGRATION = Path("docs/shared/mobile-migration.md")
WRITER = Path("firmware/common/src/parquet_writer.h")
SYSTEM_MODULE = Path("firmware/common/logger/system.module.json")
COMMON_MODULES = Path("firmware/common/sensors/modules")
BOARD_GLOB = "firmware/esp-idf-*/*/board.json"
FIELDS_GLOB = "firmware/esp-idf-*/*/telemetry_fields.inc"
LOCK = "shipped.lock"
SENSOR_GLUE = Path("firmware/common/logger/src/os_sensor_module.h")

# Template column types, as tools/board_template.py maps them.
PHYSICAL_TYPES = {"Int32": "int32", "Int64": "int64", "Float": "float"}
LOGICAL_TYPES = {"uint8_code": "uint8", "wkb_point_crs84": "geometry"}
CLASS_ROW = re.compile(r'CLASS\(\s*(\w+)\s*,\s*([^,]+?)\s*,\s*"([^"]*)"\s*,\s*(true|false)\s*\)')

BaseLock = Callable[[Path], "dict | None"]


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def load_json(path: Path) -> dict:
    def no_duplicates(pairs: list) -> dict:
        keys = [key for key, _ in pairs]
        require(len(keys) == len(set(keys)), f"{path}: duplicate key")
        return dict(pairs)

    try:
        return json.loads(path.read_text(encoding="utf-8"), object_pairs_hook=no_duplicates)
    except json.JSONDecodeError as error:
        raise AssertionError(f"{path}: not JSON, {error}") from error


def validators(root: Path) -> dict[str, Draft7Validator]:
    resources = []
    for directory in (root / STAC, root / SCHEMAS):
        for path in sorted(directory.glob("*.json")):
            contents = json.loads(path.read_text())
            resources.append((contents["$id"].rstrip("#"), Resource.from_contents(contents)))
    registry = Registry().with_resources(resources)
    require("uri" in FormatChecker().checkers, "format uri is enforced (rfc3986-validator)")
    out = {}
    for kind in ("module", "board", "lock"):
        schema = json.loads((root / SCHEMAS / f"{kind}.json").read_text())
        Draft7Validator.check_schema(schema)
        out[kind] = Draft7Validator(schema, registry=registry, format_checker=FormatChecker())
    return out


def validate(validator: Draft7Validator, document: dict, where: str) -> None:
    errors = sorted(validator.iter_errors(document), key=lambda e: list(e.absolute_path))
    require(not errors, f"{where} schema: " + "; ".join(
        f"{'/'.join(map(str, e.absolute_path)) or '(root)'}: {e.message}" for e in errors[:5]))


# Rendering. Every value must round trip into the exact .inc spelling, so a
# value the renderer cannot spell fails here and never reaches a board file.


def number(value: float | int, where: str) -> str:
    require(isinstance(value, (int, float)) and not isinstance(value, bool) and math.isfinite(value),
            f"{where}: not a finite number")
    if isinstance(value, float) and value.is_integer():
        value = int(value)
    text = str(value)
    require(float(text) == value, f"{where}: {value!r} does not round trip")
    return text


def literal(text: str, where: str) -> str:
    require('"' not in text and "\\" not in text and "\n" not in text, f"{where}: {text!r} needs escaping")
    return f'"{text}"'


@dataclass(frozen=True)
class Column:
    """One expanded board column, a manifest column with its module defaults resolved."""

    module: str
    segment: str
    spec: dict  # the manifest column
    procedure: str
    vendor: str
    model: str

    @property
    def name(self) -> str:
        return self.spec["name"]

    @property
    def firmware(self) -> dict:
        return self.spec["firmware"]


def field_row(column: Column) -> str:
    fw = column.firmware
    where = column.name
    return (f"FIELD({column.name}, {fw['type']}, {column.procedure}, {literal(fw['unit'], where)}, "
            f"{fw['validity']}, {literal(fw['live_key'], where)}, {fw['decimals']})")


def gate_text(column: Column) -> str:
    gate = column.spec.get("opensensor:valid_when")
    if gate is None:
        return ""
    if gate.get("finite") is True:
        # SEM spells a finite gate only on the column itself.
        require(gate["column"] == column.name, f"{column.name}: a finite gate must name the column itself")
        return "finite"
    value = gate["equals"]
    require(not isinstance(value, (str, bool)), f"{column.name}: SEM gates compare numbers only")
    return f"{gate['column']} == {number(value, column.name + ' valid_when')}"


def sem_row(column: Column) -> str:
    spec = column.spec
    where = column.name
    low, high = ("", "")
    if "opensensor:plausible_range" in spec:
        low, high = (number(v, where + " plausible_range") for v in spec["opensensor:plausible_range"])
    parts = [
        column.name,
        literal(spec.get("unit", ""), where),
        spec["roles"][0],
        literal(spec.get("cf:standard_name", ""), where),
        spec["opensensor:aggregate"],
        low,
        high,
        number(spec.get("opensensor:settle_s", 0), where + " settle_s"),
        literal(gate_text(column), where),
        literal(spec["opensensor:property"], where),
        literal(column.vendor, where),
        literal(column.model, where),
    ]
    return "SEM(" + ", ".join(parts) + ")"


def classes(column: Column, subsets: dict) -> list[dict]:
    listed = column.spec.get("classification:classes", [])
    keep = subsets.get(column.name)
    if keep is None:
        return listed
    names = [c["name"] for c in listed]
    require(set(keep) <= set(names), f"{column.name}: class_subsets names a class the module does not list")
    return [c for c in listed if c["name"] in keep]


# Display hints (#184). They come from the module manifests and never from a
# board table row, so tools/board_template.h does not build them. hinted()
# adds them to the built template, and check_board() reads them back.
HINT_HEAD = ("title", "opensensor:title_i18n", "description", "opensensor:description_i18n")
HINT_TAIL = ("opensensor:group", "opensensor:scale", "classification:bitfields")
CLASS_HINTS = ("title", "description", "color_hint")


def without_hints(column: dict) -> dict:
    """A template column as tools/board_template.h builds it, with no display hint."""
    out = {k: v for k, v in column.items() if k not in HINT_HEAD + HINT_TAIL + ("opensensor:decimals",)}
    if "classification:classes" in out:
        out["classification:classes"] = [{k: v for k, v in c.items() if k not in CLASS_HINTS}
                                         for c in out["classification:classes"]]
    return out


def template_column(column: Column, field_id: int, subsets: dict) -> dict:
    spec = column.spec
    fw = column.firmware
    kind = LOGICAL_TYPES.get(fw["unit"]) or PHYSICAL_TYPES.get(fw["type"])
    require(kind is not None, f"{column.name}: {fw['type']} needs a logical unit")
    out = {"name": column.name}
    out.update({key: spec[key] for key in HINT_HEAD if key in spec})
    out["type"] = kind
    for key in ("unit", "roles", "cf:standard_name"):
        if key in spec:
            out[key] = spec[key]
    kept = classes(column, subsets)
    if kept:
        out["classification:classes"] = kept
    out["opensensor:property"] = spec["opensensor:property"]
    out["opensensor:procedure"] = column.procedure
    out["opensensor:field_id"] = field_id
    for key in ("opensensor:valid_when", "opensensor:settle_s", "opensensor:derived", "opensensor:aggregate",
                "opensensor:plausible_range"):
        if key in spec and not (key == "opensensor:settle_s" and spec[key] == 0):
            out[key] = spec[key]
    out["opensensor:decimals"] = spec.get("opensensor:decimals", fw["decimals"])
    out.update({key: spec[key] for key in HINT_TAIL if key in spec})
    return out


def template_scales(root: Path, columns: list[Column]) -> dict:
    """The opensensor:scales a template carries, each scale its columns name, from the vendored registry."""
    registry = load_json(root / SCALES)["scales"]
    out = {}
    for c in columns:
        name = c.spec.get("opensensor:scale")
        if name is not None:
            require(name in registry, f"{c.name}: scale {name} is not in {SCALES}")
            require(registry[name].get("unit", c.spec.get("unit")) == c.spec.get("unit"),
                    f"{c.name}: scale {name} unit differs from the column unit")
            out.setdefault(name, registry[name])
    return out


def template_firmware_extras(board: Board) -> dict:
    """setup, settings and config_keys for the template firmware block, as board.json declares them."""
    return {key: board.manifest[key] for key in ("setup", "settings", "config_keys") if key in board.manifest}


def hinted(root: Path, board: Board, columns: list[Column], built: bytes) -> bytes:
    """The template tools/board_template.h built, with the display hints and the setup manifest added.

    Each built column must equal its manifest column without hints, so the
    manifest and the board tables still agree. The bytes stay minified
    printable ASCII, so non ASCII text is escaped.
    """
    template = json.loads(built)
    subsets = board.manifest.get("class_subsets", {})
    built_columns = template["item_assets"]["data"]["table:columns"]
    require(len(built_columns) == len(columns), f"{board.manifest['board']}: built template column count differs")
    full = []
    for index, (column, got) in enumerate(zip(columns, built_columns, strict=True)):
        expected = template_column(column, index + 1, subsets)
        require(without_hints(expected) == got,
                f"{board.manifest['board']} built column {index + 1} {column.name} differs, "
                f"{diff(without_hints(expected), got)}")
        full.append(expected)
    template["item_assets"]["data"]["table:columns"] = full
    out = {}
    for key, value in template.items():
        out[key] = value
        if key == "opensensor:sample_interval_s":
            if "primary" in board.manifest:
                out["opensensor:primary"] = board.manifest["primary"]
            scales = template_scales(root, columns)
            if scales:
                out["opensensor:scales"] = scales
    out["firmware"] = {**template["firmware"], **template_firmware_extras(board)}
    return json.dumps(out, separators=(",", ":"), ensure_ascii=True).encode("ascii")


def board_for_app(root: Path, app: Path) -> tuple[Board, list[Column]]:
    """The board whose board.json sits in app, and its expanded columns."""
    checks = validators(root)
    common = common_modules(root, checks)
    path = app / "board.json"
    require(path.is_file(), f"{app}: no board.json")
    board = load_board(root, path, checks, common)
    columns = expand(board, common)
    cross_rules(board.manifest["board"], columns, board, max_columns(root))
    return board, columns


def instrument(column: Column) -> bool:
    """tools/board_template.h instrument(), a procedure that names a fitted sensor."""
    return bool(column.model) and column.spec["roles"][0] != "referenceInformation" and \
        column.firmware["validity"] != "unavailable"


# Loading.


@dataclass
class Module:
    path: Path
    manifest: dict

    @property
    def id(self) -> str:
        return self.manifest["id"]


@dataclass
class Board:
    path: Path  # board.json
    manifest: dict
    modules: dict[str, Module] = field(default_factory=dict)  # local modules by id

    @property
    def directory(self) -> Path:
        return self.path.parent


def load_module(root: Path, path: Path, checks: dict) -> Module:
    manifest = load_json(path)
    validate(checks["module"], manifest, str(path.relative_to(root)))
    module = Module(path, manifest)
    # Checked on load, so it fails before any board row that names the module.
    require(module.id not in reserved_ids(root), f"{path.relative_to(root)}: module id {module.id!r} is reserved, "
                                                 f"ossensor::{module.id} would collide with a name in {SENSOR_GLUE} "
                                                 f"or a namespace it uses")
    names = [c["name"] for cols in manifest["segments"].values() for c in cols]
    require(len(names) == len(set(names)), f"{module.id}: a column name repeats inside the module")
    status = manifest.get("status")
    if status is not None:
        cols = [c for cols in manifest["segments"].values() for c in cols if c["name"] == status["column"]]
        require(len(cols) == 1, f"{module.id}: status column {status['column']} is not a module column")
        require(bool(cols[0].get("classification:classes")), f"{module.id}: status column has no classes")
    for cols in manifest["segments"].values():
        for c in cols:
            listed = c.get("classification:classes", [])
            values = [k["value"] for k in listed]
            require(values == sorted(set(values)), f"{c['name']}: class values ascending and unique")
            require(len({k["name"] for k in listed}) == len(listed), f"{c['name']}: class names unique")
    return module


def common_modules(root: Path, checks: dict) -> dict[str, Module]:
    paths = [root / SYSTEM_MODULE, *sorted((root / COMMON_MODULES).glob("*/module.json"))]
    out: dict[str, Module] = {}
    for path in paths:
        module = load_module(root, path, checks)
        require(module.id not in out, f"module id {module.id} repeats")
        if path != root / SYSTEM_MODULE:
            require(path.parent.name == module.id, f"{path.relative_to(root)}: directory is not the module id")
        else:
            require(module.id == "system", "system.module.json has id system")
        out[module.id] = module
    return out


def load_board(root: Path, path: Path, checks: dict, common: dict[str, Module]) -> Board:
    manifest = load_json(path)
    validate(checks["board"], manifest, str(path.relative_to(root)))
    board = Board(path, manifest)
    for relative in manifest.get("local_modules", []):
        module_path = (path.parent / relative).resolve()
        require(module_path.is_relative_to(path.parent.resolve()), f"{relative}: outside the board app")
        require(module_path.is_file(), f"{path.relative_to(root)}: missing local module {relative}")
        module = load_module(root, module_path, checks)
        require(module.id not in common and module.id not in board.modules,
                f"local module id {module.id} repeats a module id")
        require(module_path.parent.name == module.id, f"{relative}: directory is not the module id")
        board.modules[module.id] = module
    return board


def expand(board: Board, common: dict[str, Module]) -> list[Column]:
    seen = set()
    out: list[Column] = []
    for entry in board.manifest["segments"]:
        key = (entry["module"], entry["segment"])
        require(key not in seen, f"{board.manifest['board']}: segment {'.'.join(key)} repeats")
        seen.add(key)
        module = board.modules.get(entry["module"]) or common.get(entry["module"])
        require(module is not None, f"{board.manifest['board']}: unknown module {entry['module']}")
        segment = module.manifest["segments"].get(entry["segment"])
        require(segment is not None, f"{board.manifest['board']}: unknown segment {'.'.join(key)}")
        for spec in segment:
            fw = spec["firmware"]
            out.append(Column(module.id, entry["segment"], spec,
                              spec.get("opensensor:procedure", module.manifest["procedure"]),
                              fw.get("vendor", module.manifest.get("vendor", "")),
                              fw.get("model", module.manifest.get("model", ""))))
    return out


def max_columns(root: Path) -> int:
    found = re.search(r"\bkMaxColumns\s*=\s*(\d+)\s*;", (root / WRITER).read_text())
    require(found is not None, f"{WRITER}: kMaxColumns not found")
    return int(found.group(1))


def cross_rules(label: str, columns: list[Column], board: Board, cap: int) -> None:
    names = [c.name for c in columns]
    require(len(names) == len(set(names)), f"{label}: a column name repeats on the board")
    require(len(columns) <= cap, f"{label}: {len(columns)} columns, over kMaxColumns {cap}")
    properties = [c.spec["opensensor:property"] for c in columns]
    require(len(properties) == len(set(properties)), f"{label}: an opensensor:property repeats on the board")
    live = [c.firmware["live_key"] for c in columns if c.firmware["live_key"]]
    require(len(live) == len(set(live)), f"{label}: a LIVE key repeats on the board")
    known = set(names)
    for c in columns:
        gate = c.spec.get("opensensor:valid_when")
        require(gate is None or gate["column"] in known, f"{c.name}: valid_when names an unknown column")
        derived = c.spec.get("opensensor:derived")
        require(derived is None or set(derived["from"]) <= known, f"{c.name}: derived names an unknown column")
        require((derived is not None) == (c.spec["roles"] == ["modelResult"]), f"{c.name}: derived and modelResult")
        require(c.firmware["type"] == "Float" or c.firmware["decimals"] == 0, f"{c.name}: decimals on a non Float")
    for name in board.manifest.get("class_subsets", {}):
        require(name in known, f"{label}: class_subsets names unknown column {name}")
    for c in columns:
        bitfields = c.spec.get("classification:bitfields", [])
        require(not bitfields or c.spec.get("unit") == "{bitmask}", f"{c.name}: bit fields on a non {{bitmask}} column")
        width = {"Int32": 32, "Int64": 64}.get(c.firmware["type"], 0)
        used: set[int] = set()
        for b in bitfields:
            bits = set(range(b["offset"], b["offset"] + b["length"]))
            require(max(bits) < width, f"{c.name}: bit field {b.get('name', b['offset'])} is outside {c.firmware['type']}")
            require(not bits & used, f"{c.name}: bit fields overlap")
            used |= bits
            require(all(0 <= k["value"] < 2 ** b["length"] for k in b["classes"]),
                    f"{c.name}: a bit field class value does not fit its length")
    primary = board.manifest.get("primary")
    if primary is not None:
        found = [c for c in columns if c.name == primary]
        require(len(found) == 1 and found[0].spec["roles"] == ["physicalMeasurement"],
                f"{label}: primary {primary} is not a physicalMeasurement column of the board")
    setup_rules(label, board.manifest)


# Setup manifest (#184, decision 0003). The app owns these wizard step and
# settings section components, a board orders them. Any other id is a generic
# form of the keys it lists, and carries a label, since the app has no title of
# its own for it. `sensor` is the sensor identity and sticker card.
APP_STEPS = frozenset({"name", "location", "mobility", "site", "wifi", "pairing", "lan"})
APP_SECTIONS = frozenset({"location", "site", "wifi", "pairing", "lan", "bonds", "sensor"})
# Catalog ids of one list that the other list must not take as a board id.
SETUP_ONLY = APP_STEPS - APP_SECTIONS
SETTINGS_ONLY = APP_SECTIONS - APP_STEPS
# A part a secret key name would hold. PIN, PSK and LAN token are never declared.
SECRET_PARTS = frozenset({"pin", "psk", "token", "password", "secret"})



def setup_v(step_ids: list[str]) -> int:
    """The setup step list version the phone writes with thing.setup_ack. The
    first four bytes of SHA-256 over the ordered ids joined by a newline, read
    big endian with the top bit cleared, and 0 read as 1."""
    head = hashlib.sha256("\n".join(step_ids).encode()).digest()[:4]
    return int.from_bytes(head, "big") & 0x7FFFFFFF or 1

def setup_rules(label: str, manifest: dict) -> None:
    keys = manifest.get("config_keys", {})
    used: set[str] = set()
    for part in ("setup", "settings"):
        steps = manifest.get(part, [])
        ids = [s["id"] for s in steps]
        require(len(ids) == len(set(ids)), f"{label}: a {part} id repeats")
        catalog = APP_STEPS if part == "setup" else APP_SECTIONS
        for step in steps:
            listed = step.get("keys", [])
            require(part == "setup" or step["id"] not in SETUP_ONLY,
                    f"{label}: {step['id']} is a setup step, not a settings section")
            require(part == "settings" or step["id"] not in SETTINGS_ONLY,
                    f"{label}: {step['id']} is a settings section, not a setup step")
            if step["id"] in catalog:
                require(not listed, f"{label}: app step {step['id']} lists keys, the app owns its form")
            else:
                require(bool(listed), f"{label}: {part} step {step['id']} is not an app step and lists no keys")
                require("label" in step, f"{label}: {part} step {step['id']} is not an app step and has no label")
            missing = [k for k in listed if k not in keys]
            require(not missing, f"{label}: {part} step {step['id']} names undeclared keys {missing}")
            used.update(listed)
    unused = sorted(set(keys) - used)
    require(not unused, f"{label}: config_keys {unused} are in no setup step or settings section")
    for key, spec in keys.items():
        require(len(key.encode()) <= 48, f"{label}: {key} is over 48 bytes")
        require(not set(key.split(".")) & SECRET_PARTS, f"{label}: {key} looks like a secret, never declared")
        values = spec.get("values")
        if isinstance(values, dict):
            require(values["min"] <= values["max"], f"{label}: {key} range min is over max")
        if isinstance(values, list):
            require(all(len(v.encode()) <= 64 for v in values), f"{label}: {key} has a value over 64 bytes")
    # Page 7 nests keys by dotted prefix, so a key that is a prefix of another
    # would be both a leaf and an object.
    nested = sorted(k for k in keys if any(other.startswith(k + ".") for other in keys))
    require(not nested, f"{label}: config_keys {nested} are a dotted prefix of another key")


def live_keys_across(boards: dict[str, list[Column]]) -> None:
    owner: dict[str, tuple[str, str]] = {}
    for label, columns in boards.items():
        for c in columns:
            key = c.firmware["live_key"]
            if key:
                first = owner.setdefault(key, (c.name, label))
                require(first[0] == c.name, f"LIVE key {key} is {first[0]} on {first[1]} and {c.name} on {label}")


# Board files.


def inc_rows(text: str, macro: str) -> list[str]:
    rows = [line for line in text.splitlines() if line.startswith(macro + "(")]
    stray = [line for line in text.splitlines() if line.lstrip().startswith(macro + "(") and line not in rows]
    require(not stray, f"{macro} row not at column 0: {stray[:1]}")
    return rows


def class_rows(text: str) -> list[tuple[str, str, bool]]:
    code = re.sub(r"//[^\n]*", "", text)
    rows = [(m.group(1), m.group(3), m.group(4) == "true") for m in CLASS_ROW.finditer(code)]
    require(len(rows) == code.count("CLASS("), "a CLASS row did not parse")
    return rows


def segment_hash(rows: list[str]) -> str:
    return hashlib.sha256(("\n".join(rows) + "\n").encode()).hexdigest()


def lock_segments(columns: list[Column], rows: list[str]) -> list[dict]:
    out: list[dict] = []
    for index, column in enumerate(columns):
        if out and (out[-1]["module"], out[-1]["segment"]) == (column.module, column.segment):
            out[-1]["rows"] += 1
        else:
            out.append({"module": column.module, "segment": column.segment, "first": index + 1, "rows": 1})
    for entry in out:
        entry["sha256"] = segment_hash(rows[entry["first"] - 1:entry["first"] - 1 + entry["rows"]])
    return out


def git_base_lock(root: Path) -> BaseLock:
    """Read <path> at origin/<PR base or main>. Fail closed when the ref is missing.

    On a push to main GITHUB_BASE_REF is empty, so the base is the pushed
    commit and this compares the lock to itself. That is by design. Branch
    protection forces a pull request, where the base check is real, and the
    other lock rules still run on push.
    """
    ref = os.environ.get("PLUGIN_CONTRACT_BASE") or f"origin/{os.environ.get('GITHUB_BASE_REF') or 'main'}"

    def load(path: Path) -> dict | None:
        relative = path.resolve().relative_to(root.resolve()).as_posix()
        known = subprocess.run(["git", "rev-parse", "--verify", "--quiet", f"{ref}^{{commit}}"], cwd=root,
                               capture_output=True, text=True)
        require(known.returncode == 0, f"base ref {ref} is missing, fetch it or set PLUGIN_CONTRACT_BASE")
        shown = subprocess.run(["git", "show", f"{ref}:{relative}"], cwd=root, capture_output=True, text=True)
        if shown.returncode != 0:
            listed = subprocess.run(["git", "ls-tree", "--name-only", ref, "--", relative], cwd=root,
                                    capture_output=True, text=True)
            require(listed.returncode == 0 and not listed.stdout.strip(), f"cannot read {relative} at {ref}")
            # The lock is new on this branch, or its board directory moved. Say so,
            # so a move never passes unseen.
            print(f"NOTICE {relative} is not on {ref}, treating it as a new lock", file=sys.stderr, flush=True)
            return None
        return json.loads(shown.stdout)

    return load


def check_baseline(label: str, older: dict, newer: dict, notes: str) -> None:
    """An owner approved break (no backward compatibility, owner decision 14).

    Only the segments its baseline lists may differ from their shipped form.
    Every other shipped segment keeps its place, rows and hash, and the release
    still keeps every shipped segment in order and adds new ones only after
    them. Its migration entry must start a line with the exact heading and name
    its dict.
    """
    short = newer["dict"][:12]
    baseline = newer["baseline"]
    heading = f"## {baseline['migration']}"
    found = re.search(rf"^{re.escape(heading)}$", notes, re.MULTILINE)
    require(found is not None, f"{label} {LOCK}: baseline {short} names no {MIGRATION} entry {heading!r}")
    entry = notes[found.end():].split("\n## ", 1)[0]
    require(newer["dict"] in entry, f"{label} {LOCK}: the {MIGRATION} entry of baseline {short} never names its dict")

    def key(segment: dict) -> tuple[str, str]:
        return segment["module"], segment["segment"]

    shipped = older["segments"]
    kept = newer["segments"][:len(shipped)]
    require([key(s) for s in kept] == [key(s) for s in shipped],
            f"{label} {LOCK}: baseline {short} reorders or removes a shipped segment")
    allowed = {key(s) for s in baseline["segments"]}
    changed = {key(old) for old, new in zip(shipped, kept) if (old["rows"], old["sha256"]) != (new["rows"], new["sha256"])}
    for name in sorted(changed - allowed):
        raise AssertionError(f"{label} {LOCK}: baseline {short} edits shipped segment {'/'.join(name)}, "
                             f"which its baseline does not list")
    for name in sorted(allowed - changed):
        raise AssertionError(f"{label} {LOCK}: baseline {short} lists {'/'.join(name)}, "
                             f"which is not a shipped segment it edits")


def check_lock(root: Path, board: Board, raw: bytes, columns: list[Column], rows: list[str], checks: dict,
               base_lock: BaseLock) -> dict:
    path = board.directory / LOCK
    label = board.manifest["board"]
    require(path.is_file(), f"{label}: missing {LOCK}")
    lock = load_json(path)
    validate(checks["lock"], lock, str(path.relative_to(root)))
    require(lock["board"] == label, f"{LOCK}: board is {lock['board']}, not {label}")
    releases = lock["releases"]
    dicts = [r["dict"] for r in releases]
    require(len(dicts) == len(set(dicts)), f"{label} {LOCK}: a dict repeats")
    for release in releases:
        segments = release["segments"]
        first = 1
        for entry in segments:
            require(entry["first"] == first, f"{label} {LOCK}: segments of {release['dict'][:12]} are not contiguous")
            first += entry["rows"]
        require(first - 1 == release["rows"], f"{label} {LOCK}: rows of {release['dict'][:12]} do not add up")
    notes = (root / MIGRATION).read_text()
    require("baseline" not in releases[0], f"{label} {LOCK}: the first release cannot be a baseline")
    for older, newer in zip(releases, releases[1:]):
        if "baseline" in newer:
            check_baseline(label, older, newer, notes)
        else:
            require(newer["segments"][:len(older["segments"])] == older["segments"],
                    f"{label} {LOCK}: release {newer['dict'][:12]} reorders, removes or edits a shipped segment")
        require(newer["rows"] >= older["rows"], f"{label} {LOCK}: release {newer['dict'][:12]} drops rows")
    newest = releases[-1]
    digest = hashlib.sha256(raw).hexdigest()
    require(newest["dict"] == digest, f"{label}: telemetry_fields.inc is {digest}, the newest {LOCK} dict is "
                                      f"{newest['dict']}")
    require(newest["rows"] == len(columns), f"{label}: newest {LOCK} rows differ from the manifests")
    require(newest["segments"] == lock_segments(columns, rows),
            f"{label}: newest {LOCK} segments differ from the board manifest")
    for release in releases:
        require(release["dict"] in notes, f"{label}: {MIGRATION} never names dict {release['dict']}")
    base = base_lock(path)
    if base is not None:
        require(base.get("board") == label, f"{label}: base {LOCK} names another board")
        earlier = base.get("releases", [])
        require(len(earlier) <= len(releases), f"{label}: {LOCK} lost a release present on the base branch")
        for index, entry in enumerate(earlier):
            require(json.dumps(releases[index], sort_keys=True) == json.dumps(entry, sort_keys=True),
                    f"{label}: {LOCK} release {index + 1} differs from the base branch")
    return newest


def check_board(root: Path, board: Board, columns: list[Column], checks: dict, base_lock: BaseLock) -> dict:
    label = board.manifest["board"]
    directory = board.directory
    raw = (directory / "telemetry_fields.inc").read_bytes()
    have = inc_rows(raw.decode("utf-8"), "FIELD")
    want = [field_row(c) for c in columns]
    for index, (a, b) in enumerate(zip(want, have)):
        require(a == b, f"{label} FIELD row {index + 1} differs\n manifest {a}\n board    {b}")
    require(len(want) == len(have), f"{label}: {len(want)} manifest columns, {len(have)} FIELD rows")

    have_sem = inc_rows((directory / "telemetry_semantics.inc").read_text(), "SEM")
    want_sem = [sem_row(c) for c in columns]
    for index, (a, b) in enumerate(zip(want_sem, have_sem)):
        require(a == b, f"{label} SEM row {index + 1} differs\n manifest {a}\n board    {b}")
    require(len(want_sem) == len(have_sem), f"{label}: {len(want_sem)} manifest columns, {len(have_sem)} SEM rows")

    subsets = board.manifest.get("class_subsets", {})
    want_classes = [(c.name, k["name"], k.get("nodata", False)) for c in columns for k in classes(c, subsets)]
    have_classes = class_rows((directory / "telemetry_classes.inc").read_text())
    # The .inc groups rows by column in value order, but the column groups
    # need not follow field order, so compare per column.
    def grouped(rows: list[tuple[str, str, bool]]) -> dict[str, list[tuple[str, bool]]]:
        out: dict[str, list[tuple[str, bool]]] = {}
        for column, name, nodata in rows:
            require(column not in out or next(reversed(out)) == column,
                    f"{label}: CLASS rows of {column} are not grouped")
            out.setdefault(column, []).append((name, nodata))
        return out

    want_groups, have_groups = grouped(want_classes), grouped(have_classes)
    for column in sorted(set(want_groups) | set(have_groups)):
        require(want_groups.get(column) == have_groups.get(column),
                f"{label}: {column} classes differ, manifest {want_groups.get(column)} "
                f"telemetry_classes.inc {have_groups.get(column)}")

    # The lock first, so a dict move names the lock before the missing template.
    newest = check_lock(root, board, raw, columns, have, checks, base_lock)
    digest = newest["dict"]
    published = root / TEMPLATES / label / f"{digest}.json"
    require(published.is_file(), f"{label}: no published template for dict {digest}")
    template = load_json(published)
    manifest = board.manifest
    require(template["id"] == manifest["schema"] == template["firmware"]["schema"], f"{label}: schema differs")
    require(template["opensensor:sample_interval_s"] == manifest["sample_interval_s"],
            f"{label}: sample_interval_s differs from the template")
    require(template["title"] == manifest.get("title"), f"{label}: title differs from the template")
    require(template["keywords"] == manifest.get("keywords"), f"{label}: keywords differ from the template")
    instruments = list(dict.fromkeys(c.procedure for c in columns if instrument(c)))
    require(template["instruments"] == instruments, f"{label}: instruments differ, {instruments}")
    firmware = template["firmware"]
    live = {c.name: c.firmware["live_key"] for c in columns if c.firmware["live_key"]}
    require(firmware["live_keys"] == live and list(firmware["live_keys"]) == list(live),
            f"{label}: LIVE keys differ from the template")
    labels = {c.name: c.firmware["unit"] for c in columns if c.firmware["unit"] != c.spec.get("unit", "")}
    require(firmware["unit_labels"] == labels, f"{label}: unit labels differ from the template")
    require(template.get("opensensor:primary") == manifest.get("primary"), f"{label}: primary differs from the template")
    require(template.get("opensensor:scales", {}) == template_scales(root, columns),
            f"{label}: scales differ from the template")
    extras = template_firmware_extras(board)
    for key in ("setup", "settings", "config_keys"):
        require(firmware.get(key) == extras.get(key), f"{label}: firmware {key} differs from the template")
    published_columns = template["item_assets"]["data"]["table:columns"]
    require(len(published_columns) == len(columns), f"{label}: template column count differs")
    for index, (column, got) in enumerate(zip(columns, published_columns, strict=True)):
        expected = template_column(column, index + 1, subsets)
        if expected != got:
            require(False, f"{label} template column {index + 1} {column.name} differs, {diff(expected, got)}")
    return newest


def diff(expected: dict, got: dict) -> list[str]:
    keys = sorted(set(expected) | set(got))
    return [f"{k}: manifest {expected.get(k)!r} template {got.get(k)!r}" for k in keys if expected.get(k) != got.get(k)]


def check_examples(root: Path, checks: dict, common: dict[str, Module], cap: int) -> list[str]:
    directory = root / EXAMPLES
    modules = dict(common)
    for path in sorted(directory.glob("*.module.json")):
        module = load_module(root, path, checks)
        require(module.id not in modules, f"example module id {module.id} repeats a module id")
        modules[module.id] = module
    boards = sorted(directory.glob("*.board.json"))
    require(boards, f"{EXAMPLES}: no example board")
    out = []
    for path in boards:
        manifest = load_json(path)
        validate(checks["board"], manifest, str(path.relative_to(root)))
        board = Board(path, manifest)
        columns = expand(board, modules)
        cross_rules(manifest["board"], columns, board, cap)
        for c in columns:
            field_row(c)
            sem_row(c)
        out.append(f"{manifest['board']} {len(columns)} columns")
    return out


# Module C++ headers (README section 10, section 11 step 4).

HEADER_SUFFIX = "_module.h"
HEADER_WRITE = "pixi run plugin-headers"
CXX_TYPES = {"Int32": "std::int32_t", "Int64": "std::int64_t"}
# Reserved words a column, class or error name could spell. Names are already
# lower case [a-z][a-z0-9_]*, so only lower case keywords matter.
CXX_KEYWORDS = frozenset("""
alignas alignof and and_eq asm auto bitand bitor bool break case catch char char8_t char16_t char32_t class
compl concept const consteval constexpr constinit const_cast continue co_await co_return co_yield decltype
default delete do double dynamic_cast else enum explicit export extern false float for friend goto if inline
int long mutable namespace new noexcept not not_eq nullptr operator or or_eq private protected public register
reinterpret_cast requires return short signed sizeof static static_assert static_cast struct switch template
this thread_local throw true try typedef typeid typename union unsigned using virtual void volatile wchar_t
while xor xor_eq
""".split())
IDENTIFIER = re.compile(r"^[a-z][a-z0-9_]*$")
# Every generated header starts with these bytes. Only a file that does is
# ever removed or overwritten, so a hand written *_module.h is never lost.
GENERATED_HEAD = b"// clang-format off\n// GENERATED from "
# Namespaces a module namespace ossensor::<id> would shadow inside its own
# header or inside os_sensor_module.h, on top of the names that file declares.
SHADOWED = frozenset({"ossensor", "std", "telemetry", "oslogger"})
TOKEN = re.compile(r"[A-Za-z_]\w*|\S")


def header_path(module: Module) -> Path:
    return module.path.parent / f"{module.id}{HEADER_SUFFIX}"


def reserved_ids(root: Path) -> frozenset[str]:
    """Lower case names declared directly in namespace ossensor by
    os_sensor_module.h, such as detail, resolve and init_error, plus SHADOWED.
    A module id equal to one would collide with ossensor::<id>.

    It is a token scan, not a C++ parser. At the ossensor block's own brace
    depth and outside parentheses it takes a name followed by "(" (a
    function) or preceded by namespace, struct, class, union or using. Upper
    case k and CamelCase names cannot match a module id. It fails closed when
    the braces do not balance or detail and resolve are not found. A lower
    case variable, plain enum or concept added there needs this scan
    extended."""
    code = re.sub(r"//[^\n]*|/\*.*?\*/", " ", (root / SENSOR_GLUE).read_text(), flags=re.DOTALL)
    tokens = TOKEN.findall(code)
    names: set[str] = set()
    depth = parens = 0
    inside = None  # brace depth of the ossensor block, once open
    for index, token in enumerate(tokens):
        following = tokens[index + 1] if index + 1 < len(tokens) else ""
        previous = tokens[index - 1] if index else ""
        if token == "{":
            depth += 1
            if inside is None and tokens[index - 2:index] == ["namespace", "ossensor"]:
                inside = depth
        elif token == "}":
            if depth == inside:
                inside = None
            depth -= 1
        elif token == "(":
            parens += 1
        elif token == ")":
            parens -= 1
        elif inside is not None and depth == inside and parens == 0 and IDENTIFIER.match(token) and \
                token not in CXX_KEYWORDS and (following == "(" or previous in ("namespace", "struct", "class",
                                                                                "union", "using")):
            names.add(token)
    require(inside is None and depth == 0, f"{SENSOR_GLUE}: braces do not balance")
    require("detail" in names and "resolve" in names, f"{SENSOR_GLUE}: namespace ossensor declarations not found")
    return frozenset(names | SHADOWED)


def identifier(name: str, where: str) -> str:
    require(IDENTIFIER.match(name) is not None and name not in CXX_KEYWORDS and "__" not in name,
            f"{where}: {name!r} is not a C++ identifier")
    return name


def status_constant(name: str, where: str) -> str:
    """kStatus plus the class name in CamelCase, so not-fresh is kStatusNotFresh."""
    parts = [p for p in re.split(r"[-_ ]+", name) if p]
    require(parts and all(re.fullmatch(r"[A-Za-z0-9]+", p) for p in parts),
            f"{where}: class {name!r} has no C++ constant spelling")
    text = "kStatus" + "".join(p[0].upper() + p[1:] for p in parts)
    require(not text[len("kStatus"):][0].isdigit(), f"{where}: class {name!r} starts with a digit")
    return text


def module_header(root: Path, module: Module) -> str:
    """The C++ header of one module, as section 10 shows it."""
    manifest = module.manifest
    ns = identifier(module.id, f"{module.id} id")
    columns = [c for cols in manifest["segments"].values() for c in cols]
    names = [identifier(c["name"], f"{module.id} column") for c in columns]
    require(len(names) < 256, f"{module.id}: {len(names)} columns do not fit the uint8_t Column enum")
    source = module.path.resolve().relative_to(root.resolve()).as_posix()
    lines = [
        "// clang-format off",
        f"// GENERATED from {source} by tools/plugin_contract.py.",
        f"// Do not edit. Change the manifest and run `{HEADER_WRITE}`.",
        "#pragma once",
        "",
        "#include <array>",
        "#include <cstdint>",
        "#include <string_view>",
        "",
        f"namespace ossensor::{ns} {{",
        f"inline constexpr std::string_view kProcedure = {literal(manifest['procedure'], module.id)};",
    ]
    errors = manifest.get("init_errors", [])
    if errors:
        values = ", ".join(identifier(e, f"{module.id} init_errors") for e in errors)
        lines.append(f"enum class [[nodiscard]] InitError : std::uint8_t {{ {values} }};")
    lines.append("enum class Column : std::uint8_t {")
    lines += [f"  {name}," for name in names]
    lines.append("};")
    lines.append(f"inline constexpr std::array<std::string_view, {len(names)}> kColumns{{")
    lines += [f'    "{name}",' for name in names]
    lines.append("};")
    # A board selects segments, so resolve() can map only the ones it ships.
    segments = [identifier(s, f"{module.id} segment") for s in manifest["segments"]]
    lines.append("enum class Segment : std::uint8_t {")
    lines += [f"  {s}," for s in segments]
    lines.append("};")
    lines.append(f"inline constexpr std::array<Segment, {len(names)}> kColumnSegments{{")
    lines += [f"    Segment::{s}," for s, cols in manifest["segments"].items() for _ in cols]
    lines.append("};")
    status = manifest.get("status")
    if status is not None:
        column = next(c for c in columns if c["name"] == status["column"])
        kind = CXX_TYPES.get(column["firmware"]["type"])
        require(kind is not None, f"{module.id}: status column {column['name']} is not an integer column")
        lines.append(f"inline constexpr Column kStatusColumn = Column::{column['name']};")
        constants = [(status_constant(k["name"], column["name"]), number(k["value"], column["name"]))
                     for k in column["classification:classes"]]
        spelled = [name for name, _ in constants]
        require(len(spelled) == len(set(spelled)), f"{column['name']}: two classes spell one C++ constant")
        lines += [f"inline constexpr {kind} {name} = {value};" for name, value in constants]
    lines += [f"}} // namespace ossensor::{ns}", "// clang-format on", ""]
    return "\n".join(lines)


def header_modules(common: dict[str, Module], boards: list[Board]) -> list[Module]:
    """Shared sensor modules and board local modules. The logger fills the
    system segments by name, so system gets no header."""
    out = [m for m in common.values() if m.id != "system"]
    for board in boards:
        out += list(board.modules.values())
    return sorted(out, key=lambda m: m.path)


def header_roots(root: Path, modules: list[Module], boards: list[Board]) -> set[Path]:
    """Every folder that holds or held module manifests. A board's modules/
    folder counts after it drops its last local module."""
    return {root / COMMON_MODULES, *(b.directory / "modules" for b in boards), *(m.path.parent.parent for m in modules)}


def generated(path: Path) -> bool:
    with path.open("rb") as handle:
        return handle.read(len(GENERATED_HEAD)) == GENERATED_HEAD


def check_headers(root: Path, modules: list[Module], boards: list[Board], write: bool = False) -> list[str]:
    expected = {header_path(m): module_header(root, m) for m in modules}
    found = {p for directory in header_roots(root, modules, boards) if directory.is_dir()
             for p in directory.rglob(f"*{HEADER_SUFFIX}")}
    orphans = sorted(found - set(expected))
    for path in orphans:
        relative = path.relative_to(root).as_posix()
        require(generated(path), f"{relative}: hand written file with the generated {HEADER_SUFFIX} suffix and no "
                                 f"module manifest, rename it, nothing removes it")
        if write:
            path.unlink()
            print(f"REMOVED {relative}, no module manifest beside it", flush=True)
        else:
            raise AssertionError(f"{relative}: generated module header without a module manifest, "
                                 f"remove it or run `{HEADER_WRITE}`")
    out = []
    for path, text in sorted(expected.items()):
        relative = path.relative_to(root).as_posix()
        if write:
            if not path.is_file() or path.read_bytes() != text.encode():
                require(not path.exists() or generated(path),
                        f"{relative}: hand written file where the generated header goes, nothing overwrites it")
                path.write_bytes(text.encode())
                print(f"WROTE {relative}", flush=True)
        else:
            require(path.is_file(), f"{relative}: missing generated module header, run `{HEADER_WRITE}`")
            require(path.read_bytes() == text.encode(),
                    f"{relative}: stale generated module header, it differs from its manifest, run `{HEADER_WRITE}`")
        out.append(relative)
    return out


@dataclass(frozen=True)
class Result:
    board: str
    columns: int
    segments: int
    dict: str


def check(root: Path = ROOT, base_lock: BaseLock | None = None,
          write_headers: bool = False) -> tuple[list[Result], list[str], list[str]]:
    root = root.resolve()
    checks = validators(root)
    if base_lock is None:
        base_lock = git_base_lock(root)
    common = common_modules(root, checks)
    cap = max_columns(root)
    board_paths = sorted(root.glob(BOARD_GLOB))
    with_fields = {p.parent for p in root.glob(FIELDS_GLOB)}
    require(with_fields == {p.parent for p in board_paths},
            "every board app with telemetry_fields.inc has a board.json and the reverse, "
            f"{sorted(str(p.relative_to(root)) for p in with_fields ^ {p.parent for p in board_paths})}")
    boards = [load_board(root, path, checks, common) for path in board_paths]
    slugs = [b.manifest["board"] for b in boards]
    require(len(slugs) == len(set(slugs)), "a board slug repeats")
    expanded = {}
    for board in boards:
        columns = expand(board, common)
        cross_rules(board.manifest["board"], columns, board, cap)
        expanded[board.manifest["board"]] = columns
    live_keys_across(expanded)
    modules = header_modules(common, boards)
    if write_headers:
        check_headers(root, modules, boards, write=True)
    results = []
    for board in boards:
        columns = expanded[board.manifest["board"]]
        newest = check_board(root, board, columns, checks, base_lock)
        results.append(Result(board.manifest["board"], len(columns), len(newest["segments"]), newest["dict"]))
    # After the board files, so a manifest edit that also moves a board row
    # names that row first.
    headers = check_headers(root, modules, boards)
    examples = check_examples(root, checks, common, cap)
    return results, examples, headers


# Write mode for a new board (README section 11 step 7). Option A keeps every
# board file frozen, so this writes a board's files once and never again.

NEW_BOARD_FILES = ("telemetry_fields.inc", "telemetry_semantics.inc", "telemetry_classes.inc", LOCK)
WRITE_BOARD = "pixi run plugin-new-board"


def class_value(column: Column, value: int) -> str:
    """A boolean column ({boolean} unit) prints its 0 and 1 classes as false
    and true, as the shipped telemetry_classes.inc files do."""
    if column.spec.get("unit") == "{boolean}":
        require(value in (0, 1), f"{column.name}: boolean class value {value!r} is not 0 or 1")
        return "true" if value else "false"
    return number(value, column.name)


def board_texts(root: Path, board: Board, columns: list[Column], checks: dict) -> dict[str, bytes]:
    """The four files of a new board. Each FIELD, SEM and CLASS row is the one
    check_board() compares, so the gate accepts them as written."""
    source = board.path.resolve().relative_to(root.resolve()).as_posix()
    once = [f"// Written once from {source} by `{WRITE_BOARD}`.",
            "// Hand kept from then on. The plugin gate compares every row with the",
            "// manifests, and nothing rewrites this file."]
    fields = [
        "// FIELD(name, physical_type, procedure, unit, validity, live_key, decimals).",
        *once,
        "// live_key is the short LIVE JSON key, or \"\" for fields that stay out of",
        "// LIVE. decimals is the number of digits after the point when a Float",
        "// prints, and 0 for integer fields. Both are part of the dict digest.",
        "// Parquet/Iceberg field_id = 1-based order of the FIELD entries below.",
        "// Append new fields only, never reorder, remove or reuse a position.",
        *(field_row(c) for c in columns),
    ]
    semantics = [
        "// SEM(name, ucum, role, cf_standard_name, aggregate, plausible_min,",
        "//     plausible_max, settle_s, valid_when, property, sensor_vendor,",
        "//     sensor_model).",
        *once,
        "// One row per FIELD row in telemetry_fields.inc, same order, same name.",
        "// Not part of the dict digest, which hashes telemetry_fields.inc only.",
        *(sem_row(c) for c in columns),
    ]
    subsets = board.manifest.get("class_subsets", {})
    classes_text = [
        "// CLASS(column, value, name, nodata).",
        *once,
        "// One row per value of each code and boolean column, grouped by column",
        "// in value order. value is the manifest value. The board code may name",
        "// it by its enum later, the gate compares names and nodata only.",
        *(f"CLASS({c.name}, {class_value(c, k['value'])}, {literal(k['name'], c.name)}, "
          f"{'true' if k.get('nodata', False) else 'false'})" for c in columns for k in classes(c, subsets)),
    ]
    texts = {name: ("\n".join(lines) + "\n").encode() for name, lines in
             zip(NEW_BOARD_FILES, (fields, semantics, classes_text))}
    for index, column in enumerate(columns):
        template_column(column, index + 1, subsets)
    rows = inc_rows(texts["telemetry_fields.inc"].decode(), "FIELD")
    require(rows == [field_row(c) for c in columns], f"{board.manifest['board']}: a rendered FIELD row did not parse")
    require(inc_rows(texts["telemetry_semantics.inc"].decode(), "SEM") == [sem_row(c) for c in columns],
            f"{board.manifest['board']}: a rendered SEM row did not parse")
    require(class_rows(texts["telemetry_classes.inc"].decode()) ==
            [(c.name, k["name"], k.get("nodata", False)) for c in columns for k in classes(c, subsets)],
            f"{board.manifest['board']}: a rendered CLASS row did not parse")
    lock = {"board": board.manifest["board"], "releases": [{
        "dict": hashlib.sha256(texts["telemetry_fields.inc"]).hexdigest(),
        "rows": len(columns),
        "segments": lock_segments(columns, rows),
    }]}
    validate(checks["lock"], lock, f"{source} {LOCK}")
    texts[LOCK] = (json.dumps(lock, indent=2) + "\n").encode()
    return texts


def write_board(root: Path, path: Path) -> list[str]:
    """Write the four files of the new board at path, a board.json. Refuse
    when any of them exists, before writing anything, so an existing board
    file or lock is never touched."""
    root = root.resolve()
    path = path.resolve()
    boards = {p.resolve() for p in root.glob(BOARD_GLOB)}
    require(path in boards, f"{path}: not a board manifest, write mode takes {BOARD_GLOB} under {root}")
    targets = [path.parent / name for name in NEW_BOARD_FILES]
    present = [t.relative_to(root).as_posix() for t in targets if t.exists() or t.is_symlink()]
    require(not present, f"refusing to write a board file that exists, {', '.join(present)}. Board files are "
                         f"written once for a new board, then frozen (Option A)")
    checks = validators(root)
    common = common_modules(root, checks)
    cap = max_columns(root)
    board = load_board(root, path, checks, common)
    label = board.manifest["board"]
    others = [load_board(root, p, checks, common) for p in sorted(root.glob(BOARD_GLOB)) if p.resolve() != path]
    require(label not in {b.manifest["board"] for b in others}, f"{label}: the board slug already exists")
    columns = expand(board, common)
    cross_rules(label, columns, board, cap)
    live_keys_across({**{b.manifest["board"]: expand(b, common) for b in others}, label: columns})
    texts = board_texts(root, board, columns, checks)
    created: list[Path] = []
    try:
        for name, data in texts.items():
            # Exclusive create, so a file that appeared since the check above
            # is still never overwritten.
            with (path.parent / name).open("xb") as handle:
                created.append(path.parent / name)
                handle.write(data)
    except BaseException:
        # Leave no partial board. Remove only what this run created.
        for made in created:
            made.unlink(missing_ok=True)
        raise
    return [(path.parent / name).relative_to(root).as_posix() for name in texts]


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--root", type=Path, default=ROOT)
    parser.add_argument("--write-headers", action="store_true",
                        help="rewrite each module's generated C++ header from its manifest, then check")
    parser.add_argument("--write-board", type=Path, metavar="BOARD_JSON",
                        help="write telemetry_*.inc and a first shipped.lock beside a new board.json, "
                             "refusing when any of them exists")
    args = parser.parse_args()
    if args.write_board is not None:
        written = write_board(args.root, args.write_board)
        for relative in written:
            print(f"WROTE {relative}", flush=True)
        digest = hashlib.sha256((args.root.resolve() / written[0]).read_bytes()).hexdigest()
        print(f"dict {digest}. These files are now frozen. Next add telemetry_contract.h, the digest headers, "
              f"the published template, a mobile migration entry naming the dict, the LIVE fixture and the "
              f"pinned URIs, as docs/shared/board-porting.md lists. Run `pixi run plugin-headers` when the board "
              f"has local modules, then `pixi run plugin-contract`", flush=True)
        return
    results, examples, headers = check(args.root, write_headers=args.write_headers)
    for r in results:
        print(f"PASS {r.board} {r.columns} columns in {r.segments} segments reproduce FIELD, SEM, CLASS and the "
              f"template, dict {r.dict}", flush=True)
    print(f"PASS {len(headers)} generated module headers equal their manifests", flush=True)
    for line in examples:
        print(f"PASS example {line}", flush=True)


if __name__ == "__main__":
    main()
