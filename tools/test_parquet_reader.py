"""Checks for the bounded Parquet footer reader, its Iceberg DataFile metrics and
the column page decoder.

Run with ``pixi run reader-test`` (ASan + UBSan) or
``pixi run python tools/test_parquet_reader.py``.

1. Files from the firmware writer (1, 3 and 8 row groups, LZ4_RAW and
   UNCOMPRESSED, nulls, NaN, signed zeros) and from PyArrow (dictionary pages,
   ZSTD, nullable timestamps) and the GEOMETRY and UINT8 file of
   tools/geo_fixture.cpp, with and without locations, are decoded by the C++
   reader. Schema, field ids,
   chunk layout and statistics must match PyArrow and DuckDB, and the Iceberg
   metrics must match values recomputed from the rows themselves.
2. Hand-built footers cover the limits and the hostile shapes: oversized
   bool lists and maps, field id delta wrap, deep nesting, varint overflow,
   97 columns, 9 row groups, missing statistics and field ids, inconsistent
   counts and unsupported annotations. Every run has a timeout.
3. A seeded mutation loop corrupts and truncates real footers.
4. parquet_reader.cpp and parquet_columns.cpp must compile with every stack
   frame under 1 KB.
5. The page decoder (parquet_columns.h) reads every INT32, INT64 and FLOAT
   column of the writer files and the plain PyArrow files back, and each row,
   null and float bit pattern must match PyArrow and DuckDB. Dictionary pages
   and ZSTD are refused. Hand-built pages cover every error, and a seeded
   loop corrupts and truncates real pages and chunks.
6. DELTA_BINARY_PACKED INT32 and INT64 pages (#229) decode to the PyArrow and
   DuckDB rows and to the same rows as the PLAIN twin. That covers the writer
   files, every edge and real shape of tools/delta_fixture.cpp, PyArrow's own
   DELTA pages and a CrowPanel shaped file whose monotonic_us, co2_ppm and
   sd_used_bytes the chart history restore reads. Hand-built streams cover
   the spec examples and every malformed header, width and count, and the
   mutation loop runs over DELTA files too.
"""

from __future__ import annotations

import argparse
import json
import math
from pathlib import Path
import struct
import subprocess
import sys
import tempfile

import duckdb
import pyarrow as pa
import pyarrow.parquet as pq

sys.path.insert(0, str(Path(__file__).resolve().parent))
import parquet_footer as footer  # noqa: E402
from host_cxx import CXX_STD  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]
TIMEOUT = 20
PACK = {"INT32": "<i", "INT64": "<q", "FLOAT": "<f"}
CODECS = {"UNCOMPRESSED": 0, "SNAPPY": 1, "GZIP": 2, "BROTLI": 4, "ZSTD": 6, "LZ4": 7}


def require(condition: bool, description: str) -> None:
    if not condition:
        raise AssertionError(description)


class Tools:
    def __init__(self, directory: Path, sanitize: bool) -> None:
        self.reader = directory / "parquet_reader_fixture"
        self.writer = directory / "parquet_fixture"
        base = ["clang++", CXX_STD, "-Wall", "-Wextra", "-Werror", "-g"]
        if sanitize:
            base += ["-fsanitize=address,undefined", "-fno-sanitize-recover=all"]
        source = ROOT / "firmware/common/src"
        subprocess.run(base + [str(ROOT / "tools/parquet_reader_fixture.cpp"),
                               str(source / "parquet_reader.cpp"), str(source / "parquet_columns.cpp"),
                               str(source / "lz4_codec.cpp"), "-o", str(self.reader)], check=True)
        self.geo = directory / "geo_fixture"
        common = ROOT / "firmware/common"
        includes = [arg for library in ("src", "runtime/src", "connectivity/src", "logger/src",
                                        "power/src", "location/src")
                    for arg in ("-I", str(common / library))]
        subprocess.run(base + includes + [str(ROOT / "tools/geo_fixture.cpp"),
                                          str(source / "parquet_writer.cpp"), "-o", str(self.geo)],
                       check=True)
        subprocess.run(base + ["-I", str(ROOT / "firmware/common/vendor/lz4"),
                               str(ROOT / "tools/parquet_fixture.cpp"), str(source / "parquet_writer.cpp"),
                               str(source / "lz4_codec.cpp"), "-o", str(self.writer)], check=True)

    def dump(self, path: Path) -> dict:
        run = subprocess.run([str(self.reader), "dump", str(path)], capture_output=True, text=True,
                             timeout=TIMEOUT)
        require(run.returncode in (0, 1), f"reader crashed or disagreed on {path.name}: "
                                          f"rc={run.returncode} {run.stderr[-2000:]}")
        return json.loads(run.stdout)

    def columns(self, path: Path) -> dict:
        run = subprocess.run([str(self.reader), "columns", str(path)], capture_output=True, text=True,
                             timeout=TIMEOUT)
        require(run.returncode == 0, f"page decoder crashed on {path.name}: "
                                     f"rc={run.returncode} {run.stderr[-2000:]}")
        return json.loads(run.stdout)


def check_stack() -> None:
    # Firmware tasks have small stacks; the reader may not grow them.
    for source in ("parquet_reader.cpp", "parquet_columns.cpp"):
        for level in ("-O0", "-O2", "-Os"):
            subprocess.run(["clang++", CXX_STD, level, "-Wall", "-Wextra", "-Werror",
                            "-Wframe-larger-than=1024", "-c", str(ROOT / "firmware/common/src" / source),
                            "-o", "/dev/null"], check=True)
    print("PASS reader and page decoder stack frames under 1024 bytes at -O0, -O2 and -Os", flush=True)


# ---------------------------------------------------------------------------
# Cross-check against PyArrow, DuckDB and the rows themselves.

def bits(value, type_name: str) -> int:
    return int.from_bytes(struct.pack(PACK[type_name], value), "little")


