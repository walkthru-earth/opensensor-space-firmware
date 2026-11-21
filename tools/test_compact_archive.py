"""Host test for tools/compact_archive.py on board-shaped fixture trees.

Run with ``pixi run compact-test [--sanitize]``.

tools/compactor_fixture.cpp writes dated Hive trees with exactly the firmware
writer and two board field tables (CoreS3 bringup, CrowPanel logger), one
boot per Thing, in UTC rotation windows. The test compacts a tree that
crosses a month boundary, with a located and an unlocated board, a
STORE_UNCOMPRESSED Thing, an unsynced file and a file without field ids, by
month and by day. It checks row counts, content, field ids, logical types,
codecs, sort order and provenance, that sources stay byte for byte, that a
rerun and an existing output are refused, and that verify catches a changed
source or output. Then it breaks the input in ways a real archive can (footer
Thing differs from the path, a duplicate file, a file in the wrong day folder,
output inside input) and checks that each run stops without publishing. No
network, serial port or board is used.
"""

from __future__ import annotations

import argparse
import datetime as dt
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

import duckdb
import pyarrow as pa
import pyarrow.parquet as pq

sys.path.insert(0, str(Path(__file__).resolve().parent))
import compact_archive as ca  # noqa: E402
from host_cxx import CXX_STD  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent
BOARDS = {
    "cores3": ("firmware/esp-idf-cores3/bringup/telemetry_fields.inc", "m5stack-cores3"),
    "crowpanel": ("firmware/esp-idf-crowpanel-epaper/logger/telemetry_fields.inc", "elecrow-crowpanel-epaper-42"),
}
# Thing, fixture, codec, boot, start (UTC), files, rotation seconds, H3 cell.
THINGS = [
    ("6875c9dc-b5ba-4461-9154-ea2770be9879", "cores3", "lz4", "4e1530a2ddf7f63740bbb9562a209228",
     dt.datetime(2026, 10, 31, 23, 7, 30, tzinfo=dt.UTC), 6, 900, "883e6640b9fffff"),
    ("6875c9dc-b5ba-4461-9154-ea2770be987a", "cores3", "none", "4e1530a2ddf7f63740bbb9562a20922a",
     dt.datetime(2026, 10, 31, 22, 0, 0, tzinfo=dt.UTC), 3, 3600, "883e6640b1fffff"),
    ("7875c9dc-b5ba-4461-9154-ea2770be9879", "crowpanel", "lz4", "5e1530a2ddf7f63740bbb9562a209228",
     dt.datetime(2026, 10, 31, 23, 0, 0, tzinfo=dt.UTC), 4, 1800, "none"),
]


def require(condition: bool, description: str) -> None:
    if not condition:
        raise AssertionError(description)


def compile_fixture(inc: Path, exe: Path, sanitize: bool) -> None:
    command = [
        "clang++", CXX_STD, "-Wall", "-Wextra", "-Werror",
        "-I", str(ROOT / "firmware/common/vendor/lz4"),
        f'-DOS_FIELDS_INC="{inc.resolve()}"',
    ]
    if sanitize:
        command += ["-fsanitize=address,undefined", "-fno-sanitize-recover=all"]
    command += [
        str(ROOT / "tools/compactor_fixture.cpp"),
        str(ROOT / "firmware/common/src/parquet_writer.cpp"),
        str(ROOT / "firmware/common/src/lz4_codec.cpp"),
        "-o", str(exe),
    ]
    subprocess.run(command, check=True)


def build_tree(work: Path, fixtures: dict[str, Path]) -> tuple[Path, int]:
    raw = work / "raw"
    rows = 0
    for i, (thing, board, codec, boot, start, files, rotation, cell) in enumerate(THINGS):
        footer = work / f"footer-{i}.json"
        footer.write_text(json.dumps({
            "version": "0.1.0",
            "thing": {"id": thing, "device_id": f"{74886896839488 + i:012x}", "board": BOARDS[board][1]},
            "dictionary": {"sha256": "fixture"}, "template": {"sha256": "fixture"},
        }), encoding="utf-8")
        run = subprocess.run([
            str(fixtures[board]), str(raw), codec, thing, boot, str(74886896839488 + i),
            str(int(start.timestamp()) * 1_000_000), str(files), str(rotation), cell,
            str(312345678 + i), str(300456789 + i), str(7 + i), str(footer),
        ], check=True, capture_output=True, text=True)
        info = json.loads(run.stdout)
        require(info["files"] == files, f"fixture {thing} file count")
        rows += info["rows"]
    # A boot without UTC parks rows under unsynced/, which the compactor leaves.
    first = sorted((raw / f"thing={THINGS[0][0]}").rglob("*.parquet"))[0]
    unsynced = raw / f"thing={THINGS[0][0]}" / "unsynced" / f"boot={THINGS[0][3]}" / first.name
    unsynced.parent.mkdir(parents=True)
    shutil.copyfile(first, unsynced)
    # A file from a writer without field ids, at a dated Thing path.
    legacy = first.parent / f"data_2359_{'0' * 32}_0-1-0.parquet"
    pq.write_table(pa.table({"sequence": pa.array([1, 2], pa.int32())}), legacy)
    return raw, rows


