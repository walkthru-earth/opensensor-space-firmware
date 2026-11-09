"""Cross-reader conformance checks for the exact C++ writer used on the board.

Run with ``pixi run python tools/test_parquet.py [--sanitize]``.
Each fixture is read by PyArrow and DuckDB (exact values and nulls), and its
footer is decoded field by field to check statistics, column orders, row-group
metadata and the TIMESTAMP annotation. The fixture binary also checks writer
capacity rejection, abort-on-sink-failure and row-group limits.

Every case runs twice, once with the default PLAIN encoding and once with
DELTA_BINARY_PACKED integer columns (#135). tools/parquet_delta.py then checks
the DELTA encoder against PLAIN twins, an Encodings.md reference and PyArrow.
"""

from __future__ import annotations

import argparse
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
import parquet_delta  # noqa: E402
import parquet_footer as footer  # noqa: E402
from host_cxx import CXX_STD


BASE_NAMES = [
    "time_ms",
    "counter",
    "temperature",
    "optional_counter",
    "all_null",
    "signed_extreme",
    "optional_extreme",
    "all_present",
]
OPTIONAL_NAMES = {
    "optional_counter",
    "all_null",
    "optional_extreme",
    "all_present",
}
CREATED_BY = "opensensor-space-firmware version 0.3 (build fixture)"


def require(condition: bool, description: str) -> None:
    if not condition:
        raise AssertionError(description)


def column_names(columns: int) -> list[str]:
    names = BASE_NAMES + [f"extra_{i:02}" for i in range(8, columns)]
    if columns > 8:
        names[8] = "positive_zero"
    return names


def expected_temperature(index: int, rows: int) -> float:
    if rows > 4 and index < 4:
        return (-0.0, math.inf, -math.inf, math.nan)[index]
    return index * 0.25 - 10.0


def check_float(actual: float, expected: float, description: str) -> None:
    if math.isnan(expected):
        require(actual is not None and math.isnan(actual), description)
    else:
        require(actual == expected, description)
        if expected == 0:
            require(math.copysign(1, actual) == math.copysign(1, expected), description)


def expected_values(index: int, rows: int, columns: int) -> tuple:
    """Row `index` (global across row groups; `rows` is the per-group size)."""
    int32_value = 2**31 - 1 if index % 2 else -(2**31)
    int64_value = 2**63 - 1 if index % 2 else -(2**63)
    temperature = expected_temperature(index, rows)
    values = [
        1700000000000 + index * 10000,
        int32_value,
        temperature,
        int32_value if index % 2 else None,
        None,
        int64_value,
        int64_value if index % 2 else None,
        temperature,
    ] + [temperature] * (columns - 8)
    if columns > 8:
        values[8] = 0.0
    return tuple(values)


def check_row(actual: tuple, index: int, rows: int, columns: int, reader: str) -> None:
    expected = expected_values(index, rows, columns)
    for col in range(columns):
        if isinstance(expected[col], float):
            check_float(actual[col], expected[col], f"{reader}: row {index}, column {col}")
        else:
            require(actual[col] == expected[col], f"{reader}: row {index}, column {col}")


def plain(value, kind: pa.DataType) -> bytes:
    if kind == pa.int32():
        return struct.pack("<i", value)
    if kind == pa.int64():
        return struct.pack("<q", value)
    return struct.pack("<f", value)


def expected_statistics(values: list, kind: pa.DataType) -> dict:
    """null_count, nan_count and PLAIN-encoded bounds under TYPE_ORDER rules."""
    present = [v for v in values if v is not None]
    nans = [v for v in present if isinstance(v, float) and math.isnan(v)]
    finite = [v for v in present if not (isinstance(v, float) and math.isnan(v))]
    result = {"null_count": len(values) - len(present), "nan_count": len(nans), "bounds": None}
    if finite:
        low, high = min(finite), max(finite)
        if kind == pa.float32():
            low = -0.0 if low == 0 else low
            high = 0.0 if high == 0 else high
        result["bounds"] = (plain(low, kind), plain(high, kind))
    return result


