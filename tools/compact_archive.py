#!/usr/bin/env python3
"""Host compactor prototype for board Parquet archives (#136, DR 0002).

Reads a dated Hive tree of board files, the layout the storage worker writes
and the phone or laptop archive keeps,

    <root>/thing=<uuid>/year=YYYY/month=MM/day=DD/data_HHMM_<boot>_<a>-<b>-<n>.parquet

with DuckDB `read_parquet` and explicit `hive_types`, and writes one file per
board and month (or day) under a separate output prefix,

    <out>/board=<slug>/year=YYYY/month=MM/compact_<digest>.parquet

The output is LZ4_RAW Parquet with V2 data pages, sorted by Thing, H3 cell and
event time, with every device column keeping its field id and its logical
type, so the UTC columns stay TIMESTAMP(MICROS, UTC). The Thing UUID that the
device keeps only in the path and footer becomes a `thing_id` UUID column with
host field id 1001, outside the device range 1 to 96 (`kMaxColumns`) and far
from the ids Iceberg reserves near 2^31. The `geo` key is GeoParquet 2.0 with
the union of the source boxes, and `opensensor_compaction` records the sources.

Nothing under the input root is written. Unsynced files, files without field
ids (legacy, DR 0002 item 1) and files that do not parse are listed as skipped
with a reason. A Thing id in the footer that differs from the path, a row
whose UTC date differs from its path date, a column name that maps to two
field ids within a board and a duplicate (device, boot, sequence) key each
stop the run. Before a file is published the tool checks row counts, a
multiset hash of every row and an exact EXCEPT ALL both ways, field ids,
logical types and codecs, and that every source still has the SHA-256 it had
when the run started. Output files are written to a staging directory and
linked into place without overwriting anything. A manifest with every source
and output SHA-256 goes to `<out>/_manifests/`.

Usage

    pixi run compact compact --input <root> --output <out> [--partition day]
    pixi run compact verify --input <root> --manifest <out>/_manifests/<m>.json
    pixi run compact measure --input <root> --output <out> [--runs 5]
    pixi run compact ducklake --input <root> --scratch <dir>

`measure` runs the reader benchmark (DR 0002 item 6) on the local disk and
counts DuckDB file system opens, reads and bytes. `ducklake` copies the
dated sources into the scratch directory, registers and merges them in a
DuckLake catalog there and reports sizes, field ids and whether DuckLake
cleanup deletes registered files. It installs the ducklake extension into the
scratch directory, so it needs the network once and is not part of the gate.
"""

from __future__ import annotations

import argparse
import collections
from dataclasses import dataclass, field
import datetime as dt
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import statistics
import sys
import time

import duckdb
import pyarrow.parquet as pq

TOOL_ID = "opensensor-space-firmware compact_archive 0.1.0"
THING_FIELD_ID = 1001
DEVICE_MAX_FIELD_ID = 96  # telemetry::kMaxColumns
HIVE_TYPES = "{'thing': UUID, 'year': INTEGER, 'month': INTEGER, 'day': INTEGER}"
SORT_KEYS = ("thing_id", "h3_cell_id", "event_time_utc_us", "device_id", "boot_id_hi", "boot_id_lo", "sequence")
ROW_KEY = ("device_id", "boot_id_hi", "boot_id_lo", "sequence")
ROW_GROUP_SIZE = 122_880
DATED = re.compile(
    r"(?:^|/)thing=(?P<thing>[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12})"
    r"/year=(?P<year>\d{4})/month=(?P<month>\d{2})/day=(?P<day>\d{2})"
    r"/data_\d{4}_[0-9a-f]{32}_\d+-\d+-\d+\.parquet$"
)


class CompactError(Exception):
    """A check failed. Nothing is published for the partition that failed."""


@dataclass
class Source:
    path: Path
    rel: str
    thing: str
    year: int
    month: int
    day: int
    board: str
    dictionary: dict
    template: dict
    field_ids: dict[str, int]
    logical: dict[str, str]
    rows: int
    size: int
    sha256: str
    bbox: list[float] | None