def expected_metrics(path: Path, meta: dict) -> dict:
    """Iceberg metrics recomputed from the decoded rows and a separate footer parse."""
    parquet = pq.ParquetFile(path)
    table = parquet.read()
    metadata = parquet.metadata
    columns = []
    for c, field in enumerate(table.schema):
        values = table.column(c)
        type_name = metadata.schema.column(c).physical_type
        if pa.types.is_timestamp(field.type):
            values = values.cast(pa.int64())
        raw = values.to_pylist()
        present = [v for v in raw if v is not None]
        nans = sum(1 for v in present if isinstance(v, float) and math.isnan(v))
        finite = [v for v in present if not (isinstance(v, float) and math.isnan(v))]
        if type_name == "BYTE_ARRAY":
            finite = []  # GEOMETRY bounds live only in GeospatialStatistics
        groups = meta.get(footer.FILE_ROW_GROUPS, [])
        stats = [g[footer.GROUP_COLUMNS][c][footer.CHUNK_META].get(footer.META_STATISTICS, {}) for g in groups]
        has_nan = type_name == "FLOAT" and all(footer.STAT_NANS in s for s in stats)
        has_null = all(footer.STAT_NULLS in s for s in stats)
        lower = upper = None
        if finite:
            low, high = min(finite), max(finite)
            if type_name == "FLOAT":
                low = -0.0 if low == 0 else low
                high = 0.0 if high == 0 else high
            lower = struct.pack(PACK[type_name], low).hex()
            upper = struct.pack(PACK[type_name], high).hex()
        columns.append({
            "field_id": int(field.metadata[b"PARQUET:field_id"]),
            "column_size": sum(metadata.row_group(g).column(c).total_compressed_size
                               for g in range(metadata.num_row_groups)),
            "value_count": len(raw),
            "null_value_count": len(raw) - len(present) if has_null else None,
            "nan_value_count": nans if has_nan else None,
            "lower_bound": lower,
            "upper_bound": upper,
        })
    splits = []
    for g in range(metadata.num_row_groups):
        group = metadata.row_group(g)
        starts = []
        for c in range(group.num_columns):
            chunk = group.column(c)
            start = chunk.data_page_offset
            if chunk.dictionary_page_offset is not None:
                start = min(start, chunk.dictionary_page_offset)
            starts.append(start)
        splits.append(min(starts))
    return {"record_count": metadata.num_rows, "file_size_in_bytes": path.stat().st_size,
            "split_offsets": splits, "columns": columns}


def check_against_pyarrow(path: Path, got: dict) -> None:
    parquet = pq.ParquetFile(path)
    metadata = parquet.metadata
    require(got["num_rows"] == metadata.num_rows, "num_rows")
    require(got["file_size"] == path.stat().st_size, "file_size")
    require(len(got["columns"]) == metadata.num_columns, "column count")
    require(len(got["row_groups"]) == metadata.num_row_groups, "row group count")
    for c, (column, field) in enumerate(zip(got["columns"], parquet.schema_arrow, strict=True)):
        leaf = metadata.schema.column(c)
        require(column["name"] == leaf.name == field.name, f"name {c}")
        require(column["type"] == leaf.physical_type, f"physical type {leaf.name}")
        require(column["nullable"] == (leaf.max_definition_level == 1), f"nullable {leaf.name}")
        require(column["field_id"] == int(field.metadata[b"PARQUET:field_id"]), f"field_id {leaf.name}")
        timestamp = leaf.logical_type.type == "TIMESTAMP"
        require(column["timestamp_micros_utc"] == timestamp, f"logical type {leaf.name}")
        logical = ("timestamp_micros_utc" if timestamp else
                   {"None": "none", "Int(bitWidth=8, isSigned=false)": "uint8",
                    "Geometry(crs=)": "geometry"}.get(str(leaf.logical_type)))
        require(column["logical"] == logical, f"logical {leaf.logical_type} {leaf.name}")
        if timestamp:
            require(field.type == pa.timestamp("us", tz="UTC"), f"timestamp unit {leaf.name}")
    for g, group in enumerate(got["row_groups"]):
        expected = metadata.row_group(g)
        require(group["num_rows"] == expected.num_rows, f"group {g} rows")
        for c, chunk in enumerate(group["columns"]):
            meta = expected.column(c)
            name = f"group {g} column {meta.path_in_schema}"
            require(chunk["codec"] == CODECS[meta.compression], f"{name} codec")
            require(chunk["num_values"] == meta.num_values, f"{name} num_values")
            require(chunk["total_compressed_size"] == meta.total_compressed_size, f"{name} compressed size")
            require(chunk["total_uncompressed_size"] == meta.total_uncompressed_size, f"{name} size")
            require(chunk["data_page_offset"] == meta.data_page_offset, f"{name} data page")
            start = meta.data_page_offset if meta.dictionary_page_offset is None else meta.dictionary_page_offset
            require(chunk["start_offset"] == start, f"{name} start offset")
            stats = meta.statistics
            require(chunk["null_count"] == (stats.null_count if stats is not None and stats.has_null_count else None),
                    f"{name} null_count")
            if stats is not None and stats.has_min_max:
                require(chunk["min"] == bits(stats.min_raw, stats.physical_type), f"{name} min")
                require(chunk["max"] == bits(stats.max_raw, stats.physical_type), f"{name} max")
            else:
                require(chunk["min"] is None and chunk["max"] is None, f"{name} no bounds")


def check_against_duckdb(path: Path, got: dict) -> None:
    connection = duckdb.connect()
    ids = connection.execute(
        "SELECT name, field_id FROM parquet_schema(?) WHERE num_children IS NULL", [str(path)]).fetchall()
    require([(c["name"], c["field_id"]) for c in got["columns"]] == ids, "DuckDB field ids")
    rows = connection.execute(
        "SELECT row_group_id, column_id, num_values, total_compressed_size, data_page_offset, "
        "stats_null_count, stats_min_value, stats_max_value FROM parquet_metadata(?) "
        "ORDER BY row_group_id, column_id", [str(path)]).fetchall()
    require(len(rows) == len(got["row_groups"]) * len(got["columns"]), "DuckDB chunk count")
    for g, c, values, size, page, nulls, low, high in rows:
        chunk = got["row_groups"][g]["columns"][c]
        column = got["columns"][c]
        name = f"DuckDB group {g} column {column['name']}"
        require((chunk["num_values"], chunk["total_compressed_size"], chunk["data_page_offset"])
                == (values, size, page), f"{name} layout")
        require(chunk["null_count"] == nulls, f"{name} null_count")
        if chunk["min"] is None:
            require(low is None and high is None, f"{name} no bounds")
            continue
        if column["type"] == "FLOAT" and low is None and high is None:
            # DuckDB withholds FLOAT statistics for chunks holding NaN or an
            # infinity; PyArrow and the rows still check those bounds.
            continue
        for value, actual in ((low, chunk["min"]), (high, chunk["max"])):
            if column["type"] == "FLOAT":
                require(bits(float(value), "FLOAT") == actual, f"{name} float bound")
            elif not column["timestamp_micros_utc"]:
                require(bits(int(value), column["type"]) == actual, f"{name} integer bound")