def check_footer(path: Path, rows: int, columns: int, groups: int, codec: int, delta: bool) -> None:
    names = column_names(columns)
    meta = footer.read_footer(path)
    require(meta[footer.FILE_VERSION] == 1, "footer: format version")
    require(meta[footer.FILE_CREATED_BY] == CREATED_BY.encode(), "footer: created_by with build")
    schema = meta[footer.FILE_SCHEMA]
    require(len(schema) == columns + 1 and schema[0][footer.SCHEMA_CHILDREN] == columns, "footer: schema root")
    require(footer.SCHEMA_FIELD_ID not in schema[0], "footer: root has no field_id")
    for index, (element, name) in enumerate(zip(schema[1:], names, strict=True), start=1):
        require(element[footer.SCHEMA_NAME] == name.encode(), "footer: leaf name")
        require(element.get(footer.SCHEMA_FIELD_ID) == index, f"footer: field_id on {name}")
        logical = element.get(footer.SCHEMA_LOGICAL)
        if name == "time_ms":
            # LogicalType.TIMESTAMP{isAdjustedToUTC=true, unit=MICROS}
            require(logical == {footer.LOGICAL_TIMESTAMP: {1: True, 2: {2: {}}}}, "footer: TIMESTAMP(MICROS, UTC)")
        else:
            require(logical is None, f"footer: no annotation on {name}")
    orders = meta[footer.FILE_COLUMN_ORDERS]
    require(orders == [{1: {}}] * columns, "footer: one TYPE_ORDER per leaf")
    row_groups = meta.get(footer.FILE_ROW_GROUPS, [])
    require(len(row_groups) == groups and meta[footer.FILE_ROWS] == rows * groups, "footer: row groups and rows")
    types = column_types(columns)
    for g, group in enumerate(row_groups):
        chunks = group[footer.GROUP_COLUMNS]
        require(group[footer.GROUP_ROWS] == rows, "footer: group rows")
        require(group[footer.GROUP_ORDINAL] == g, "footer: group ordinal")
        require(group[footer.GROUP_SORTING] == [{1: 0, 2: False, 3: False}], "footer: sorted by time_ms ascending")
        require(group[footer.GROUP_OFFSET] == chunks[0][footer.CHUNK_META][footer.META_DATA_PAGE_OFFSET],
                "footer: group file_offset is the first page")
        require(group[footer.GROUP_COMPRESSED] == sum(c[footer.CHUNK_META][footer.META_COMPRESSED] for c in chunks),
                "footer: group total_compressed_size")
        require(group[footer.GROUP_BYTES] == sum(c[footer.CHUNK_META][footer.META_UNCOMPRESSED] for c in chunks),
                "footer: group total_byte_size")
        for index, chunk in enumerate(chunks):
            column = chunk[footer.CHUNK_META]
            require(chunk[footer.CHUNK_FILE_OFFSET] == 0, "footer: deprecated ColumnChunk.file_offset is 0")
            integer = types[index] in (pa.int32(), pa.int64())
            require(column[footer.META_ENCODINGS] == [5 if delta and integer else 0, 3],
                    f"footer: value encoding then RLE for {names[index]}")
            require(column[footer.META_CODEC] == codec and column[footer.META_VALUES] == rows, "footer: chunk codec/values")
            values = [expected_values(g * rows + r, rows, columns)[index] for r in range(rows)]
            expected = expected_statistics(values, types[index])
            stats = column[footer.META_STATISTICS]
            require(footer.STAT_MIN_DEPRECATED not in stats and footer.STAT_MAX_DEPRECATED not in stats,
                    "footer: deprecated min/max omitted")
            require(stats[footer.STAT_NULLS] == expected["null_count"], f"footer: null_count {names[index]}")
            if types[index] == pa.float32():
                require(stats[footer.STAT_NANS] == expected["nan_count"], f"footer: nan_count {names[index]}")
            else:
                require(footer.STAT_NANS not in stats, "footer: nan_count only on FLOAT")
            if expected["bounds"] is None:
                require(footer.STAT_MIN not in stats and footer.STAT_MAX not in stats, f"footer: no bounds {names[index]}")
            else:
                require((stats[footer.STAT_MIN], stats[footer.STAT_MAX]) == expected["bounds"],
                        f"footer: min_value/max_value {names[index]} in group {g}")
                require(stats[footer.STAT_MIN_EXACT] is True and stats[footer.STAT_MAX_EXACT] is True,
                        "footer: exact flags")


