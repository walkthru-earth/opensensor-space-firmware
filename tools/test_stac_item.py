"""Host gate for the STAC File Item builder in firmware/common/stac (#142).

Builds tools/test_stac_item.cpp once per board, with that board's field and
semantics tables, and proves six things.

1. The vendored stac-opensensor fixtures are the bytes of commit 83787768
   (tools/fixtures/stac/SHA256SUMS).
2. The board statistics table equals the measurement columns of the board's
   published template, with the same opensensor:valid_when gates.
3. round2, rfc3339 and the Item id match canonical.mjs, and every board error
   path reports its own error.
4. The Item for each fixture file equals the example under the canonical.mjs
   `differences` rules, with the same key order, and each case in cases.json
   gives the Item or the no Item reason canonical.mjs gave.
5. Every Item the board builds validates against the pinned profile.json and
   OpenSensor extension schema.json (tools/stac_opensensor/v0.1.0). The core
   STAC and the table, file, grid and anonymized-location schemas are remote
   and not vendored. stac-opensensor CI validates the two examples against
   them, and point 4 proves the board Items equal those examples.
6. Source checks pin the device only sidecar lines of os_logger.cpp. The
   SHA-256 gets the same bytes as the CRC-32 from one full read, the digest
   finishes only after the Parquet rename, the partial opens with O_EXCL and
   an existing .json is never replaced. Each listed mutant must fail.

Then it applies known bugs to copies of the sources and requires the gate to
catch each one. Host builds prove no SD, power cut or board behavior.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import math
from pathlib import Path
import re
import shutil
import subprocess
import tempfile

from jsonschema import Draft7Validator, FormatChecker
from referencing import Registry, Resource

import boards as registry
from host_cxx import CXX_STD
from h3_library import checkout, compile_library

ROOT = Path(__file__).resolve().parent.parent
FIXTURES = ROOT / "tools/fixtures/stac"
SCHEMAS = ROOT / "tools/stac_opensensor/v0.1.0"
COMMON = ROOT / "firmware/common"
# The stac-opensensor fixture file of each board, keyed by its board.json alias
# (#141). The app directory and the template slug come from the registry.
BOARDS = {
    "cores3": {
        "rows": "fixtures/thing=3f2b9c1e-6a7d-4e58-9b1c-2d4f6a8e0c13/"
                "data_1300_0f3a9c2e5b7d41e8a6c3f0b2d9e4a713_2881-2970-0.rows.json",
        "example": "examples/thing=3f2b9c1e-6a7d-4e58-9b1c-2d4f6a8e0c13/year=2026/month=10/day=24/"
                   "data_1300_0f3a9c2e5b7d41e8a6c3f0b2d9e4a713_2881-2970-0.json",
    },
    "waveshare": {
        "rows": "fixtures/thing=7c1e4b9a-3d52-4f86-a0b7-e9d2c5f81a46/"
                "data_0815_5e9d0c7b31a84f62b8d47a10c3e6f295_412-487-0.rows.json",
        "example": "examples/thing=7c1e4b9a-3d52-4f86-a0b7-e9d2c5f81a46/year=2026/month=10/day=24/"
                   "data_0815_5e9d0c7b31a84f62b8d47a10c3e6f295_412-487-0.json",
    },
}


def board_dir(board: str) -> Path:
    """The telemetry app of a board.json alias, which holds its generated headers."""
    return ROOT / registry.find(board).app


def registry_proofs() -> None:
    """Every registry board that writes the sidecar has fixtures here, and every
    fixture names a registry board."""
    every = registry.boards()
    aliases = {board.alias for board in every}
    require(set(BOARDS) <= aliases, f"fixtures name boards outside board.json {sorted(set(BOARDS) - aliases)}")
    for board in every:
        writes = any(".stac_columns =" in source.read_text()
                     for source in (ROOT / board.app).glob("*.cpp"))
        require(not writes or board.alias in BOARDS,
                f"{board.alias} sets Config::stac_columns but has no STAC fixture in tools/test_stac_item.py")
        require(board.alias not in BOARDS or writes,
                f"{board.alias} has a STAC fixture but its app never sets Config::stac_columns")


# canonical.mjs buildItem reasons and the board's ItemError names.
REASONS = {
    "not a dated data_HHMM_ file": "not-dated",
    "no Thing id or mobility in the footer": "no-thing",
    "no rows": "no-rows",
    "a row has no UTC time": "null-time",
    "no located row": "not-located",
    "a cell is not resolution 8": "not-resolution-8",
    "a fixed Thing file holds more than one cell": "fixed-many-cells",
}
MEASUREMENT = {"physicalMeasurement", "modelResult"}
NUMERIC = {"int8", "int16", "int32", "int64", "uint8", "uint16", "uint32", "uint64", "float", "double"}
TOLERANCE = 1e-9
NEAR = re.compile(r"^\.(geometry|bbox)|\.extent\.spatial")
SANITIZE = ["-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-fno-sanitize-recover=all"]


def require(condition: bool, message: str) -> None:
    if not condition:
        raise SystemExit(f"stac-item-test: {message}")


# ---- canonical.mjs differences, ported ----

def is_number(value: object) -> bool:
    return isinstance(value, (int, float)) and not isinstance(value, bool)


def differences(a: object, b: object, path: str = "", loose: bool = False) -> list[str]:
    here = path or "(root)"
    near = loose or NEAR.search(path) is not None
    if is_number(a) and is_number(b):
        same = abs(a - b) <= TOLERANCE if near else a == b
        return [] if same else [f"{here} {a} != {b}"]
    if isinstance(a, list) or isinstance(b, list):
        if not isinstance(a, list) or not isinstance(b, list):
            return [f"{here} is an array on one side only"]
        if len(a) != len(b):
            return [f"{here} has {len(a)} entries, not {len(b)}"]
        return [d for i, (x, y) in enumerate(zip(a, b)) for d in differences(x, y, f"{path}[{i}]", near)]
    if isinstance(a, dict) and isinstance(b, dict):
        out: list[str] = []
        for key in dict.fromkeys([*a, *b]):
            if key not in a or key not in b:
                out.append(f"{path}.{key} is on one side only")
            else:
                out += differences(a[key], b[key], f"{path}.{key}", near)
        return out
    same = type(a) is type(b) and a == b
    return [] if same else [f"{here} {json.dumps(a)} != {json.dumps(b)}"]


# Beyond the profile, which lets key order differ. The board writes the
# canonical.mjs order, so a reader can diff the bytes by eye.
def key_order(a: object, b: object, path: str = "") -> list[str]:
    if isinstance(a, dict) and isinstance(b, dict):
        if list(a) != list(b):
            return [f"{path or '(root)'} key order {list(a)} != {list(b)}"]
        return [d for key in a for d in key_order(a[key], b[key], f"{path}.{key}")]
    if isinstance(a, list) and isinstance(b, list):
        return [d for i, (x, y) in enumerate(zip(a, b)) for d in key_order(x, y, f"{path}[{i}]")]
    return []


def comparator_proofs(example: dict) -> None:
    """The comparator itself catches what it must and allows what it may."""
    require(differences(example, json.loads(json.dumps(example))) == [], "comparator equal")
    dropped = json.loads(json.dumps(example))
    dropped["geometry"]["coordinates"][0].pop()
    require(differences(dropped, example), "comparator misses a dropped closing point")
    for delta, caught in ((1e-8, True), (1e-10, False)):
        moved = json.loads(json.dumps(example))
        moved["geometry"]["coordinates"][0][2][0] += delta
        moved["bbox"][1] -= delta
        require(bool(differences(moved, example)) == caught,
                f"comparator tolerance at {delta}")
    stats = json.loads(json.dumps(example))
    first = stats["assets"]["data"]["table:columns"][0]["statistics"]
    first["mean"] = math.nextafter(first["mean"], math.inf)
    require(differences(stats, example), "comparator misses a 1 ulp mean change")
    order = json.loads(json.dumps(example))
    order["links"].reverse()
    require(differences(order, example), "comparator misses array order")
    missing = json.loads(json.dumps(example))
    del missing["properties"]["anon:warning"]
    require(differences(missing, example), "comparator misses a missing key")
    typed = json.loads(json.dumps(example))
    typed["properties"]["datetime"] = 0
    require(differences(typed, example), "comparator misses null versus 0")
    shuffled = dict(reversed(list(example.items())))
    require(differences(shuffled, example) == [] and key_order(shuffled, example),
            "key order is free to differences and checked by key_order")


# ---- profile and extension schemas ----

def schema_validators() -> dict[str, Draft7Validator]:
    """Validators for the pinned profile.json and extension schema.json."""
    require("uri" in FormatChecker().checkers, "format uri is enforced (rfc3986-validator)")
    loaded = {name: json.loads((SCHEMAS / name).read_text()) for name in ("profile.json", "schema.json")}
    registry = Registry().with_resources(
        [(contents["$id"].rstrip("#"), Resource.from_contents(contents)) for contents in loaded.values()])
    return {name: Draft7Validator(contents, registry=registry, format_checker=FormatChecker())
            for name, contents in loaded.items()}


def schema_problems(validators: dict[str, Draft7Validator], item: dict, label: str) -> list[str]:
    return [f"{label} {name} {error.json_path} {error.message[:120]}"
            for name, validator in validators.items() for error in validator.iter_errors(item)]


def schema_proofs(validators: dict[str, Draft7Validator], example: dict) -> None:
    """The schemas pass the example and reject what each one must."""
    require(not schema_problems(validators, example, "example"), "the example fails its schemas")
    absolute = json.loads(json.dumps(example))
    absolute["assets"]["data"]["href"] = "https://example.com/data.parquet"
    require(list(validators["profile.json"].iter_errors(absolute)), "profile.json misses an absolute href")
    checksum = json.loads(json.dumps(example))
    checksum["assets"]["data"]["file:checksum"] = "00"
    require(list(validators["profile.json"].iter_errors(checksum)), "profile.json misses a bad checksum")
    mobility = json.loads(json.dumps(example))
    mobility["properties"]["opensensor:mobility"] = "parked"
    require(list(validators["schema.json"].iter_errors(mobility)), "schema.json misses a bad mobility")


# ---- fixtures ----

def manifest() -> None:
    lines = (FIXTURES / "SHA256SUMS").read_text().splitlines()
    listed = {}
    for line in lines:
        digest, name = line.split("  ", 1)
        listed[name] = digest
    # Finder writes .DS_Store on macOS. It is never tracked.
    present = {p.relative_to(FIXTURES).as_posix() for p in FIXTURES.rglob("*")
               if p.is_file() and p.name != ".DS_Store"}
    present -= {"SHA256SUMS", "README.md"}
    require(present == set(listed), f"fixture files differ from SHA256SUMS: {sorted(present ^ set(listed))}")
    for name, digest in listed.items():
        actual = hashlib.sha256((FIXTURES / name).read_bytes()).hexdigest()
        require(actual == digest, f"{name} is {actual}, SHA256SUMS says {digest}")


def load_base(board: str) -> dict:
    fixture = json.loads((FIXTURES / BOARDS[board]["rows"]).read_text())
    return {
        "file": dict(fixture["file"]),
        "rows": json.loads(json.dumps(fixture["rows"])),
        "footer": json.loads((FIXTURES / fixture["footer"]).read_text()),
    }


def apply(data: dict, ops: list[dict]) -> dict:
    """generate_cases.mjs apply(), the same edits."""
    for op in ops:
        kind = op["op"]
        if kind == "set":
            values = data["rows"][op["column"]]
            rows = range(len(values)) if op["rows"] == "all" else op["rows"]
            for i in rows:
                values[i] = op["value"]
        elif kind == "file":
            data["file"][op["key"]] = op["value"]
        elif kind == "thing":
            data["footer"]["thing"][op["key"]] = op["value"]
        elif kind == "truncate":
            for name in data["rows"]:
                data["rows"][name] = data["rows"][name][: op["rows"]]
        else:
            raise SystemExit(f"stac-item-test: unknown op {kind}")
    return data


def value_text(value: object) -> str:
    if value is None:
        return "null"
    if isinstance(value, bool):
        raise SystemExit("stac-item-test: boolean row value")
    if isinstance(value, float):
        return repr(value)  # shortest text that reads back to the same double
    return str(value)


def input_text(data: dict) -> str:
    file = data["file"]
    thing = data["footer"]["thing"]
    require(" " not in file["name"] and "\n" not in file["name"], "file name without white space")
    lines = [
        f"file {file['name']}",
        f"size {file['size']}",
        f"sha256 {file['sha256']}",
        f"thing {thing['id'] or '-'}",
        f"mobility {thing['mobility']}",
        f"interval_ms {data['footer']['acquisition']['sample_interval_ms']}",
    ]
    for name, values in data["rows"].items():
        lines.append(" ".join(["col", name, *map(value_text, values)]))
    return "\n".join(lines) + "\n"


def run_item(executable: Path, directory: Path, data: dict) -> str:
    path = directory / "input.txt"
    path.write_text(input_text(data))
    return subprocess.run([str(executable), "item", str(path)], check=True,
                          capture_output=True, text=True).stdout.strip()


def numbers_text(cases: dict) -> str:
    lines = [f"round2 {a} {b}" for a, b in cases["round2"]]
    lines += [f"rfc3339 {us} {text}" for us, text in cases["rfc3339"]]
    lines += [f"id x{name.encode().hex()} {'-' if ident is None else 'x' + ident.encode().hex()}"
              for name, ident in cases["item_ids"]]
    return "\n".join(lines) + "\n"


# ---- checks ----

def template_table(board: str) -> list[str]:
    """The statistics columns canonical.mjs columnStatistics reads."""
    digest = re.search(r'"([0-9a-f]{64})"',
                       (board_dir(board) / "telemetry_dictionary_digest.h").read_text())
    require(digest is not None, f"{board} dictionary digest")
    path = ROOT / "docs/shared/templates" / registry.find(board).slug / f"{digest.group(1)}.json"
    require(path.exists(), f"{board} has no published template {path.name}")
    template = json.loads(path.read_text())
    out = []
    for column in template["item_assets"]["data"]["table:columns"]:
        if (column.get("roles") or [None])[0] not in MEASUREMENT or column["type"] not in NUMERIC:
            continue
        gate = column.get("opensensor:valid_when")
        if gate is None:
            out.append(f"{column['name']} {column['type']} none")
        elif gate.get("finite") is True:
            out.append(f"{column['name']} {column['type']} finite {gate['column']}")
        else:
            out.append(f"{column['name']} {column['type']} equals {gate['column']} {gate['equals']}")
    return out


def board_checks(board: str, executable: Path, directory: Path, cases: dict,
                 validators: dict[str, Draft7Validator]) -> tuple[list[str], int]:
    """Every equality and schema check of one board. Returns the problems
    found and the number of Items validated against the schemas."""
    problems: list[str] = []
    validated = 0
    example = json.loads((FIXTURES / BOARDS[board]["example"]).read_text())
    output = run_item(executable, directory, load_base(board))
    if not output.startswith("{"):
        problems.append(f"{board} fixture gave {output}")
    else:
        item = json.loads(output)
        problems += [f"{board} fixture {d}" for d in differences(item, example)]
        problems += [f"{board} fixture {d}" for d in key_order(item, example)]
        problems += schema_problems(validators, item, f"{board} fixture")
        validated += 1
    for case in cases["cases"]:
        if case["base"] != board:
            continue
        output = run_item(executable, directory, apply(load_base(board), case["ops"]))
        expect = case["expect"]
        if "none" in expect:
            want = f"none {REASONS[expect['none']]}"
            if output != want:
                problems.append(f"{case['name']} gave {output[:60]}, want {want}")
        elif not output.startswith("{"):
            problems.append(f"{case['name']} gave {output}, want an Item")
        else:
            item = json.loads(output)
            problems += [f"{case['name']} {d}" for d in differences(item, expect["item"])]
            problems += [f"{case['name']} {d}" for d in key_order(item, expect["item"])]
            problems += schema_problems(validators, item, case["name"])
            validated += 1
    return problems, validated


def build(board: str, directory: Path, h3_objects: list[str], sanitize: bool,
          sources: Path = COMMON, name: str = "test") -> Path:
    executable = directory / f"{board}-{name}"
    include = ["src", "sensors/src", "sensors/modules", "logger/src", "connectivity/src",
               "runtime/src", "location/src", "power/src", "stac/src"]
    # The test calls H3 directly for its reference outlines, so it sees the
    # H3 headers the pinned library compiled with.
    library = checkout(ROOT)
    command = ["clang++", CXX_STD, "-Wall", "-Wextra", "-Werror", "-ffp-contract=off",
               "-I", str(board_dir(board)), "-I", str(library / "include"),
               "-I", str(directory / "embedded-h3-location/include"),
               "-I", str(library / "vendor/h3/h3lib/include")]
    for path in include:
        command += ["-I", str(sources / path)]
    if sanitize:
        command += SANITIZE
    if sources != COMMON:
        # A mutant may leave a helper unused. Warnings still print.
        command.remove("-Werror")
    command += [str(ROOT / "tools/test_stac_item.cpp"),
                str(sources / "stac/src/os_stac_item.cpp"), *h3_objects, "-o", str(executable)]
    subprocess.run(command, check=True)
    return executable


# Each mutation is a bug the gate must catch, given as (name, file, old text,
# new text, board). The old text must appear exactly once.
MUTATIONS = [
    ("accept a cell that is not resolution 8", "stac/src/os_stac_item.cpp",
     "if (!published || *published != cell)", "if (!published)", "cores3"),
    ("count rows that fail opensensor:valid_when", "stac/src/os_stac_item.cpp",
     "!std::isfinite(value) || !gate_passes(column, data, valid)", "!std::isfinite(value)", "cores3"),
    ("let a null gate value pass", "stac/src/os_stac_item.cpp",
     "if (valid[column.gate_field] == 0)\n    return false;", "if (false)\n    return false;", "cores3"),
    ("leave each ring open", "stac/src/os_stac_item.cpp",
     "for (std::uint32_t i = 0; i < r.count; ++i) {",
     "for (std::uint32_t i = 0; i + 1 < r.count; ++i) {", "cores3"),
    ("skip the cell sort", "stac/src/os_stac_item.cpp",
     "std::ranges::sort(cells);", "", "waveshare"),
    ("round half down in round2", "stac/src/os_stac_item.cpp",
     "const double shifted = scaled + 0.5;", "const double shifted = scaled;", "cores3"),
    ("allow two cells for a fixed Thing", "stac/src/os_stac_item.cpp",
     "distinct.size() > 1", "distinct.size() > 2", "cores3"),
    ("keep trailing zeros of the fraction", "stac/src/os_stac_item.cpp",
     "while (out[used - 1] == '0')", "while (false)", "cores3"),
]


def mutation_proofs(directory: Path, h3_objects: list[str], cases: dict, numbers: Path,
                    validators: dict[str, Draft7Validator]) -> None:
    for index, (name, relative, old, new, board) in enumerate(MUTATIONS):
        sources = directory / f"mutant-{index}"
        for part in ("location/src", "stac/src"):
            shutil.copytree(COMMON / part, sources / part)
        for part in ("src", "sensors/src", "sensors/modules", "logger/src", "connectivity/src",
                     "runtime/src", "power/src"):
            (sources / part).parent.mkdir(parents=True, exist_ok=True)
            (sources / part).symlink_to(COMMON / part)
        target = sources / relative
        text = target.read_text()
        require(text.count(old) == 1, f"mutation '{name}' text is not in {relative} once")
        target.write_text(text.replace(old, new))
        executable = build(board, directory, h3_objects, False, sources, f"mutant-{index}")
        problems, _ = board_checks(board, executable, directory, cases, validators)
        unit = subprocess.run([str(executable), "unit", str(numbers)], capture_output=True, text=True)
        if unit.returncode != 0:
            problems.append("unit checks failed")
        require(bool(problems), f"mutation '{name}' was not caught")
        print(f"stac mutation caught: {name} ({problems[0][:90]})")



# Device only integrity lines of the sidecar in os_logger.cpp. The host build
# never runs them, so source checks pin them, as tools/test_logger_footer.py
# does for the footer gate. Each listed mutant must fail its check.
LOGGER = COMMON / "logger/src/os_logger.cpp"
# check_file() hands the SHA-256 the same bytes as the CRC-32, from one read.
SHA_FEED = re.compile(
    r"crc = crc32_update\(crc, writer_state->crc_scratch, count\);\s*"
    r"if \(sha\)\s*sha->update\(\{writer_state->crc_scratch, count\}\);\s*"
    r"read_bytes \+= count;")
# finalized_file() reads the whole file, never a CRC cache hit, when it hashes.
SHA_READ = re.compile(r"bool ok = check_file\(path, file, check, false, sha\);")
# write_sidecar() creates its partial and never truncates one already there.
SIDECAR_OPEN = re.compile(
    r"out\.fd = ::open\(partial, O_WRONLY \| O_CREAT \| O_EXCL, 0600\);")
# The final .json is checked right before the rename and never replaced.
SIDECAR_NO_REPLACE = re.compile(
    r"if \(::access\(path, F_OK\) == 0\) \{\s*"
    r"reason = \"exists\";\s*error = EEXIST;\s*"
    r"\} else if \(::rename\(partial, path\) != 0\) \{")
# path is <ready without .parquet>.json and partial is path plus .partial.
SIDECAR_NAMES = re.compile(
    r"path_length = std::snprintf\(\s*path, sizeof\(path\), \"%\.\*s\.json\",\s*"
    r"static_cast<int>\(ready_path\.size\(\) - kParquet\.size\(\)\), ready\);\s*"
    r"partial_length =\s*std::snprintf\(partial, sizeof\(partial\), \"%s\.partial\", path\);")
# finalize_file() order. The digest begins before the one read, an error path
# aborts it, and it finishes only after the Parquet rename.
FINALIZE_ORDER = (
    "if (const auto begun = stac_state->sha.begin(); begun)",
    "ok = finalized_file(target.partial, size, crc, sha) &&",
    "ok = ::access(ready, F_OK) != 0 && ::rename(target.partial, ready) == 0;",
    "if (sha)\n      sha->abort();\n    close_failed(target, \"finalize\"",
    "PARQUET READY name=",
    "if (sha) {\n      if (const auto finished = sha->finish(); finished)",
    "write_sidecar(target, ready, size, digest ? &*digest : nullptr, sha_error);",
)


def logger_body(source: str, signature: str) -> str:
    start = source.find(signature)
    require(start >= 0, f"{signature} not found in os_logger.cpp")
    end = source.find("\n}\n", start)
    require(end > start, f"end of {signature} not found")
    return source[start:end]


def check_logger_sidecar(source: str) -> None:
    check = logger_body(source, "\nbool check_file(const char *path")
    require(len(SHA_FEED.findall(check)) == 1,
            "check_file() must feed sha->update({crc_scratch, count}) right after the CRC")
    require(check.count("sha->") == 1, "check_file() touches the SHA-256 only in that update")
    finalized = logger_body(source, "\nbool finalized_file(const char *path")
    require(len(SHA_READ.findall(finalized)) == 1,
            "finalized_file() must hash through check_file(path, file, check, false, sha)")
    sidecar = logger_body(source, "\nvoid write_sidecar(const OutputFile &target")
    require(len(SIDECAR_NAMES.findall(sidecar)) == 1,
            "write_sidecar() must name <file>.json and <file>.json.partial from the ready name")
    require(len(SIDECAR_OPEN.findall(sidecar)) == 1 and sidecar.count("::open(") == 1,
            "write_sidecar() must open only its partial with O_CREAT | O_EXCL")
    require(len(SIDECAR_NO_REPLACE.findall(sidecar)) == 1 and sidecar.count("::rename(") == 1,
            "write_sidecar() must refuse an existing .json right before its one rename")
    finalize = logger_body(source, "\nbool finalize_file(OutputFile &target)")
    at = 0
    for step in FINALIZE_ORDER:
        found = finalize.find(step, at)
        require(found >= 0, f"finalize_file() lacks, or reorders, {step.splitlines()[0]}")
        at = found + len(step)
    require(finalize.count("sha->finish()") == 1 and finalize.count("sha.begin()") == 1,
            "finalize_file() begins and finishes the digest once")


LOGGER_MUTANTS = [
    ("sha fed count - 1 bytes", "sha->update({writer_state->crc_scratch, count});",
     "sha->update({writer_state->crc_scratch, count - 1});"),
    ("sha fed the whole scratch buffer", "sha->update({writer_state->crc_scratch, count});",
     "sha->update({writer_state->crc_scratch, kCrcScratchBytes});"),
    ("sha update removed", "if (sha)\n      sha->update({writer_state->crc_scratch, count});\n", ""),
    ("hash after a CRC cache hit", "bool ok = check_file(path, file, check, false, sha);",
     "bool ok = check_file(path, file, check, true, sha);"),
    ("partial truncated", "out.fd = ::open(partial, O_WRONLY | O_CREAT | O_EXCL, 0600);",
     "out.fd = ::open(partial, O_WRONLY | O_CREAT | O_TRUNC, 0600);"),
    ("final name opened", "out.fd = ::open(partial, O_WRONLY | O_CREAT | O_EXCL, 0600);",
     "out.fd = ::open(path, O_WRONLY | O_CREAT | O_EXCL, 0600);"),
    ("existing check on the partial", "if (::access(path, F_OK) == 0) {",
     "if (::access(partial, F_OK) != 0) {"),
    ("existing check inverted", "if (::access(path, F_OK) == 0) {",
     "if (::access(path, F_OK) != 0) {"),
    ("existing check removed", "if (::access(path, F_OK) == 0) {", "if (false) {"),
    ("sidecar named .parquet.json", "\"%.*s.json\",\n        static_cast<int>(ready_path.size() - kParquet.size()), ready);",
     "\"%.*s.json\",\n        static_cast<int>(ready_path.size()), ready);"),
    ("digest skips the read", "ok = finalized_file(target.partial, size, crc, sha) &&",
     "ok = finalized_file(target.partial, size, crc, nullptr) &&"),
    ("abort removed", "if (sha)\n      sha->abort();\n    close_failed(target, \"finalize\"",
     "close_failed(target, \"finalize\""),
]


def logger_sidecar_proofs() -> None:
    source = LOGGER.read_text()
    check_logger_sidecar(source)
    finalize = logger_body(source, "\nbool finalize_file(OutputFile &target)")
    finish = "    if (sha) {\n      if (const auto finished = sha->finish(); finished)"
    require(finalize.count(finish) == 1, "finalize_file() digest finish block not found")
    moved = source.replace(finish, "    if (false) {\n      if (const auto finished = sha->finish(); finished)")
    early = source.replace(
        "  if (ok)\n    ok = finalized_file(target.partial, size, crc, sha) &&",
        "  if (sha)\n    (void)sha->finish();\n  if (ok)\n    ok = finalized_file(target.partial, size, crc, sha) &&")
    mutants = [(name, source.replace(old, new)) for name, old, new in LOGGER_MUTANTS]
    mutants += [("finish never reached", moved), ("finish before the read", early)]
    for name, mutant in mutants:
        require(mutant != source, f"logger mutant '{name}' text not found")
        try:
            check_logger_sidecar(mutant)
        except SystemExit:
            print(f"logger sidecar mutant caught: {name}")
            continue
        require(False, f"logger sidecar check missed the mutant: {name}")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sanitize", action="store_true")
    parser.add_argument("--skip-mutations", action="store_true")
    args = parser.parse_args()
    manifest()
    registry_proofs()
    logger_sidecar_proofs()
    cases = json.loads((FIXTURES / "cases.json").read_text())
    require(cases["canonical_sha256"] == "98583cdb3c5b625f0d998e4650ee3a303c2ab4addd53f9486820c641182b9090",
            "cases.json is not from canonical.mjs at 83787768")
    require(cases["h3_js"] == "4.5.0", "cases.json is not from h3-js 4.5.0")
    comparator_proofs(json.loads((FIXTURES / BOARDS["cores3"]["example"]).read_text()))
    validators = schema_validators()
    for board in BOARDS:
        schema_proofs(validators, json.loads((FIXTURES / BOARDS[board]["example"]).read_text()))
    with tempfile.TemporaryDirectory(prefix="os-stac-item-test-") as temporary:
        directory = Path(temporary)
        h3_objects, _ = compile_library(ROOT, directory, args.sanitize)
        numbers = directory / "numbers.txt"
        numbers.write_text(numbers_text(cases))
        for board in BOARDS:
            executable = build(board, directory, h3_objects, args.sanitize)
            table = subprocess.run([str(executable), "table"], check=True, capture_output=True,
                                   text=True).stdout.splitlines()
            require(table == template_table(board),
                    f"{board} statistics table differs from its template\n{table}\n{template_table(board)}")
            fixture_columns = set(load_base(board)["rows"])
            require(all(line.split()[0] in fixture_columns for line in table),
                    f"{board} fixture rows lack a statistics column, the example would differ")
            subprocess.run([str(executable), "unit", str(numbers)], check=True)
            problems, validated = board_checks(board, executable, directory, cases, validators)
            require(not problems, f"{board} differs from canonical.mjs or its schemas\n"
                    + "\n".join(problems[:20]))
            count = 1 + sum(case["base"] == board for case in cases["cases"])
            print(f"stac {board}: {len(table)} statistics columns match the template, "
                  f"{count} Items and reasons equal canonical.mjs, "
                  f"{validated} Items valid against profile.json and schema.json")
        if not args.skip_mutations:
            if args.sanitize:
                # Mutants build without sanitizers, so the library objects
                # must too. The generated h3api.h is the same in both folders.
                (directory / "plain").mkdir()
                h3_objects, _ = compile_library(ROOT, directory / "plain", False)
            mutation_proofs(directory, h3_objects, cases, numbers, validators)
    print("stac-item-test: passed")


if __name__ == "__main__":
    main()