def tree_digest(root: Path) -> dict[str, str]:
    return {p.relative_to(root).as_posix(): hashlib.sha256(p.read_bytes()).hexdigest()
            for p in sorted(root.rglob("*")) if p.is_file()}


def published(root: Path) -> list[Path]:
    return sorted(root.rglob("compact_*.parquet")) if root.exists() else []


def expect_error(description: str, action, fragment: str) -> None:
    try:
        action()
    except ca.CompactError as error:
        require(fragment in str(error), f"{description}: message {error}")
        return
    raise AssertionError(f"{description}: no CompactError")


def check_output_file(path: Path, board: str) -> None:
    parquet = pq.ParquetFile(path)
    schema = parquet.schema_arrow
    ids = {f.name: int((f.metadata or {}).get(b"PARQUET:field_id", b"-1")) for f in schema}
    require(ids["thing_id"] == ca.THING_FIELD_ID, f"{path.name}: thing_id field id")
    device = {n: i for n, i in ids.items() if n != "thing_id"}
    require(sorted(device.values()) == list(range(1, len(device) + 1)), f"{path.name}: device field ids 1..N")
    for name in ("event_time_utc_us", "clock_anchor_utc_us"):
        if name in ids:
            kind = schema.field(name).type
            require(pa.types.is_timestamp(kind) and kind.unit == "us" and kind.tz == "UTC",
                    f"{path.name}: {name} is {kind}")
    codecs = {r[0] for r in duckdb.execute("SELECT DISTINCT compression FROM parquet_metadata(?)",
                                           [str(path)]).fetchall()}
    require(codecs == {"LZ4_RAW"}, f"{path.name}: codecs {codecs}")
    require(parquet.metadata.format_version != "1.0", f"{path.name}: Parquet V2")
    kv = parquet.metadata.metadata
    provenance = json.loads(kv[b"opensensor_compaction"])
    require(provenance["board"] == board and provenance["tool"] == ca.TOOL_ID, f"{path.name}: provenance")
    require(("geometry" in ids) == (b"geo" in kv), f"{path.name}: geo key follows the geometry column")
    order = [k for k in ca.SORT_KEYS if k in ids]
    # DuckDB sorts NULLs last. Timestamps come back as epoch micros, since the
    # pixi environment has no pytz for TIMESTAMPTZ values.
    select = ", ".join(f"epoch_us({k})" if k.endswith("_utc_us") else k for k in order)
    keys = duckdb.execute(f"SELECT {select} FROM read_parquet(?)", [str(path)]).fetchall()
    require(keys == sorted(keys, key=lambda row: tuple((v is None, v if v is not None else 0) for v in row)),
            f"{path.name}: rows sorted by {order}")