def check_file(tools: Tools, path: Path, label: str) -> dict:
    got = tools.dump(path)
    require("error" not in got, f"{label}: reader refused a valid file: {got.get('error')}")
    check_against_pyarrow(path, got)
    check_against_duckdb(path, got)
    expected = expected_metrics(path, footer.read_footer(path))
    require(got.get("iceberg") == expected, f"{label}: Iceberg metrics\n got {got.get('iceberg')}\n"
                                            f"want {expected}")
    return got


def expected_rows(values: list, type_name: str) -> list:
    """Rows as the page decoder prints them, FLOAT as its bit pattern, null as None."""
    if type_name != "FLOAT":
        return [None if v is None else int(v) for v in values]
    return [None if v is None else bits(v, "FLOAT") for v in values]


def same_rows(got: list, want: list, type_name: str) -> bool:
    if len(got) != len(want):
        return False
    for g, w in zip(got, want, strict=True):
        if (g is None) != (w is None):
            return False
        if g is None or g == w:
            continue
        # Python floats lose a NaN payload; any NaN pattern matches any NaN.
        nan = type_name == "FLOAT" and math.isnan(struct.unpack("<f", g.to_bytes(4, "little"))[0]) \
            and math.isnan(struct.unpack("<f", w.to_bytes(4, "little"))[0])
        if not nan:
            return False
    return True


def check_columns(tools: Tools, path: Path, refused: dict[str, str] | None = None) -> int:
    """Page decoder rows against PyArrow and DuckDB. `refused` maps a column to its required error."""
    refused = refused or {}
    got = {column["name"]: column for column in tools.columns(path)["columns"]}
    parquet = pq.ParquetFile(path)
    table = parquet.read()
    connection = duckdb.connect()
    decoded = 0
    for c in range(parquet.metadata.num_columns):
        leaf = parquet.metadata.schema.column(c)
        if leaf.physical_type == "BYTE_ARRAY":
            require(leaf.name not in got, f"{path.name}: BYTE_ARRAY {leaf.name} skipped")
            continue
        column = got[leaf.name]
        if leaf.name in refused:
            require(column.get("error") == refused[leaf.name],
                    f"{path.name}: {leaf.name} must be refused with {refused[leaf.name]}, got {column}")
            continue
        require("error" not in column, f"{path.name}: decoder refused {leaf.name}: {column.get('error')}")
        arrow = table.column(leaf.name)
        if pa.types.is_timestamp(arrow.type):
            arrow = arrow.cast(pa.int64())
        want = expected_rows(arrow.to_pylist(), leaf.physical_type)
        require(same_rows(column["rows"], want, leaf.physical_type),
                f"{path.name}: {leaf.name} rows differ from PyArrow")
        quoted = '"' + leaf.name.replace('"', '""') + '"'
        if pa.types.is_timestamp(table.column(leaf.name).type):
            quoted = f"epoch_us({quoted})"
        duck = [row[0] for row in connection.execute(
            f"SELECT {quoted} FROM read_parquet(?, file_row_number = true) ORDER BY file_row_number",
            [str(path)]).fetchall()]
        require(same_rows(column["rows"], expected_rows(duck, leaf.physical_type), leaf.physical_type),
                f"{path.name}: {leaf.name} rows differ from DuckDB")
        decoded += 1
    return decoded


def writer_files(tools: Tools, directory: Path) -> None:
    for rows, columns, groups in ((9, 9, 1), (9, 9, 3), (9, 9, 8), (0, 8, 1), (90, 96, 1), (90, 77, 4)):
        for codec in ("none", "lz4"):
            path = directory / f"writer-{rows}x{columns}x{groups}-{codec}.parquet"
            subprocess.run([str(tools.writer), str(path), str(rows), str(columns), codec, str(groups)],
                           check=True, stdout=subprocess.DEVNULL, timeout=TIMEOUT)
            got = check_file(tools, path, path.name)
            if rows == 9:
                metrics = got["iceberg"]["columns"]
                # temperature carries NaN, -0.0 and both infinities in group 0.
                require(metrics[2]["nan_value_count"] == 1, "NaN counted")
                require(metrics[2]["lower_bound"] == struct.pack("<f", -math.inf).hex(), "-inf lower bound")
                require(metrics[4]["lower_bound"] is None and metrics[4]["null_value_count"] == rows * groups,
                        "all-null column has no bounds")
                require((metrics[8]["lower_bound"], metrics[8]["upper_bound"]) == ("00000080", "00000000"),
                        "zero bounds widened to cover -0.0 and +0.0")
            decoded = check_columns(tools, path)
            print(f"PASS firmware writer {groups} x {rows} rows x {columns} columns {codec}: "
                  f"PyArrow + DuckDB + Iceberg metrics agree, {decoded} columns decode to the same rows",
                  flush=True)
    # DELTA_BINARY_PACKED integers (#135, no board writes it yet) decode to
    # the same rows as the PLAIN twin, FLOAT columns beside them too.
    for codec in ("none", "lz4"):
        plain = directory / f"writer-delta-plain-{codec}.parquet"
        path = directory / f"writer-delta-{codec}.parquet"
        for target, extra in ((plain, []), (path, ["delta"])):
            subprocess.run([str(tools.writer), str(target), "90", "9", codec, "2", *extra],
                           check=True, stdout=subprocess.DEVNULL, timeout=TIMEOUT)
        integers = [leaf.name for leaf in pq.ParquetFile(path).schema if leaf.physical_type in ("INT32", "INT64")]
        require(len(integers) >= 4, "delta file has integer columns")
        require(delta_columns(path) == set(integers), f"{path.name}: integer chunks list DELTA_BINARY_PACKED")
        decoded = check_columns(tools, path)
        require(decoded == check_columns(tools, plain), "delta file decodes every column the PLAIN file does")
        require(tools.columns(path) == tools.columns(plain), f"{path.name}: rows equal the PLAIN twin")
    print("PASS DELTA_BINARY_PACKED writer files decode to the PyArrow and DuckDB rows and equal the PLAIN twin",
          flush=True)


def delta_columns(path: Path) -> set[str]:
    """Columns whose chunk lists DELTA_BINARY_PACKED."""
    meta = pq.ParquetFile(path).metadata
    return {meta.schema.column(c).name for g in range(meta.num_row_groups) for c in range(meta.num_columns)
            if "DELTA_BINARY_PACKED" in meta.row_group(g).column(c).encodings}