def column_types(columns: int) -> list[pa.DataType]:
    return [pa.int64(), pa.int32(), pa.float32(), pa.int32(), pa.float32(),
            pa.int64(), pa.int64(), pa.float32()] + [pa.float32()] * (columns - 8)


def integer_encoding(data_type: pa.DataType, delta: bool) -> str:
    """The encoding PyArrow must report for a column's values."""
    integer = pa.types.is_integer(data_type) or pa.types.is_timestamp(data_type)
    return "DELTA_BINARY_PACKED" if delta and integer else "PLAIN"


def check_file(path: Path, rows: int, columns: int, groups: int = 1, codec: str = "UNCOMPRESSED",
               delta: bool = False) -> None:
    names = column_names(columns)
    types = column_types(columns)
    parquet = pq.ParquetFile(path)
    metadata = parquet.metadata
    table = parquet.read()
    total = rows * groups
    require(table.num_rows == total and table.num_columns == columns, "Arrow table dimensions")
    require(table.column_names == names, "Arrow schema names/order")
    require(table.schema.field(0).type == pa.timestamp("us", tz="UTC"), "Arrow reads the TIMESTAMP annotation")
    for index, field in enumerate(parquet.schema_arrow, start=1):
        require(field.metadata and field.metadata.get(b"PARQUET:field_id") == str(index).encode(), f"Arrow field_id: {field.name}")
    for field, name, data_type in zip(list(table.schema)[1:], names[1:], types[1:], strict=True):
        require(field.type == data_type, f"Arrow type: {name}")
        require(field.nullable == (name in OPTIONAL_NAMES), f"Arrow nullability: {name}")
    require(metadata.num_rows == total, "footer row count")
    require(metadata.num_columns == columns, "footer column count")
    require(metadata.num_row_groups == (groups if rows else 0), "footer row-group count")
    require(metadata.metadata[b"sample_interval_ms"] == b"10000", "interval metadata")
    require(metadata.metadata[b"fixture"] == b"firmware writer interoperability", "fixture metadata")
    require(metadata.created_by == CREATED_BY, "writer identity")
    for g in range(metadata.num_row_groups):
        group = metadata.row_group(g)
        require(group.num_rows == rows, "row-group rows")
        require(group.total_byte_size == sum(group.column(i).total_uncompressed_size for i in range(columns)),
                "row-group uncompressed size accounting")
        require([(s.column_index, s.descending, s.nulls_first) for s in group.sorting_columns] == [(0, False, False)],
                "row-group sorting columns")
        for index, name in enumerate(names):
            column = group.column(index)
            require(column.path_in_schema == name, "column metadata order")
            require(column.compression == codec, f"codec: {name}")
            # The default writes PLAIN everywhere, byte for byte as before #135.
            require(set(column.encodings) == {integer_encoding(types[index], delta), "RLE"}, f"encodings: {name}")
            require(column.num_values == rows, f"value count: {name}")
            values = [expected_values(g * rows + r, rows, columns)[index] for r in range(rows)]
            expected = expected_statistics(values, types[index])
            statistics = column.statistics
            require(statistics.null_count == expected["null_count"], f"null statistics: {name}")
            require(statistics.has_min_max == (expected["bounds"] is not None), f"min/max presence: {name}")
            if expected["bounds"] is not None and index:
                low, high = expected["bounds"]
                require(plain(statistics.min, types[index]) == low and plain(statistics.max, types[index]) == high,
                        f"PyArrow min/max: {name} in group {g}")
    check_footer(path, rows, columns, groups if rows else 0, 7 if codec == "LZ4" else 0, delta)
    arrays = [table.column(0).cast(pa.int64()).to_pylist()] + [c.to_pylist() for c in table.columns[1:]]
    for index, row in enumerate(zip(*arrays, strict=True)):
        check_row(row, index, rows, columns, "PyArrow")
    with duckdb.connect() as connection:
        # DuckDB presents TIMESTAMP(MICROS, UTC) as TIMESTAMPTZ; recover the integer.
        result = connection.execute(
            "SELECT * REPLACE (epoch_us(time_ms) AS time_ms) FROM read_parquet(?)", [str(path)])
        require([column[0] for column in result.description] == names, "DuckDB schema names/order")
        duck_rows = result.fetchall()
        if rows:
            duck_groups = connection.execute(
                "SELECT count(DISTINCT row_group_id), sum(stats_null_count) FROM parquet_metadata(?) "
                "WHERE path_in_schema = 'all_null'", [str(path)]).fetchone()
            require(duck_groups == (groups, total), "DuckDB sees every row group and its null statistics")
            later = connection.execute(
                "SELECT count(*) FROM read_parquet(?) WHERE epoch_us(time_ms) >= ?",
                [str(path), 1700000000000 + rows * 10000]).fetchone()[0]
            require(later == rows * (groups - 1), "DuckDB range query across row groups")
    require(len(duck_rows) == total, "DuckDB row count")
    for index, row in enumerate(duck_rows):
        require(len(row) == columns, "DuckDB column count")
        check_row(row, index, rows, columns, "DuckDB")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sanitize", action="store_true", help="enable AddressSanitizer and UBSan")
    parser.add_argument("--keep-delta", type=Path, metavar="DIR",
                        help="copy the PLAIN and DELTA twins from tools/delta_fixture.cpp to DIR, which must not exist")
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    with tempfile.TemporaryDirectory(prefix="m5-parquet-test-") as temporary:
        directory = Path(temporary)
        executable = directory / "parquet_fixture"
        flags = [CXX_STD, "-Wall", "-Wextra", "-Werror"]
        if args.sanitize:
            flags += ["-fsanitize=address,undefined", "-fno-sanitize-recover=all"]
        command = ["clang++", *flags, "-I", str(root / "firmware/common/vendor/lz4")]
        command += [
            str(root / "tools/parquet_fixture.cpp"),
            str(root / "firmware/common/src/parquet_writer.cpp"),
            str(root / "firmware/common/src/lz4_codec.cpp"),
            "-o", str(executable),
        ]
        subprocess.run(command, check=True)
        for label, rows, columns, groups in (
            ("empty", 0, 8, 1),
            ("one", 1, 8, 1),
            ("wide", 90, 96, 1),
            ("large", 65536, 8, 1),
            ("hourly", 90, 77, 4),
            ("max-groups", 3, 9, 8),
        ):
            for codec in ("UNCOMPRESSED", "LZ4"):
                for encoding in ("plain", "delta"):
                    path = directory / f"{label}-{codec}-{encoding}.parquet"
                    command = [str(executable), str(path), str(rows), str(columns),
                               "lz4" if codec == "LZ4" else "none", str(groups), encoding]
                    subprocess.run(command, check=True, stdout=subprocess.DEVNULL)
                    # PyArrow names codec enum 7 (LZ4_RAW) "LZ4" in its Python API.
                    check_file(path, rows, columns, groups, codec, encoding == "delta")
                    print(f"PASS {label} {codec} {encoding}: {groups} x {rows} rows x {columns} columns; "
                          "PyArrow + DuckDB exact readback, footer statistics", flush=True)
        parquet_delta.run(root, directory, flags, args.keep_delta.resolve() if args.keep_delta else None)
    print("PASS firmware Parquet writer conformance" + (" (ASan + UBSan)" if args.sanitize else ""))


if __name__ == "__main__":
    main()
