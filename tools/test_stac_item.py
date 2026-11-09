"""Host gate for the STAC File Item builder in firmware/common/stac (#142).

Builds tools/test_stac_item.cpp once per board, with that board's field and
semantics tables, and proves four things.

1. The vendored stac-opensensor fixtures are the bytes of commit 83787768
   (tools/fixtures/stac/SHA256SUMS).
2. The board statistics table equals the measurement columns of the board's
   published template, with the same opensensor:valid_when gates.
3. round2, rfc3339 and the Item id match canonical.mjs, and every board error
   path reports its own error.
4. The Item for each fixture file equals the example under the canonical.mjs
   `differences` rules, with the same key order, and each case in cases.json
   gives the Item or the no Item reason canonical.mjs gave.

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

from host_cxx import CXX_STD
from test_location import compile_h3

ROOT = Path(__file__).resolve().parent.parent
FIXTURES = ROOT / "tools/fixtures/stac"
COMMON = ROOT / "firmware/common"
BOARDS = {
    "cores3": {
        "dir": ROOT / "firmware/esp-idf-cores3/bringup",
        "template_dir": "m5stack-cores3",
        "rows": "fixtures/thing=3f2b9c1e-6a7d-4e58-9b1c-2d4f6a8e0c13/"
                "data_1300_0f3a9c2e5b7d41e8a6c3f0b2d9e4a713_2881-2970-0.rows.json",
        "example": "examples/thing=3f2b9c1e-6a7d-4e58-9b1c-2d4f6a8e0c13/year=2026/month=10/day=24/"
                   "data_1300_0f3a9c2e5b7d41e8a6c3f0b2d9e4a713_2881-2970-0.json",
    },
    "waveshare": {
        "dir": ROOT / "firmware/esp-idf-waveshare-sim7670g/logger",
        "template_dir": "waveshare-esp32-s3-sim7670g",
        "rows": "fixtures/thing=7c1e4b9a-3d52-4f86-a0b7-e9d2c5f81a46/"
                "data_0815_5e9d0c7b31a84f62b8d47a10c3e6f295_412-487-0.rows.json",
        "example": "examples/thing=7c1e4b9a-3d52-4f86-a0b7-e9d2c5f81a46/year=2026/month=10/day=24/"
                   "data_0815_5e9d0c7b31a84f62b8d47a10c3e6f295_412-487-0.json",
    },
}
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
                       (BOARDS[board]["dir"] / "telemetry_dictionary_digest.h").read_text())
    require(digest is not None, f"{board} dictionary digest")
    path = ROOT / "docs/shared/templates" / BOARDS[board]["template_dir"] / f"{digest.group(1)}.json"
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


def board_checks(board: str, executable: Path, directory: Path, cases: dict) -> list[str]:
    """Every equality check of one board. Returns the problems found."""
    problems: list[str] = []
    example = json.loads((FIXTURES / BOARDS[board]["example"]).read_text())
    output = run_item(executable, directory, load_base(board))
    if not output.startswith("{"):
        problems.append(f"{board} fixture gave {output}")
    else:
        item = json.loads(output)
        problems += [f"{board} fixture {d}" for d in differences(item, example)]
        problems += [f"{board} fixture {d}" for d in key_order(item, example)]
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
    return problems


def build(board: str, directory: Path, h3_objects: list[str], sanitize: bool,
          sources: Path = COMMON, name: str = "test") -> Path:
    executable = directory / f"{board}-{name}"
    include = ["src", "sensors/src", "sensors/modules", "logger/src", "connectivity/src",
               "runtime/src", "location/src", "power/src", "stac/src"]
    command = ["clang++", CXX_STD, "-Wall", "-Wextra", "-Werror", "-ffp-contract=off",
               "-I", str(BOARDS[board]["dir"]), "-I", str(directory / "h3"),
               "-I", str(COMMON / "location/vendor/h3/h3lib/include")]
    for path in include:
        command += ["-I", str(sources / path)]
    if sanitize:
        command += SANITIZE
    if sources != COMMON:
        # A mutant may leave a helper unused. Warnings still print.
        command.remove("-Werror")
    command += [str(ROOT / "tools/test_stac_item.cpp"),
                str(sources / "location/src/os_location.cpp"),
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
    ("leave each ring open", "location/src/os_location.cpp",
     "points[point_count] =\n        points[point_count - static_cast<std::size_t>(loop.numVerts)];",
     "points[point_count] = points[point_count - 1];", "cores3"),
    ("skip the cell sort", "stac/src/os_stac_item.cpp",
     "std::ranges::sort(cells);", "", "waveshare"),
    ("round half down in round2", "stac/src/os_stac_item.cpp",
     "const double shifted = scaled + 0.5;", "const double shifted = scaled;", "cores3"),
    ("allow two cells for a fixed Thing", "stac/src/os_stac_item.cpp",
     "distinct.size() > 1", "distinct.size() > 2", "cores3"),
    ("keep trailing zeros of the fraction", "stac/src/os_stac_item.cpp",
     "while (out[used - 1] == '0')", "while (false)", "cores3"),
]


def mutation_proofs(directory: Path, h3_objects: list[str], cases: dict, numbers: Path) -> None:
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
        problems = board_checks(board, executable, directory, cases)
        unit = subprocess.run([str(executable), "unit", str(numbers)], capture_output=True, text=True)
        if unit.returncode != 0:
            problems.append("unit checks failed")
        require(bool(problems), f"mutation '{name}' was not caught")
        print(f"stac mutation caught: {name} ({problems[0][:90]})")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sanitize", action="store_true")
    parser.add_argument("--skip-mutations", action="store_true")
    args = parser.parse_args()
    manifest()
    cases = json.loads((FIXTURES / "cases.json").read_text())
    require(cases["canonical_sha256"] == "98583cdb3c5b625f0d998e4650ee3a303c2ab4addd53f9486820c641182b9090",
            "cases.json is not from canonical.mjs at 83787768")
    require(cases["h3_js"] == "4.5.0", "cases.json is not from h3-js 4.5.0")
    comparator_proofs(json.loads((FIXTURES / BOARDS["cores3"]["example"]).read_text()))
    with tempfile.TemporaryDirectory(prefix="os-stac-item-test-") as temporary:
        directory = Path(temporary)
        h3_objects = compile_h3(ROOT, directory, args.sanitize)
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
            problems = board_checks(board, executable, directory, cases)
            require(not problems, f"{board} differs from canonical.mjs\n" + "\n".join(problems[:20]))
            count = 1 + sum(case["base"] == board for case in cases["cases"])
            print(f"stac {board}: {len(table)} statistics columns match the template, "
                  f"{count} Items and reasons equal canonical.mjs")
        if not args.skip_mutations:
            if args.sanitize:
                # Mutants build without sanitizers, so the H3 objects must too.
                # The generated h3api.h is the same in both folders.
                (directory / "plain").mkdir()
                h3_objects = compile_h3(ROOT, directory / "plain", False)
            mutation_proofs(directory, h3_objects, cases, numbers)
    print("stac-item-test: passed")


if __name__ == "__main__":
    main()