def delta_files(tools: Tools, directory: Path, sanitize: bool) -> None:
    """Every DELTA shape the writer makes, PyArrow DELTA pages and a CrowPanel file."""
    flags = [CXX_STD, "-Wall", "-Wextra", "-Werror", "-g", "-I", str(ROOT / "firmware/common/vendor/lz4")]
    if sanitize:
        flags += ["-fsanitize=address,undefined", "-fno-sanitize-recover=all"]
    source = ROOT / "firmware/common/src"
    fixture = directory / "delta_fixture"
    subprocess.run(["clang++", *flags, str(ROOT / "tools/delta_fixture.cpp"), str(source / "parquet_writer.cpp"),
                    str(source / "lz4_codec.cpp"), "-o", str(fixture)], check=True)
    out = directory / "delta-shapes"
    out.mkdir()
    subprocess.run([str(fixture), str(out)], check=True, stdout=subprocess.DEVNULL, timeout=120)
    deltas = sorted(out.glob("*-delta-*.parquet"))
    require(len(deltas) == 26, f"delta fixture wrote 26 DELTA files, got {len(deltas)}")
    for path in deltas:
        plain = path.with_name(path.name.replace("-delta-", "-plain-"))
        require(delta_columns(path), f"{path.name}: has DELTA chunks")
        check_columns(tools, path)
        require(tools.columns(path) == tools.columns(plain), f"{path.name}: rows equal the PLAIN twin")
    print(f"PASS DELTA_BINARY_PACKED edge and real shapes: {len(deltas)} files (wrap, full 64 bit width, zero "
          "width miniblocks, nulls, all null, 1 to 1000 rows) decode to the PyArrow, DuckDB and PLAIN rows",
          flush=True)

    # PyArrow's own DELTA writer, several pages per chunk and both codecs.
    rows = 1000
    table = pa.table({
        "t": pa.array([1_700_000_000_000_000 + i * 10_000_000 + (i * 7919 % 13) if i % 17 else None
                       for i in range(rows)], pa.int64()),
        "wide": pa.array([(-(2**63) if i % 2 else 2**63 - 1) for i in range(rows)], pa.int64()),
        "small": pa.array([(i * 2654435761) % 4001 - 2000 for i in range(rows)], pa.int32()),
        "edge32": pa.array([(-(2**31) if i % 3 == 0 else 2**31 - 1) if i % 5 else None for i in range(rows)],
                           pa.int32()),
        "flat": pa.array([42] * rows, pa.int32()),
        "f": pa.array([i * 0.25 for i in range(rows)], pa.float32()),
    })
    encodings = dict.fromkeys(("t", "wide", "small", "edge32", "flat"), "DELTA_BINARY_PACKED")
    arrow = 0
    for compression in ("NONE", "LZ4"):
        # 200 rows a group keeps the file within the reader's 8 row groups and
        # still crosses a 128 value block inside each chunk.
        for group_rows in (rows, 200):
            path = directory / f"pyarrow-delta-{compression}-{group_rows}.parquet"
            pq.write_table(table, path, compression=compression, use_dictionary=False,
                           column_encoding=encodings, row_group_size=group_rows, data_page_size=512)
            require(delta_columns(path) == set(encodings), f"{path.name}: PyArrow wrote DELTA chunks")
            check_columns(tools, path)
            arrow += 1
    print(f"PASS PyArrow DELTA_BINARY_PACKED files: {arrow} files, many pages per chunk, decode to the PyArrow "
          "and DuckDB rows", flush=True)

    # A CrowPanel shaped file, as the chart history restore reads it.
    board = directory / "crowpanel_fixture"
    inc = ROOT / "firmware/esp-idf-crowpanel-epaper/logger/telemetry_fields.inc"
    subprocess.run(["clang++", *flags, f'-DOS_FIELDS_INC="{inc}"', str(ROOT / "tools/partial_rebuild_fixture.cpp"),
                    str(source / "parquet_writer.cpp"), str(source / "lz4_codec.cpp"), "-o", str(board)],
                   check=True)
    for codec in ("none", "lz4"):
        files = {}
        for encoding in ("plain", "delta"):
            path = directory / f"crowpanel-{encoding}-{codec}.parquet"
            extra = ["delta"] if encoding == "delta" else []
            subprocess.run([str(board), str(path), codec, "8", "90", *extra], check=True, stdout=subprocess.DEVNULL,
                           timeout=TIMEOUT)
            files[encoding] = path
        path = files["delta"]
        require({"monotonic_us", "co2_ppm", "sd_used_bytes"} <= delta_columns(path),
                f"{path.name}: restore columns are DELTA")
        check_columns(tools, path)
        rows_delta = {c["name"]: c for c in tools.columns(path)["columns"]}
        rows_plain = {c["name"]: c for c in tools.columns(files["plain"])["columns"]}
        require(rows_delta == rows_plain, f"{path.name}: rows equal the PLAIN twin")
        require(len(rows_delta["monotonic_us"]["rows"]) == 720 and None not in rows_delta["monotonic_us"]["rows"],
                "monotonic_us decodes every row")
        # used_range() reads sd_used_bytes from the chunk statistics, which
        # stay PLAIN whatever the page encoding.
        stats = [tools.dump(files[e])["row_groups"] for e in ("plain", "delta")]
        index = [c["name"] for c in tools.dump(path)["columns"]].index("sd_used_bytes")
        require([(g["columns"][index]["min"], g["columns"][index]["max"]) for g in stats[0]] ==
                [(g["columns"][index]["min"], g["columns"][index]["max"]) for g in stats[1]],
                "sd_used_bytes statistics equal the PLAIN twin")
    print("PASS CrowPanel shaped DELTA files (8 x 90 rows, both codecs): monotonic_us, co2_ppm and every other "
          "column decode to the PLAIN rows, sd_used_bytes statistics are unchanged", flush=True)


def geo_files(tools: Tools, directory: Path) -> None:
    """location_source UINT8 and geometry GEOMETRY from the firmware writer."""
    for mode in ("with", "none"):
        path = directory / f"geo-{mode}.parquet"
        subprocess.run([str(tools.geo), mode, str(path)], check=True, stdout=subprocess.DEVNULL,
                       timeout=TIMEOUT)
        got = check_file(tools, path, path.name)
        kinds = {c["name"]: (c["type"], c["logical"]) for c in got["columns"]}
        require(kinds["location_source"] == ("INT32", "uint8") and
                kinds["geometry"] == ("BYTE_ARRAY", "geometry"), f"geo logical types {kinds}")
        index = [c["name"] for c in got["columns"]].index("geometry")
        require(all(g["columns"][index]["min"] is None for g in got["row_groups"]) and
                got["iceberg"]["columns"][index]["lower_bound"] is None,
                "GEOMETRY has no ordinary bounds")
        print(f"PASS firmware writer GEOMETRY and UINT8 {mode} locations: "
              "PyArrow + DuckDB + Iceberg metrics agree", flush=True)