def check_compact(raw: Path, rows: int, work: Path) -> None:
    before = tree_digest(raw)
    out = work / "out-month"
    report = ca.compact(raw, out)
    require(tree_digest(raw) == before, "sources unchanged after a month run")
    require(report["totals"]["rows"] == rows, f"month rows {report['totals']['rows']} != {rows}")
    reasons = sorted(item["reason"] for item in report["skipped"])
    require(reasons == ["no field ids (legacy, DR 0002 item 1)", "unsynced, no UTC date yet"], f"skip reasons {reasons}")
    dirs = sorted(Path(o["path"]).parent.as_posix() for o in report["outputs"])
    require(dirs == [
        "board=elecrow-crowpanel-epaper-42/year=2026/month=10", "board=elecrow-crowpanel-epaper-42/year=2026/month=11",
        "board=m5stack-cores3/year=2026/month=10", "board=m5stack-cores3/year=2026/month=11",
    ], f"month partitions {dirs}")
    for output in report["outputs"]:
        require(output["content"]["ok"] and output["content"]["rows_in"] == output["rows"], "content check")
        check_output_file(out / output["path"], output["path"].split("/")[0].removeprefix("board="))
    require(sum(o["rows"] for o in report["outputs"]) == rows, "output rows add up")
    manifest = Path(report["manifest"])
    require(ca.verify(raw, manifest)["ok"], "verify a clean month run")
    # The same source set again, into the same prefix, is refused.
    expect_error("rerun", lambda: ca.compact(raw, out), "compacted already")
    require(tree_digest(raw) == before, "sources unchanged after a refused rerun")
    # An output name that exists is never overwritten, even without a manifest.
    taken = work / "out-taken"
    shutil.copytree(out, taken)
    shutil.rmtree(taken / "_manifests")
    expect_error("existing output", lambda: ca.compact(raw, taken), "never overwritten")

    day = work / "out-day"
    report = ca.compact(raw, day, by_day=True)
    require(report["totals"]["rows"] == rows, "day rows")
    require(all("/day=" in o["path"] for o in report["outputs"]), "day partitions")
    require(len(report["outputs"]) == 4, f"day outputs {len(report['outputs'])}")
    for output in report["outputs"]:
        check_output_file(day / output["path"], output["path"].split("/")[0].removeprefix("board="))
    require(ca.verify(raw, Path(report["manifest"]))["ok"], "verify a clean day run")

    # verify catches a changed source and a changed output.
    copy = work / "raw-copy"
    shutil.copytree(raw, copy)
    report = ca.compact(copy, work / "out-copy")
    target = copy / report["sources"][0]["path"]
    target.write_bytes(target.read_bytes()[:-1] + b"X")
    result = ca.verify(copy, Path(report["manifest"]))
    require(not result["ok"] and any("changed" in p for p in result["problems"]), "verify catches a changed source")
    output = work / "out-copy" / report["outputs"][0]["path"]
    os.chmod(output, 0o644)
    output.write_bytes(b"PAR1")
    result = ca.verify(raw, Path(report["manifest"]))
    require(not result["ok"], "verify catches a changed output")

    bench = ca.measure(raw, out, runs=1)
    require(set(bench["queries"]) == {"fleet_summary", "one_thing_one_day", "one_h3_cell"}, "measure queries")
    require(bench["queries"]["one_thing_one_day"]["raw_day_folder"]["opens"] > 0, "measure counts file opens")


def check_refusals(raw: Path, work: Path) -> None:
    def fresh(name: str) -> Path:
        tree = work / name
        shutil.copytree(raw, tree)
        return tree

    thing = THINGS[0][0]
    source = sorted((raw / f"thing={thing}" / "year=2026").rglob("data_*_0-*.parquet"))[0]
    rel = source.relative_to(raw / f"thing={thing}")

    tree = fresh("bad-thing")
    other = tree / "thing=00000000-0000-4000-8000-000000000001" / rel
    other.parent.mkdir(parents=True)
    shutil.copyfile(source, other)
    expect_error("footer Thing differs from the path", lambda: ca.compact(tree, work / "o-thing"), "footer")
    require(not published(work / "o-thing"), "nothing published for a Thing mismatch")

    tree = fresh("bad-duplicate")
    copy = (tree / f"thing={thing}" / rel).with_name(source.name.replace("-0.parquet", "-1.parquet"))
    shutil.copyfile(source, copy)
    expect_error("duplicate rows", lambda: ca.compact(tree, work / "o-dup"), "keys repeat")
    require(not published(work / "o-dup"), "nothing published for duplicate rows")

    tree = fresh("bad-day")
    moved = tree / f"thing={thing}" / "year=2026" / "month=10" / "day=30" / source.name
    moved.parent.mkdir(parents=True)
    shutil.move(tree / f"thing={thing}" / rel, moved)
    expect_error("row date differs from the path", lambda: ca.compact(tree, work / "o-day"), "differs from the path")
    require(not published(work / "o-day"), "nothing published for a wrong day folder")

    expect_error("output inside input", lambda: ca.compact(raw, raw / "out"), "outside the input")
    expect_error("input inside output", lambda: ca.compact(raw, raw.parent), "outside the input")
    empty = work / "empty"
    empty.mkdir()
    expect_error("empty tree", lambda: ca.compact(empty, work / "o-empty"), "no dated Thing")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--sanitize", action="store_true", help="build the fixture with ASan and UBSan")
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="os-compact-") as temporary:
        work = Path(temporary)
        fixtures = {}
        for name, (inc, _) in BOARDS.items():
            fixtures[name] = work / f"fixture-{name}"
            compile_fixture(ROOT / inc, fixtures[name], args.sanitize)
        raw, rows = build_tree(work, fixtures)
        check_compact(raw, rows, work)
        check_refusals(raw, work)
    suffix = " (ASan + UBSan fixture)" if args.sanitize else ""
    print(f"PASS compact archive, {len(THINGS)} Things on {len(BOARDS)} boards, {rows} rows, "
          f"month and day partitions, verify and refusals{suffix}")


if __name__ == "__main__":
    main()