@dataclass
class Partition:
    board: str
    year: int
    month: int
    day: int | None
    sources: list[Source] = field(default_factory=list)

    def directory(self) -> str:
        parts = [f"board={self.board}", f"year={self.year:04d}", f"month={self.month:02d}"]
        if self.day is not None:
            parts.append(f"day={self.day:02d}")
        return "/".join(parts)


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for block in iter(lambda: handle.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def quote_ident(name: str) -> str:
    return '"' + name.replace('"', '""') + '"'


def quote_text(text: str) -> str:
    return "'" + text.replace("'", "''") + "'"


def sql_list(paths: list[Path]) -> str:
    return "[" + ", ".join(quote_text(str(p)) for p in paths) + "]"


# A plain INT32 or INT64 column with no annotation reads the same as a signed
# Int annotation of that width. DuckDB writes the annotation, the board writer
# leaves it out, so both spell the same type here.
PLAIN_SIGNED = {"INT32": "Int(bitWidth=32, isSigned=true)", "INT64": "Int(bitWidth=64, isSigned=true)"}


def logical_types(meta: pq.FileMetaData) -> dict[str, str]:
    schema = meta.schema
    types = {}
    for i in range(len(schema)):
        column = schema.column(i)
        kind = str(column.logical_type)
        if kind == "None" and column.physical_type in PLAIN_SIGNED:
            kind = PLAIN_SIGNED[column.physical_type]
        types[column.name] = f"{column.physical_type} {kind}"
    return types


def inspect(path: Path, rel: str, match: re.Match) -> Source | str:
    """Returns a Source, or the reason the file is skipped."""
    try:
        parquet = pq.ParquetFile(path)
    except Exception as error:  # noqa: BLE001, any reader failure is a skip reason
        return f"unreadable ({type(error).__name__})"
    meta = parquet.metadata
    ids: dict[str, int] = {}
    for item in parquet.schema_arrow:
        raw = (item.metadata or {}).get(b"PARQUET:field_id")
        if raw is None:
            return "no field ids (legacy, DR 0002 item 1)"
        ids[item.name] = int(raw)
    kv = meta.metadata or {}
    if b"opensensor" not in kv:
        return "no opensensor footer key"
    footer = json.loads(kv[b"opensensor"])
    thing = footer.get("thing", {})
    if thing.get("id") != match["thing"]:
        raise CompactError(f"{rel}: footer thing.id {thing.get('id')!r} differs from the path")
    if not thing.get("board"):
        return "no thing.board in the opensensor key"
    bbox = None
    if b"geo" in kv:
        geo = json.loads(kv[b"geo"])
        bbox = geo.get("columns", {}).get(geo.get("primary_column", ""), {}).get("bbox")
    if any(v < 1 or v > DEVICE_MAX_FIELD_ID for v in ids.values()):
        raise CompactError(f"{rel}: a field id is outside 1..{DEVICE_MAX_FIELD_ID}")
    return Source(
        path=path, rel=rel, thing=match["thing"], year=int(match["year"]), month=int(match["month"]),
        day=int(match["day"]), board=thing["board"], dictionary=footer.get("dictionary", {}),
        template=footer.get("template", {}), field_ids=ids, logical=logical_types(meta),
        rows=meta.num_rows, size=path.stat().st_size, sha256=sha256_file(path), bbox=bbox,
    )


def discover(root: Path) -> tuple[list[Source], list[dict]]:
    sources: list[Source] = []
    skipped: list[dict] = []
    for path in sorted(root.rglob("*.parquet")):
        rel = path.relative_to(root).as_posix()
        if "/unsynced/" in f"/{rel}":
            skipped.append({"path": rel, "reason": "unsynced, no UTC date yet"})
            continue
        match = DATED.search(rel)
        if not match:
            skipped.append({"path": rel, "reason": "not a dated Thing path"})
            continue
        found = inspect(path, rel, match)
        if isinstance(found, str):
            skipped.append({"path": rel, "reason": found})
        else:
            sources.append(found)
    return sources, skipped


def plan(sources: list[Source], by_day: bool) -> list[Partition]:
    boards: dict[str, dict[str, int]] = {}
    for source in sources:
        ids = boards.setdefault(source.board, {})
        for name, fid in source.field_ids.items():
            if ids.setdefault(name, fid) != fid:
                raise CompactError(f"board {source.board}: column {name} has field ids {ids[name]} and {fid}")
        if "thing_id" in source.field_ids:
            raise CompactError(f"{source.rel}: a device column is named thing_id")
    for board, ids in boards.items():
        if len(set(ids.values())) != len(ids):
            raise CompactError(f"board {board}: two columns share one field id")
    groups: dict[tuple, Partition] = {}
    for source in sources:
        day = source.day if by_day else None
        key = (source.board, source.year, source.month, day)
        groups.setdefault(key, Partition(source.board, source.year, source.month, day)).sources.append(source)
    return [groups[k] for k in sorted(groups, key=lambda k: (k[0], k[1], k[2], k[3] or 0))]


def columns_of(partition: Partition) -> list[tuple[str, int]]:
    ids: dict[str, int] = {}
    for source in partition.sources:
        ids.update(source.field_ids)
    return sorted(ids.items(), key=lambda item: item[1])


def source_relation(paths: list[Path]) -> str:
    return (f"read_parquet({sql_list(paths)}, hive_partitioning=true, hive_types={HIVE_TYPES}, "
            "union_by_name=true)")


def select_list(columns: list[tuple[str, int]], thing: str) -> str:
    return ", ".join([f"{thing} AS thing_id"] + [quote_ident(name) for name, _ in columns])


def bbox_union(sources: list[Source]) -> list[float] | None:
    boxes = [s.bbox for s in sources if s.bbox]
    if not boxes:
        return None
    return [min(b[0] for b in boxes), min(b[1] for b in boxes), max(b[2] for b in boxes), max(b[3] for b in boxes)]


def geo_value(bbox: list[float] | None) -> str:
    column: dict = {"encoding": "WKB", "geometry_types": ["Point"]}
    if bbox:
        column["bbox"] = bbox
    return json.dumps({"version": "2.0.0", "primary_column": "geometry", "columns": {"geometry": column}},
                      separators=(",", ":"))


def source_digest(sources: list[Source]) -> str:
    lines = "".join(f"{s.rel}\t{s.sha256}\n" for s in sorted(sources, key=lambda s: s.rel))
    return hashlib.sha256(lines.encode()).hexdigest()


def check_input(con: duckdb.DuckDBPyConnection, relation: str, partition: Partition) -> int:
    """Row level checks on the sources. Returns the row count."""
    # Every row must sit in the file for its own UTC date, in day and month
    # partitions alike. make_timestamp keeps the date math in UTC without ICU.
    utc = "make_timestamp(epoch_us(event_time_utc_us))"
    rows, bad_date, no_utc = con.execute(
        "SELECT count(*), count(*) FILTER (WHERE year(ts) <> year OR month(ts) <> month OR day(ts) <> day), "
        f"count(*) FILTER (WHERE ts IS NULL) "
        f"FROM (SELECT {utc} AS ts, year, month, day FROM {relation})"
    ).fetchone()
    if no_utc:
        raise CompactError(f"{partition.directory()}: {no_utc} rows in dated files have no UTC time")
    if bad_date:
        raise CompactError(f"{partition.directory()}: {bad_date} rows have a UTC date that differs from the path")
    keys = ", ".join(ROW_KEY)
    duplicates = con.execute(
        f"SELECT count(*) FROM (SELECT {keys} FROM {relation} GROUP BY ALL HAVING count(*) > 1)"
    ).fetchone()[0]
    if duplicates:
        raise CompactError(f"{partition.directory()}: {duplicates} (device, boot, sequence) keys repeat")
    return rows


def content_check(con: duckdb.DuckDBPyConnection, left: str, right: str, names: list[str]) -> dict:
    """Row count, multiset hash and exact EXCEPT ALL both ways of two relations."""
    cols = ", ".join(quote_ident(n) for n in names)
    digest = f"SELECT count(*), coalesce(sum(hash({cols})::HUGEINT), 0)::VARCHAR FROM "
    left_rows, left_hash = con.execute(digest + f"(SELECT {cols} FROM {left})").fetchone()
    right_rows, right_hash = con.execute(digest + f"(SELECT {cols} FROM {right})").fetchone()
    only_left = con.execute(f"SELECT count(*) FROM (SELECT {cols} FROM {left} EXCEPT ALL SELECT {cols} FROM {right})").fetchone()[0]
    only_right = con.execute(f"SELECT count(*) FROM (SELECT {cols} FROM {right} EXCEPT ALL SELECT {cols} FROM {left})").fetchone()[0]
    return {"rows_in": left_rows, "rows_out": right_rows, "hash_in": left_hash, "hash_out": right_hash,
            "only_in": only_left, "only_out": only_right,
            "ok": left_rows == right_rows and left_hash == right_hash and only_left == 0 and only_right == 0}


def check_output_schema(path: Path, partition: Partition, columns: list[tuple[str, int]]) -> dict:
    parquet = pq.ParquetFile(path)
    meta = parquet.metadata
    ids = {f.name: int((f.metadata or {}).get(b"PARQUET:field_id", b"-1")) for f in parquet.schema_arrow}
    expected = {"thing_id": THING_FIELD_ID, **dict(columns)}
    if ids != expected:
        raise CompactError(f"{path.name}: field ids differ, got {ids}")
    written = logical_types(meta)
    if written.get("thing_id") != "FIXED_LEN_BYTE_ARRAY UUID":
        raise CompactError(f"{path.name}: thing_id is {written.get('thing_id')}, not UUID")
    for source in partition.sources:
        for name, kind in source.logical.items():
            if written.get(name) != kind:
                raise CompactError(f"{path.name}: column {name} is {written.get(name)}, source {source.rel} has {kind}")
    for name in ("event_time_utc_us", "clock_anchor_utc_us"):
        kind = written.get(name)
        if kind is not None and ("isAdjustedToUTC=true" not in kind or "microseconds" not in kind):
            raise CompactError(f"{path.name}: {name} is {kind}, not TIMESTAMP(MICROS, UTC)")
    # PyArrow names the LZ4_RAW codec "LZ4", so read the codec names DuckDB
    # takes from the footer, which keep LZ4_RAW apart from the Hadoop LZ4.
    codecs = {row[0] for row in duckdb.execute(
        "SELECT DISTINCT compression FROM parquet_metadata(?)", [str(path)]).fetchall()}
    if codecs != {"LZ4_RAW"}:
        raise CompactError(f"{path.name}: codecs {sorted(codecs)}, expected LZ4_RAW only")
    if meta.format_version == "1.0":
        raise CompactError(f"{path.name}: format version 1.0, expected Parquet V2")
    return {"row_groups": meta.num_row_groups, "columns": meta.num_columns,
            "format_version": meta.format_version, "codecs": sorted(codecs)}


def compact(input_root: Path, output_root: Path, by_day: bool = False) -> dict:
    input_root, output_root = input_root.resolve(), output_root.resolve()
    if output_root == input_root or input_root in output_root.parents or output_root in input_root.parents:
        raise CompactError("the output prefix must be outside the input tree and must not contain it")
    started = time.perf_counter()
    sources, skipped = discover(input_root)
    discover_seconds = time.perf_counter() - started
    if not sources:
        reasons = collections.Counter(item["reason"] for item in skipped)
        raise CompactError(f"no dated Thing file with field ids under the input root, skipped {dict(reasons)}")
    partitions = plan(sources, by_day)
    run_digest = source_digest(sources)
    manifest_dir = output_root / "_manifests"
    manifest_path = manifest_dir / f"compaction_{run_digest[:16]}.json"
    if manifest_path.exists():
        raise CompactError(f"{manifest_path} exists, this source set was compacted already")
    staging = output_root / f".staging-{os.getpid()}-{run_digest[:8]}"
    staging.mkdir(parents=True)
    con = duckdb.connect()
    outputs = []
    try:
        for partition in partitions:
            outputs.append(write_partition(con, partition, output_root, staging))
        changed = [s.rel for s in sources if sha256_file(s.path) != s.sha256]
        if changed:
            raise CompactError(f"source files changed during the run: {changed[:5]}")
        # Publish only when every partition passed, so a failed run leaves no
        # compacted file behind. A hard link fails when the name exists, so a
        # published file is never overwritten.
        for output in outputs:
            final = output_root / output["path"]
            final.parent.mkdir(parents=True, exist_ok=True)
            os.link(output.pop("staged"), final)
    finally:
        con.close()
        shutil.rmtree(staging, ignore_errors=True)
    report = {
        "tool": TOOL_ID,
        "created_utc": dt.datetime.now(dt.UTC).strftime("%Y-%m-%dT%H:%M:%SZ"),
        "partition": "day" if by_day else "month",
        "input_root": str(input_root),
        "sources_sha256": run_digest,
        "sources": [{"path": s.rel, "bytes": s.size, "rows": s.rows, "sha256": s.sha256} for s in sources],
        "skipped": skipped,
        "outputs": outputs,
        "totals": {
            "source_files": len(sources), "source_bytes": sum(s.size for s in sources),
            "rows": sum(s.rows for s in sources), "skipped_files": len(skipped),
            "output_files": len(outputs), "output_bytes": sum(o["bytes"] for o in outputs),
            "discover_seconds": round(discover_seconds, 3), "seconds": round(time.perf_counter() - started, 3),
        },
        "sources_unchanged": True,
    }
    manifest_dir.mkdir(parents=True, exist_ok=True)
    with manifest_path.open("x", encoding="utf-8") as handle:
        json.dump(report, handle, indent=1)
        handle.write("\n")
    report["manifest"] = str(manifest_path)
    return report


def write_partition(con: duckdb.DuckDBPyConnection, partition: Partition, output_root: Path, staging: Path) -> dict:
    columns = columns_of(partition)
    names = [n for n, _ in columns]
    paths = [s.path for s in partition.sources]
    relation = source_relation(paths)
    started = time.perf_counter()
    rows = check_input(con, relation, partition)
    input_seconds = time.perf_counter() - started
    digest = source_digest(partition.sources)
    name = f"compact_{digest[:16]}.parquet"
    final = output_root / partition.directory() / name
    if final.exists():
        raise CompactError(f"{final} exists, output files are never overwritten")
    staged = staging / partition.directory() / name
    staged.parent.mkdir(parents=True, exist_ok=True)
    present = set(names)
    order = ", ".join(quote_ident(k) for k in SORT_KEYS if k == "thing_id" or k in present)
    provenance = {
        "tool": TOOL_ID, "board": partition.board, "partition": partition.directory(),
        "sources": len(partition.sources), "rows": rows, "sources_sha256": digest,
        "sort": [k for k in SORT_KEYS if k == "thing_id" or k in present],
        "host_field_ids": {"thing_id": THING_FIELD_ID},
        "dictionaries": sorted({json.dumps(s.dictionary, sort_keys=True) for s in partition.sources}),
        "templates": sorted({json.dumps(s.template, sort_keys=True) for s in partition.sources}),
    }
    provenance["dictionaries"] = [json.loads(d) for d in provenance["dictionaries"]]
    provenance["templates"] = [json.loads(t) for t in provenance["templates"]]
    kv = {"opensensor_compaction": json.dumps(provenance, separators=(",", ":"), sort_keys=True)}
    options = [
        "FORMAT parquet", "COMPRESSION lz4_raw", "PARQUET_VERSION v2", f"ROW_GROUP_SIZE {ROW_GROUP_SIZE}",
        "FIELD_IDS {" + ", ".join([f"thing_id: {THING_FIELD_ID}"] + [f"{quote_ident(n)}: {i}" for n, i in columns]) + "}",
    ]
    if "geometry" in present:
        kv["geo"] = geo_value(bbox_union(partition.sources))
        options.append("GEOPARQUET_VERSION 'V2'")
    options.append("KV_METADATA {" + ", ".join(f"{quote_ident(k)}: {quote_text(v)}" for k, v in kv.items()) + "}")
    started = time.perf_counter()
    con.execute(f"COPY (SELECT {select_list(columns, 'thing')} FROM {relation} ORDER BY {order}) "
            f"TO {quote_text(str(staged))} ({', '.join(options)})")
    seconds = time.perf_counter() - started
    started = time.perf_counter()
    written = f"read_parquet({quote_text(str(staged))})"
    check = content_check(con, f"(SELECT {select_list(columns, 'thing')} FROM {relation})", written,
                          ["thing_id"] + names)
    if not check["ok"]:
        raise CompactError(f"{partition.directory()}: content differs {check}")
    schema = check_output_schema(staged, partition, columns)
    check_seconds = time.perf_counter() - started
    return {
        "path": final.relative_to(output_root).as_posix(), "staged": staged, "bytes": staged.stat().st_size,
        "rows": rows, "sha256": sha256_file(staged), "sources": len(partition.sources),
        "source_bytes": sum(s.size for s in partition.sources), "input_check_seconds": round(input_seconds, 3),
        "copy_seconds": round(seconds, 3), "output_check_seconds": round(check_seconds, 3),
        "content": check, "schema": schema,
    }


def verify(input_root: Path, manifest_path: Path) -> dict:
    """Rechecks a finished run against its manifest and the raw tree."""
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    output_root = manifest_path.parent.parent
    problems = []
    for entry in manifest["sources"]:
        path = input_root / entry["path"]
        if not path.is_file() or sha256_file(path) != entry["sha256"]:
            problems.append(f"source {entry['path']} is missing or changed")
    sources, _ = discover(input_root)
    known = {e["path"] for e in manifest["sources"]}
    sources = [s for s in sources if s.rel in known]
    partitions = {p.directory(): p for p in plan(sources, manifest["partition"] == "day")}
    con = duckdb.connect()
    try:
        for output in manifest["outputs"]:
            path = output_root / output["path"]
            if not path.is_file() or sha256_file(path) != output["sha256"]:
                problems.append(f"output {output['path']} is missing or changed")
                continue
            partition = partitions.get(str(Path(output["path"]).parent.as_posix()))
            if partition is None:
                problems.append(f"output {output['path']} has no source partition")
                continue
            columns = columns_of(partition)
            relation = source_relation([s.path for s in partition.sources])
            check = content_check(con, f"(SELECT {select_list(columns, 'thing')} FROM {relation})",
                                  f"read_parquet({quote_text(str(path))})", ["thing_id"] + [n for n, _ in columns])
            if not check["ok"] or check["rows_out"] != output["rows"]:
                problems.append(f"output {output['path']} content differs {check}")
    finally:
        con.close()
    return {"manifest": str(manifest_path), "outputs": len(manifest["outputs"]),
            "sources": len(manifest["sources"]), "problems": problems, "ok": not problems}


def file_system_counts(con: duckdb.DuckDBPyConnection) -> dict:
    rows = con.execute("SELECT op, count(*), coalesce(sum(bytes), 0) FROM duckdb_logs_parsed('FileSystem') "
                   "GROUP BY ALL").fetchall()
    counts = {op.lower(): {"calls": calls, "bytes": int(size)} for op, calls, size in rows}
    return {"opens": counts.get("open", {}).get("calls", 0), "reads": counts.get("read", {}).get("calls", 0),
            "read_bytes": counts.get("read", {}).get("bytes", 0)}


def timed(con: duckdb.DuckDBPyConnection, query: str, runs: int) -> dict:
    answer = sorted(con.execute(query).fetchall(), key=repr)  # also warms the OS cache for every reader
    con.execute("CALL truncate_duckdb_logs()")
    con.execute("CALL enable_logging('FileSystem')")
    con.execute(query).fetchall()
    con.execute("CALL disable_logging()")
    counts = file_system_counts(con)
    con.execute("CALL truncate_duckdb_logs()")
    times = []
    for _ in range(runs):
        started = time.perf_counter()
        con.execute(query).fetchall()
        times.append(time.perf_counter() - started)
    return {"median_ms": round(statistics.median(times) * 1000, 2), **counts,
            "answer_sha256": hashlib.sha256(repr(answer).encode()).hexdigest()[:16]}


def measure(input_root: Path, output_root: Path, runs: int = 5) -> dict:
    """Reader benchmark of the raw dated tree against the compacted prefix."""
    sources, _ = discover(input_root.resolve())
    if not sources:
        raise CompactError("no dated Thing file with field ids under the input root")
    compacted = sorted(p for p in output_root.resolve().rglob("compact_*.parquet") if "/." not in p.as_posix())
    if not compacted:
        raise CompactError("no compacted file under the output root")
    first = min(sources, key=lambda s: (s.thing, s.year, s.month, s.day))
    day = dt.date(first.year, first.month, first.day)
    cell = None
    con = duckdb.connect()
    con.execute("SET enable_object_cache=false")
    raw = (f"(SELECT thing AS thing_id, * EXCLUDE (thing) FROM {source_relation([s.path for s in sources])})")
    by_day = {"/day=" in p.as_posix() for p in compacted}
    if len(by_day) != 1:
        raise CompactError("the output root mixes month and day partitions, measure one layout at a time")
    keys = "'board': VARCHAR, 'year': INTEGER, 'month': INTEGER" + (", 'day': INTEGER" if by_day.pop() else "")
    out = (f"read_parquet({sql_list(compacted)}, hive_partitioning=true, hive_types={{{keys}}}, "
           "union_by_name=true)")
    if any("h3_cell_id" in s.field_ids for s in sources):
        cell = con.execute(f"SELECT h3_cell_id FROM {out} WHERE h3_cell_id IS NOT NULL LIMIT 1").fetchone()
        cell = cell[0] if cell else None
    start, end = f"TIMESTAMPTZ '{day.isoformat()} 00:00:00+00'", f"TIMESTAMPTZ '{(day + dt.timedelta(days=1)).isoformat()} 00:00:00+00'"
    day_filter = f"event_time_utc_us >= {start} AND event_time_utc_us < {end}"
    queries = {
        "fleet_summary": ("SELECT thing_id, count(*), epoch_us(min(event_time_utc_us)), "
                          "epoch_us(max(event_time_utc_us)) FROM {rel} GROUP BY thing_id"),
        "one_thing_one_day": (f"SELECT count(*), max(sequence) FROM {{rel}} WHERE thing_id = '{first.thing}'::UUID "
                              f"AND {day_filter}"),
    }
    if cell is not None:
        queries["one_h3_cell"] = f"SELECT count(*) FROM {{rel}} WHERE h3_cell_id = {cell}"
    # Two more raw readers for the one Thing, one day question. A glob with
    # hive pruning and no union_by_name opens only the matching folders, but it
    # binds the schema of the first file it finds, so it suits one board only.
    # The direct path is a reader that already knows the folder. Both keep to
    # the files the compactor took, by name, since a folder can also hold files
    # it skipped.
    folder = (input_root.resolve() / f"thing={first.thing}" / f"year={day.year:04d}" / f"month={day.month:02d}"
              / f"day={day.day:02d}")
    taken = sql_list([s.path for s in sources if s.path.parent == folder])
    glob = (f"read_parquet({quote_text(str(input_root.resolve()) + '/thing=*/year=*/month=*/day=*/*.parquet')}, "
            f"hive_partitioning=true, hive_types={HIVE_TYPES}, filename=true)")
    direct = (f"read_parquet({quote_text(str(folder) + '/*.parquet')}, hive_partitioning=true, "
              f"hive_types={HIVE_TYPES}, filename=true)")
    extra = {
        "raw_glob_pruned": (f"SELECT count(*), max(sequence) FROM {glob} WHERE thing = '{first.thing}'::UUID "
                            f"AND year = {day.year} AND month = {day.month} AND day = {day.day} "
                            f"AND filename IN {taken}"),
        "raw_day_folder": f"SELECT count(*), max(sequence) FROM {direct} WHERE filename IN {taken}",
    }
    result = {"raw_files": len(sources), "raw_bytes": sum(s.size for s in sources),
              "compacted_files": len(compacted), "compacted_bytes": sum(p.stat().st_size for p in compacted),
              "rows": sum(s.rows for s in sources), "runs": runs, "queries": {}}
    try:
        for label, template in queries.items():
            result["queries"][label] = {"raw": timed(con, template.format(rel=raw), runs),
                                        "compacted": timed(con, template.format(rel=out), runs)}
        for label, query in extra.items():
            result["queries"]["one_thing_one_day"][label] = timed(con, query, runs)
    finally:
        con.close()
    for label, entry in result["queries"].items():
        answers = {reader["answer_sha256"] for reader in entry.values()}
        if len(answers) != 1:
            raise CompactError(f"{label}: readers disagree {entry}")
    return result


def ducklake(input_root: Path, scratch: Path) -> dict:
    """DuckLake comparison on byte copies of the dated sources, in scratch only."""
    sources, _ = discover(input_root.resolve())
    if not sources:
        raise CompactError("no dated Thing file with field ids under the input root")
    scratch = scratch.resolve()
    if scratch.exists() and any(scratch.iterdir()):
        raise CompactError(f"{scratch} must be empty or absent")
    copies = scratch / "raw-copy"
    for source in sources:
        target = copies / source.rel
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(source.path, target)
    con = duckdb.connect()
    con.execute(f"SET extension_directory = {quote_text(str(scratch / 'extensions'))}")
    con.execute("INSTALL ducklake")
    con.execute("LOAD ducklake")
    version = con.execute("SELECT extension_version FROM duckdb_extensions() WHERE extension_name = 'ducklake'").fetchone()[0]
    con.execute(f"ATTACH {quote_text('ducklake:' + str(scratch / 'meta.ducklake'))} AS lake "
            f"(DATA_PATH {quote_text(str(scratch / 'lake') + '/')})")
    con.execute("CALL lake.set_option('parquet_compression', 'lz4_raw')")
    con.execute("CALL lake.set_option('parquet_version', '2')")
    boards = sorted({s.board for s in sources})
    result: dict = {"ducklake_version": version, "duckdb_version": duckdb.__version__, "boards": {}}
    for board in boards:
        group = [s for s in sources if s.board == board]
        table = re.sub(r"[^a-z0-9_]", "_", board)
        partition = Partition(board, 0, 0, None, group)
        columns = columns_of(partition)
        copied = [copies / s.rel for s in group]
        entry: dict = {"files": len(group), "bytes": sum(s.size for s in group)}
        # A. Register the raw copies, then merge adjacent files.
        con.execute(f"CREATE TABLE lake.raw_{table} AS SELECT {', '.join(quote_ident(n) for n, _ in columns)} "
                f"FROM read_parquet({quote_text(str(copied[0]))}) LIMIT 0")
        started = time.perf_counter()
        # One call with the whole list. One call per file costs a snapshot each.
        con.execute(f"CALL ducklake_add_data_files('lake', 'raw_{table}', {sql_list(copied)})")
        entry["add_files_seconds"] = round(time.perf_counter() - started, 3)
        started = time.perf_counter()
        con.execute("CALL ducklake_merge_adjacent_files('lake')")
        entry["merge_seconds"] = round(time.perf_counter() - started, 3)
        entry["raw_after_merge"] = lake_files(con, f"raw_{table}")
        con.execute("CALL ducklake_expire_snapshots('lake', older_than => now() + INTERVAL 1 DAY)")
        con.execute("CALL ducklake_cleanup_old_files('lake', cleanup_all => true)")
        entry["raw_copies_left_after_cleanup"] = sum(1 for p in copied if p.exists())
        # B. Insert a sorted copy with a thing_id column, as the Hive path writes
        # it. Cleanup may have deleted the registered copies, so this reads the
        # original sources, which DuckLake never registered.
        relation = source_relation([s.path for s in group])
        started = time.perf_counter()
        con.execute(f"CREATE TABLE lake.sorted_{table} AS SELECT {select_list(columns, 'thing')} FROM {relation} "
                f"ORDER BY {', '.join(k for k in SORT_KEYS if k == 'thing_id' or k in dict(columns))}")
        entry["insert_seconds"] = round(time.perf_counter() - started, 3)
        entry["sorted"] = lake_files(con, f"sorted_{table}")
        entry["sorted_fleet_summary"] = timed(
            con, f"SELECT thing_id, count(*), epoch_us(min(event_time_utc_us)), epoch_us(max(event_time_utc_us)) "
            f"FROM lake.sorted_{table} GROUP BY thing_id", 3)
        entry["field_ids_kept"] = field_ids_kept(entry["raw_after_merge"]["paths"], dict(columns))
        entry["sorted_field_ids"] = sorted_ids(entry["sorted"]["paths"])
        result["boards"][board] = entry
    con.close()
    meta = scratch / "meta.ducklake"
    result["metadata_bytes"] = meta.stat().st_size if meta.exists() else None
    for entry in result["boards"].values():
        entry["raw_after_merge"].pop("paths")
        entry["sorted"].pop("paths")
    return result


def lake_files(con: duckdb.DuckDBPyConnection, table: str) -> dict:
    rows = con.execute(f"SELECT data_file, data_file_size_bytes FROM ducklake_list_files('lake', '{table}')").fetchall()
    return {"files": len(rows), "bytes": sum(r[1] for r in rows), "paths": [r[0] for r in rows]}


def field_ids_kept(paths: list[str], expected: dict[str, int]) -> bool:
    for path in paths:
        schema = pq.ParquetFile(path).schema_arrow
        for item in schema:
            raw = (item.metadata or {}).get(b"PARQUET:field_id")
            if item.name in expected and (raw is None or int(raw) != expected[item.name]):
                return False
    return True


def sorted_ids(paths: list[str]) -> dict[str, int]:
    if not paths:
        return {}
    schema = pq.ParquetFile(paths[0]).schema_arrow
    return {f.name: int((f.metadata or {}).get(b"PARQUET:field_id", b"-1")) for f in schema
            if f.name in ("thing_id", "schema_version", "sequence", "geometry")}


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    commands = parser.add_subparsers(dest="command", required=True)
    run = commands.add_parser("compact", help="compact a dated Hive tree into a separate prefix")
    run.add_argument("--input", type=Path, required=True)
    run.add_argument("--output", type=Path, required=True)
    run.add_argument("--partition", choices=("month", "day"), default="month")
    check = commands.add_parser("verify", help="recheck a finished run against its manifest")
    check.add_argument("--input", type=Path, required=True)
    check.add_argument("--manifest", type=Path, required=True)
    bench = commands.add_parser("measure", help="reader benchmark, raw against compacted")
    bench.add_argument("--input", type=Path, required=True)
    bench.add_argument("--output", type=Path, required=True)
    bench.add_argument("--runs", type=int, default=5)
    lake = commands.add_parser("ducklake", help="DuckLake comparison on copies in a scratch directory")
    lake.add_argument("--input", type=Path, required=True)
    lake.add_argument("--scratch", type=Path, required=True)
    args = parser.parse_args(argv)
    try:
        if args.command == "compact":
            report = compact(args.input, args.output, by_day=args.partition == "day")
            report.pop("sources")
        elif args.command == "verify":
            report = verify(args.input.resolve(), args.manifest.resolve())
        elif args.command == "measure":
            report = measure(args.input, args.output, args.runs)
        else:
            report = ducklake(args.input, args.scratch)
    except CompactError as error:
        print(f"compact_archive: {error}", file=sys.stderr)
        return 1
    print(json.dumps(report, indent=1, default=str))
    return 0 if report.get("ok", True) else 1


if __name__ == "__main__":
    sys.exit(main())