def pyarrow_files(tools: Tools, directory: Path) -> None:
    def field(name: str, kind: pa.DataType, field_id: int) -> pa.Field:
        return pa.field(name, kind, metadata={b"PARQUET:field_id": str(field_id).encode()})

    schema = pa.schema([
        field("event_time_utc_us", pa.timestamp("us", tz="UTC"), 1),
        field("pm2_5", pa.float32(), 7),
        field("count", pa.int32(), 3),
        field("total", pa.int64(), 40),
        field("zero", pa.float32(), 5),
    ])
    rows = 11
    table = pa.table([
        pa.array([1_700_000_000_000_000 + i * 10_000_000 if i != 4 else None for i in range(rows)],
                 pa.timestamp("us", tz="UTC")),
        pa.array([None if i == 2 else math.nan if i in (3, 8) else i * 1.5 - 4 for i in range(rows)], pa.float32()),
        pa.array([i % 3 - 1 for i in range(rows)], pa.int32()),
        pa.array([None if i % 2 else -(2**62) + i for i in range(rows)], pa.int64()),
        pa.array([-0.0 if i % 2 else 0.0 for i in range(rows)], pa.float32()),
    ], schema=schema)
    for compression in ("NONE", "LZ4", "ZSTD"):
        for dictionary in (False, True):
            for group_rows in (rows, 4):
                path = directory / f"pyarrow-{compression}-{dictionary}-{group_rows}.parquet"
                pq.write_table(table, path, compression=compression, use_dictionary=dictionary,
                               row_group_size=group_rows)
                check_file(tools, path, path.name)
                # PyArrow writes bit-packed definition levels and page
                # statistics. ZSTD and dictionary pages are refused.
                names = [leaf.name for leaf in pq.ParquetFile(path).schema]
                if compression == "ZSTD":
                    refused = dict.fromkeys(names, "codec")
                elif dictionary:
                    refused = dict.fromkeys(names, "encoding")
                else:
                    refused = {}
                check_columns(tools, path, refused)
    print("PASS PyArrow-written files (dictionary pages, LZ4_RAW, ZSTD, 1 and 3 row groups), "
          "plain NONE and LZ4_RAW pages decode to the PyArrow and DuckDB rows", flush=True)
    for label, extra in (("string", pa.array(["a"] * rows)), ("list", pa.array([[1]] * rows)),
                         ("double", pa.array([1.0] * rows))):
        path = directory / f"pyarrow-{label}.parquet"
        pq.write_table(table.append_column(field(label, extra.type, 99), extra), path)
        require("error" in tools.dump(path), f"reader must refuse a {label} column")
    print("PASS PyArrow files with string, list and double leaves are refused", flush=True)


# ---------------------------------------------------------------------------
# Hand-built footers. A tiny Thrift Compact encoder writes the exact shapes.

STOP, TRUE, FALSE, BYTE, I16, I32, I64, DOUBLE, BINARY, LIST, SET, MAP, STRUCT = range(13)


def varint(value: int) -> bytes:
    out = bytearray()
    while True:
        byte = value & 0x7F
        value >>= 7
        out.append(byte | (0x80 if value else 0))
        if not value:
            return bytes(out)


def zigzag(value: int) -> bytes:
    return varint((value << 1) ^ (value >> 63) if value < 0 else value << 1)


class Struct:
    """Fields are (id, type, payload bytes); written in order with short deltas when possible."""

    def __init__(self) -> None:
        self.fields: list[tuple[int, int, bytes]] = []

    def add(self, field_id: int, kind: int, payload: bytes = b"") -> Struct:
        self.fields.append((field_id, kind, payload))
        return self

    def i32(self, field_id: int, value: int) -> Struct:
        return self.add(field_id, I32, zigzag(value))

    def i64(self, field_id: int, value: int) -> Struct:
        return self.add(field_id, I64, zigzag(value))

    def binary(self, field_id: int, value: bytes) -> Struct:
        return self.add(field_id, BINARY, varint(len(value)) + value)

    def struct(self, field_id: int, value: Struct) -> Struct:
        return self.add(field_id, STRUCT, value.encode())

    def boolean(self, field_id: int, value: bool) -> Struct:
        return self.add(field_id, TRUE if value else FALSE)

    def list(self, field_id: int, kind: int, items: list[bytes]) -> Struct:
        return self.add(field_id, LIST, list_header(kind, len(items)) + b"".join(items))

    def encode(self) -> bytes:
        out = bytearray()
        last = 0
        for field_id, kind, payload in self.fields:
            delta = field_id - last
            out += bytes([delta << 4 | kind]) if 0 < delta <= 15 else bytes([kind]) + zigzag(field_id)
            out += payload
            last = field_id
        return bytes(out + b"\x00")


def list_header(kind: int, count: int) -> bytes:
    return bytes([count << 4 | kind]) if count < 15 else bytes([0xF0 | kind]) + varint(count)


def timestamp(utc: bool = True, unit: int = 2) -> Struct:
    return Struct().struct(8, Struct().boolean(1, utc).struct(2, Struct().struct(unit, Struct())))


def build_file(columns: list[dict], groups: list[dict], *, num_rows: int | None = None,
               extra: list[tuple[int, int, bytes]] = (), schema_extra: Struct | None = None,
               trailing: bytes = b"") -> bytes:
    """columns: name, type, optional, field_id, logical, converted.
    groups: rows plus per-chunk overrides in groups[i]['chunks'][c]."""
    chunk_size = 16
    data = b"\x00" * (chunk_size * max(1, len(columns) * len(groups)))
    meta = Struct().i32(1, 1)
    root = Struct().binary(4, b"schema").i32(5, len(columns))
    leaves = [root.encode()]
    for column in columns:
        leaf = Struct().i32(1, column["type"]).i32(3, 1 if column.get("optional") else 0)
        leaf.binary(4, column["name"].encode())
        if "converted" in column:
            leaf.i32(6, column["converted"])
        if column.get("field_id") is not None:
            leaf.i32(9, column["field_id"])
        if column.get("logical") is not None:
            leaf.struct(10, column["logical"])
        leaves.append(leaf.encode())
    meta.list(2, STRUCT, leaves)
    if schema_extra is not None:
        meta.struct(2, schema_extra)
    meta.i64(3, sum(g["rows"] for g in groups) if num_rows is None else num_rows)
    encoded_groups = []
    offset = 4
    for group in groups:
        chunks = []
        for c, column in enumerate(columns):
            spec = {"null_count": 0, "min": None, "max": None, "nan_count": None}
            spec.update(group.get("chunks", {}).get(c, {}))
            stats = Struct()
            if spec["null_count"] is not None:
                stats.i64(3, spec["null_count"])
            if spec["max"] is not None:
                stats.binary(5, spec["max"])
            if spec["min"] is not None:
                stats.binary(6, spec["min"])
            if spec["nan_count"] is not None:
                stats.i64(9, spec["nan_count"])
            info = Struct().i32(1, spec.get("type", column["type"]))
            info.list(3, BINARY, [varint(len(n)) + n for n in [spec.get("path", column["name"]).encode()]])
            info.i32(4, 0).i64(5, spec.get("num_values", group["rows"])).i64(6, chunk_size)
            info.i64(7, spec.get("size", chunk_size))
            page = offset + (8 if "dictionary" in spec else 0)
            info.i64(9, page)
            if "dictionary" in spec:
                info.i64(11, offset)
            info.struct(12, stats)
            chunks.append(Struct().i64(2, 0).struct(3, info).encode())
            offset += chunk_size
        encoded_groups.append(Struct().list(1, STRUCT, chunks).i64(2, chunk_size * len(columns))
                              .i64(3, group["rows"]).encode())
    meta.list(4, STRUCT, encoded_groups)
    for field in extra:
        meta.add(*field)
    body = meta.encode() + trailing
    return b"PAR1" + data + body + struct.pack("<I", len(body)) + b"PAR1"


def f32(value: float) -> bytes:
    return struct.pack("<f", value)


def i32(value: int) -> bytes:
    return struct.pack("<i", value)


BASE_COLUMNS = [
    {"name": "event_time_utc_us", "type": 2, "field_id": 1, "logical": timestamp()},
    {"name": "pm2_5", "type": 4, "optional": True, "field_id": 2},
    {"name": "count", "type": 1, "field_id": 3},
]


def integer(width: int = 8, signed: bool = False) -> Struct:
    return Struct().struct(10, Struct().add(1, BYTE, bytes([width])).boolean(2, signed))


def geometry(crs: str | None = None) -> Struct:
    inner = Struct()
    if crs is not None:
        inner.binary(1, crs.encode())
    return Struct().struct(17, inner)


# BASE_COLUMNS plus the two location columns the boards write (#83).
GEO_COLUMNS = BASE_COLUMNS + [
    {"name": "location_source", "type": 1, "optional": True, "field_id": 4, "converted": 11,
     "logical": integer()},
    {"name": "geometry", "type": 6, "optional": True, "field_id": 5, "logical": geometry()},
]


def base_groups(count: int = 2) -> list[dict]:
    return [{"rows": 5, "chunks": {
        0: {"min": struct.pack("<q", 10 * g), "max": struct.pack("<q", 10 * g + 4)},
        1: {"null_count": 1, "nan_count": 1, "min": f32(-1.5 - g), "max": f32(2.0 + g)},
        2: {"min": i32(-g), "max": i32(g)},
    }} for g in range(count)]


def with_chunk(groups: list[dict], group: int, column: int, **changes) -> list[dict]:
    groups[group]["chunks"].setdefault(column, {}).update(changes)
    return groups


def crafted(tools: Tools, directory: Path) -> None:
    counts = {"accepted": 0, "refused": 0}

    def run(label: str, data: bytes) -> dict:
        path = directory / f"crafted-{label}.parquet"
        path.write_bytes(data)
        return tools.dump(path)

    def accept(label: str, data: bytes) -> dict:
        got = run(label, data)
        require("error" not in got, f"{label}: refused: {got.get('error')}")
        counts["accepted"] += 1
        return got

    def refuse(label: str, data: bytes) -> None:
        got = run(label, data)
        require("error" in got, f"{label}: must be refused, got {json.dumps(got)[:300]}")
        counts["refused"] += 1

    base = accept("base", build_file(BASE_COLUMNS, base_groups()))
    metrics = base["iceberg"]["columns"]
    require([c["field_id"] for c in metrics] == [1, 2, 3], "crafted field ids")
    require(metrics[0]["lower_bound"] == struct.pack("<q", 0).hex()
            and metrics[0]["upper_bound"] == struct.pack("<q", 14).hex(), "INT64 bounds merged across groups")
    require(metrics[1] == {"field_id": 2, "column_size": 32, "value_count": 10, "null_value_count": 2,
                           "nan_value_count": 2, "lower_bound": f32(-2.5).hex(), "upper_bound": f32(3.0).hex()},
            "FLOAT metrics merged across groups")
    require(base["iceberg"]["split_offsets"] == [4, 52], "split offsets")

    # Thrift structure the reader must skip correctly, then accept.
    skip_cases = {
        "bool-list": (20, LIST, list_header(TRUE, 3) + b"\x01\x02\x01"),
        "bool-set": (20, SET, list_header(TRUE, 2) + b"\x01\x01"),
        "bool-map": (21, MAP, varint(2) + bytes([TRUE << 4 | FALSE]) + b"\x01\x02\x02\x01"),
        "empty-map": (21, MAP, varint(0)),
        "nested-10": (22, STRUCT, b"\x1c" * 9 + b"\x00" * 10),
        "double": (23, DOUBLE, b"\x00" * 8),
        "far-id": (32767, I32, zigzag(1)),
    }
    for label, field in skip_cases.items():
        got = accept(label, build_file(BASE_COLUMNS, base_groups(), extra=[field]))
        same = {**got["iceberg"], "file_size_in_bytes": base["iceberg"]["file_size_in_bytes"]}
        require(same == base["iceberg"], f"{label}: unknown field changed the result")

    huge = 2**31
    refuse_cases = {
        # Containers whose declared length exceeds the bytes left.
        "huge-bool-list": [(20, LIST, list_header(TRUE, huge))],
        "huge-bool-map": [(21, MAP, varint(huge) + bytes([TRUE << 4 | TRUE]))],
        "huge-struct-list": [(20, LIST, list_header(STRUCT, 2**60))],
        "huge-binary": [(20, BINARY, varint(2**40))],
        "bad-map-types": [(21, MAP, varint(1) + b"\xd0\x00\x00")],
        # Field id arithmetic: a short delta after 32767 would wrap int16.
        "delta-wrap": [(32767, I32, zigzag(1)), (32768, I32, zigzag(1))],
        "deep-nesting": [(22, STRUCT, b"\x1c" * 99 + b"\x00" * 100)],
        "varint-overflow": [(20, I64, b"\xff" * 10 + b"\x01")],
        "bad-type": [(20, 13, b"")],
    }
    for label, fields in refuse_cases.items():
        refuse(label, build_file(BASE_COLUMNS, base_groups(), extra=fields))

    many = [{"name": f"c{i:02}", "type": 1, "field_id": i + 1} for i in range(97)]
    accept("96-columns", build_file(many[:96], [{"rows": 1}]))
    refuse("97-columns", build_file(many, [{"rows": 1}]))
    accept("8-groups", build_file(BASE_COLUMNS, base_groups(8)))
    refuse("9-groups", build_file(BASE_COLUMNS, base_groups(9)))
    accept("no-groups", build_file(BASE_COLUMNS, []))
    # Two INT64_MAX groups plus 2 rows wrap a uint64 sum to num_rows 0.
    wrap = [{"rows": 2**63 - 1}, {"rows": 2**63 - 1}, {"rows": 2}]
    refuse("rows-wrap", build_file(BASE_COLUMNS, wrap, num_rows=0))
    long_name = [{"name": "n" * 63, "type": 1, "field_id": 1}]
    accept("name-63", build_file(long_name, [{"rows": 1}]))
    refuse("name-64", build_file([{"name": "n" * 64, "type": 1, "field_id": 1}], [{"rows": 1}]))

    # Missing statistics stay missing instead of turning into zeros.
    got = accept("no-null-count", build_file(BASE_COLUMNS, with_chunk(base_groups(), 1, 1, null_count=None)))
    pm = got["iceberg"]["columns"][1]
    require(pm["null_value_count"] is None and pm["lower_bound"] is not None, "absent null_count")
    got = accept("no-nan-count", build_file(BASE_COLUMNS, with_chunk(base_groups(), 0, 1, nan_count=None)))
    require(got["iceberg"]["columns"][1]["nan_value_count"] is None, "absent nan_count in one group")
    got = accept("no-bounds", build_file(BASE_COLUMNS, with_chunk(base_groups(), 1, 2, min=None, max=None)))
    require(got["iceberg"]["columns"][2]["lower_bound"] is None, "bounds missing for a chunk with values")
    groups = with_chunk(base_groups(), 1, 1, null_count=3, nan_count=2, min=None, max=None)
    got = accept("all-null-or-nan", build_file(BASE_COLUMNS, groups))
    require(got["iceberg"]["columns"][1]["lower_bound"] == f32(-1.5).hex(), "all null or NaN chunk is excused")
    groups = with_chunk(base_groups(), 1, 1, null_count=None, nan_count=None, min=None, max=None)
    got = accept("unknown-chunk", build_file(BASE_COLUMNS, groups))
    require(got["iceberg"]["columns"][1]["lower_bound"] is None, "chunk with unknown content voids bounds")
    groups = with_chunk(base_groups(1), 0, 1, min=f32(0.0), max=f32(0.0))
    got = accept("zero-bounds", build_file(BASE_COLUMNS, groups))
    require((got["iceberg"]["columns"][1]["lower_bound"], got["iceberg"]["columns"][1]["upper_bound"])
            == ("00000080", "00000000"), "signed zero widening")
    got = accept("dictionary", build_file(BASE_COLUMNS, with_chunk(base_groups(), 0, 0, dictionary=True)))
    require(got["row_groups"][0]["columns"][0]["start_offset"] == 4
            and got["row_groups"][0]["columns"][0]["data_page_offset"] == 12, "dictionary start offset")
    # Field ids: the summary still decodes, the Iceberg metrics refuse.
    no_id = [dict(c) for c in BASE_COLUMNS]
    no_id[2]["field_id"] = None
    got = accept("no-field-id", build_file(no_id, base_groups()))
    require(got.get("metrics_error") == "column has no field_id", "missing field id")
    duplicate = [dict(c) for c in BASE_COLUMNS]
    duplicate[2]["field_id"] = 2
    got = accept("duplicate-field-id", build_file(duplicate, base_groups()))
    require(got.get("metrics_error") == "duplicate field_id", "duplicate field id")
    for value in (0, -3):
        bad = [dict(c) for c in BASE_COLUMNS]
        bad[1]["field_id"] = value
        refuse(f"field-id-{value}", build_file(bad, base_groups()))

    def columns_with(index: int, **changes) -> list[dict]:
        out = [dict(c) for c in BASE_COLUMNS]
        out[index].update(changes)
        return out

    refuse_files = {
        "nan-min": build_file(BASE_COLUMNS, with_chunk(base_groups(), 0, 1, min=f32(math.nan))),
        "min-above-max": build_file(BASE_COLUMNS, with_chunk(base_groups(), 0, 2, min=i32(5), max=i32(1))),
        "short-bound": build_file(BASE_COLUMNS, with_chunk(base_groups(), 0, 2, min=b"\x01", max=b"\x02")),
        "min-only": build_file(BASE_COLUMNS, with_chunk(base_groups(), 0, 2, max=None)),
        "num-rows": build_file(BASE_COLUMNS, base_groups(), num_rows=11),
        "num-values": build_file(BASE_COLUMNS, with_chunk(base_groups(), 0, 2, num_values=4)),
        "null-count": build_file(BASE_COLUMNS, with_chunk(base_groups(), 0, 1, null_count=6)),
        "nan-count": build_file(BASE_COLUMNS, with_chunk(base_groups(), 0, 1, null_count=4, nan_count=2)),
        "chunk-outside": build_file(BASE_COLUMNS, with_chunk(base_groups(), 1, 2, size=10_000)),
        "path": build_file(BASE_COLUMNS, with_chunk(base_groups(), 0, 2, path="other")),
        "chunk-type": build_file(BASE_COLUMNS, with_chunk(base_groups(), 0, 2, type=2)),
        "nanos": build_file(columns_with(0, logical=timestamp(unit=3)), base_groups()),
        "local-time": build_file(columns_with(0, logical=timestamp(utc=False)), base_groups()),
        "string-logical": build_file(columns_with(0, logical=Struct().struct(1, Struct())), base_groups()),
        "timestamp-int32": build_file(columns_with(2, logical=timestamp()), base_groups()),
        "converted-only": build_file(columns_with(2, converted=10), base_groups()),
        "double-type": build_file(columns_with(2, type=5), base_groups()),
        "repeated-schema": build_file(BASE_COLUMNS, base_groups(), schema_extra=Struct()),
        "encrypted": build_file(BASE_COLUMNS, base_groups(), extra=[(8, STRUCT, b"\x00")]),
        "trailing-bytes": build_file(BASE_COLUMNS, base_groups(), trailing=b"\x00"),
    }
    for label, data in refuse_files.items():
        refuse(label, data)
    accept("converted-with-logical", build_file(columns_with(0, converted=10), base_groups()))

    got = accept("geo-columns", build_file(GEO_COLUMNS, base_groups()))
    require([c["logical"] for c in got["columns"][3:]] == ["uint8", "geometry"], "UINT8 and GEOMETRY")
    require(got["iceberg"]["columns"][4]["lower_bound"] is None, "GEOMETRY has no Iceberg bounds")
    accept("uint8-without-converted", build_file(
        BASE_COLUMNS + [{k: v for k, v in GEO_COLUMNS[3].items() if k != "converted"}], base_groups()))

    def geo_with(index: int, **changes) -> list[dict]:
        out = [dict(c) for c in GEO_COLUMNS]
        out[index].update(changes)
        return out

    geo_refused = {
        "uint8-int64": build_file(geo_with(3, type=2), base_groups()),
        "int8-signed": build_file(geo_with(3, logical=integer(signed=True)), base_groups()),
        "uint16": build_file(geo_with(3, logical=integer(width=16)), base_groups()),
        "uint8-converted-only": build_file(geo_with(3, logical=None), base_groups()),
        "uint8-converted-on-timestamp": build_file(geo_with(0, converted=11), base_groups()),
        "geometry-crs": build_file(geo_with(4, logical=geometry("OGC:CRS84")), base_groups()),
        "geometry-int64": build_file(geo_with(4, type=2), base_groups()),
        "byte-array-plain": build_file(geo_with(4, logical=None), base_groups()),
        "geometry-bounds": build_file(GEO_COLUMNS, with_chunk(base_groups(), 0, 4, min=b"\x00" * 4,
                                                              max=b"\x00" * 4)),
    }
    for label, data in geo_refused.items():
        refuse(label, data)

    raw = build_file(BASE_COLUMNS, base_groups())
    refuse("bad-head-magic", b"PAR0" + raw[4:])
    refuse("bad-tail-magic", raw[:-1] + b"0")
    refuse("footer-too-long", raw[:-8] + struct.pack("<I", len(raw)) + b"PAR1")
    refuse("footer-empty", raw[:-8] + struct.pack("<I", 0) + b"PAR1")
    refuse("tiny", b"PAR1PAR1")
    for cut in (1, 7, 40, 200):
        length = struct.unpack("<I", raw[-8:-4])[0]
        body = raw[-8 - length:-8]
        refuse(f"truncated-{cut}", raw[:-8 - length] + body[:-cut] + struct.pack("<I", length - cut) + b"PAR1")
    print(f"PASS crafted footers: {counts['accepted']} accepted shapes, "
          f"{counts['refused']} hostile or inconsistent shapes refused", flush=True)


def fuzz(tools: Tools, directory: Path) -> None:
    targets = sorted(directory.glob("writer-9x9x*-lz4.parquet")) + [directory / "pyarrow-ZSTD-True-4.parquet",
                                                                     directory / "crafted-base.parquet"]
    total = accepted = 0
    for seed, path in enumerate(targets, start=1):
        run = subprocess.run([str(tools.reader), "fuzz", str(path), "4000", str(seed)], capture_output=True,
                             text=True, timeout=300)
        require(run.returncode == 0, f"fuzz {path.name}: rc={run.returncode} {run.stderr[-3000:]}")
        result = json.loads(run.stdout)
        total += result["iterations"]
        accepted += result["accepted"]
    print(f"PASS fuzz loop: {total} mutated or truncated footers, {accepted} still valid, "
          "no crash, hang or inconsistent summary", flush=True)


def page_cases(tools: Tools, directory: Path) -> None:
    run = subprocess.run([str(tools.reader), "pagecases"], capture_output=True, text=True, timeout=TIMEOUT)
    require(run.returncode == 0, f"page cases: rc={run.returncode} {run.stderr[-3000:]}")
    cases = json.loads(run.stdout)["cases"]
    targets = sorted(directory.glob("writer-9x9x*.parquet")) + sorted(directory.glob("writer-90x77x4-*.parquet")) + [
        directory / "pyarrow-NONE-False-4.parquet", directory / "pyarrow-LZ4-False-11.parquet",
        directory / "writer-delta-none.parquet", directory / "writer-delta-lz4.parquet",
        directory / "delta-shapes/edge-130-delta-none.parquet", directory / "delta-shapes/edge-257-delta-lz4.parquet",
        directory / "pyarrow-delta-NONE-200.parquet", directory / "crowpanel-delta-lz4.parquet"]
    total = accepted = refused = 0
    for seed, path in enumerate(targets, start=101):
        run = subprocess.run([str(tools.reader), "pagefuzz", str(path), "1500", str(seed)], capture_output=True,
                             text=True, timeout=300)
        require(run.returncode == 0, f"page fuzz {path.name}: rc={run.returncode} {run.stderr[-3000:]}")
        result = json.loads(run.stdout)
        total += result["iterations"]
        accepted += result["accepted"]
        refused += result["refused"]
    print(f"PASS page decoder: {cases} hand-built pages return their exact result, {total} corrupted or "
          f"truncated files gave {refused} refused and {accepted} bounded column decodes, no crash", flush=True)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--sanitize", action="store_true", help="enable AddressSanitizer and UBSan")
    args = parser.parse_args()
    check_stack()
    with tempfile.TemporaryDirectory(prefix="os-parquet-reader-") as temporary:
        directory = Path(temporary)
        tools = Tools(directory, args.sanitize)
        writer_files(tools, directory)
        geo_files(tools, directory)
        pyarrow_files(tools, directory)
        delta_files(tools, directory, args.sanitize)
        crafted(tools, directory)
        fuzz(tools, directory)
        page_cases(tools, directory)
    print("PASS Parquet footer reader" + (" (ASan + UBSan)" if args.sanitize else ""))


if __name__ == "__main__":
    main()
