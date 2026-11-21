"""DELTA_BINARY_PACKED checks for the firmware writer (#135), run by test_parquet.py.

tools/delta_fixture.cpp writes each case twice through the exact writer, once
with IntegerEncoding::Plain and once with IntegerEncoding::DeltaBinaryPacked.
This module proves four things.

1. PyArrow and DuckDB read every DELTA file value equal to its PLAIN twin.
2. Only INT32 and INT64 columns change encoding, in the page header and in
   the column chunk encodings list. FLOAT and GEOMETRY stay PLAIN.
3. Every integer value stream is byte equal to an independent Python encoder
   written from parquet-format 2.14.0 Encodings.md, and every INT32 stream is
   byte equal to the one PyArrow writes. Arrow uses 128 values per block for
   INT32, as we do, but 256 for INT64, so INT64 has only the Python oracle.
4. The worst case INT64 page is exactly delta_binary_packed_bound().
5. Every footer field outside the page layout, statistics included, is the
   same in both twins, and every integer edge column of every DELTA file
   reads the absolute values an independent Python copy of the fixture
   generator gives, in PyArrow and in DuckDB. That covers one row, nulls,
   all null columns, INT32 and INT64 min and max and wrapping deltas, and
   footer null_count, min_value and max_value.

The checks use reader values and our own footer parser, never a reader's
metadata strings, so they hold across PyArrow and DuckDB upgrades.
"""

from __future__ import annotations

import json
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile

import duckdb
import pyarrow as pa
import pyarrow.parquet as pq

import parquet_footer as footer

# parquet.thrift Encoding and Type values (apache-parquet-format-2.14.0).
PLAIN, RLE, DELTA_BINARY_PACKED = 0, 3, 5
INT32, INT64 = 1, 2
# PageHeader and DataPageHeader field ids.
PAGE_UNCOMPRESSED, PAGE_COMPRESSED, PAGE_DATA = 2, 3, 5
DATA_VALUES, DATA_ENCODING, DATA_DEFINITION = 1, 2, 3
BLOCK, MINIBLOCKS, MINIBLOCK = 128, 4, 32
UTC_COLUMNS = ("i64_utc", "event_time_utc_us")


def require(condition: bool, description: str) -> None:
    if not condition:
        raise AssertionError(description)


def uleb128(value: int) -> bytes:
    out = bytearray()
    while True:
        byte = value & 0x7F
        value >>= 7
        if value:
            out.append(byte | 0x80)
        else:
            out.append(byte)
            return bytes(out)


def zigzag(value: int) -> int:
    return value << 1 if value >= 0 else ((-value) << 1) - 1


def signed(value: int, bits: int) -> int:
    value &= (1 << bits) - 1
    return value - (1 << bits) if value >> (bits - 1) else value


def delta_reference(values: list[int], bits: int) -> bytes:
    """Encodings.md DELTA_BINARY_PACKED with 128 value blocks of 4 miniblocks."""
    mask = (1 << bits) - 1
    out = bytearray(uleb128(BLOCK) + uleb128(MINIBLOCKS) + uleb128(len(values)))
    out += uleb128(zigzag(signed(values[0], bits) if values else 0))
    deltas = [signed(b - a, bits) for a, b in zip(values, values[1:])]
    for start in range(0, len(deltas), BLOCK):
        block = deltas[start:start + BLOCK]
        low = min(block)
        relative = [(d - low) & mask for d in block]
        minis = [relative[i:i + MINIBLOCK] for i in range(0, len(relative), MINIBLOCK)]
        widths = [max(r.bit_length() for r in mini) for mini in minis]
        widths += [0] * (MINIBLOCKS - len(widths))
        out += uleb128(zigzag(low)) + bytes(widths)
        for mini, width in zip(minis, widths):
            packed = 0
            for slot, value in enumerate(mini):  # LSB first, padding stays zero
                packed |= value << (slot * width)
            out += packed.to_bytes(width * MINIBLOCK // 8, "little")
    return bytes(out)


def pages(path: Path) -> list[tuple[int, str, int, dict, bytes]]:
    """(row group, column name, physical type, page header, payload) per chunk, UNCOMPRESSED only."""
    data = path.read_bytes()
    result = []
    for index, group in enumerate(footer.read_footer(path).get(footer.FILE_ROW_GROUPS, [])):
        for chunk in group[footer.GROUP_COLUMNS]:
            meta = chunk[footer.CHUNK_META]
            reader = footer.Reader(data, meta[footer.META_DATA_PAGE_OFFSET])
            header = reader.struct()
            payload = data[reader.pos:reader.pos + header[PAGE_COMPRESSED]]
            name = meta[footer.META_PATH][0].decode()
            result.append((index, name, meta[footer.META_TYPE], header, payload))
    return result


def value_stream(payload: bytes, optional: bool) -> bytes:
    """A DataPageV1 payload without its 4 byte prefixed RLE definition levels."""
    if not optional:
        return payload
    return payload[4 + struct.unpack("<I", payload[:4])[0]:]


def present_integers(table: pa.Table, name: str) -> list[int]:
    column = table.column(name)
    if pa.types.is_timestamp(column.type):
        column = column.cast(pa.int64())
    return [value for value in column.to_pylist() if value is not None]


def duck_rows(connection, path: Path, utc: str) -> list[tuple]:
    sql = f"SELECT * REPLACE (epoch_us({utc}) AS {utc}) FROM read_parquet(?)"
    return connection.execute(sql, [str(path)]).fetchall()


def layout_free(meta: dict) -> dict:
    """The footer without page sizes, offsets and encodings, which DELTA must change."""
    keep = {key: meta.get(key) for key in (footer.FILE_VERSION, footer.FILE_SCHEMA, footer.FILE_ROWS,
                                           footer.FILE_KV, footer.FILE_CREATED_BY, footer.FILE_COLUMN_ORDERS)}
    keep["groups"] = [
        {"rows": group[footer.GROUP_ROWS], "sorting": group.get(footer.GROUP_SORTING),
         "ordinal": group.get(footer.GROUP_ORDINAL),
         "chunks": [{key: chunk[footer.CHUNK_META].get(key)
                     for key in (footer.META_TYPE, footer.META_PATH, footer.META_CODEC, footer.META_VALUES,
                                 footer.META_STATISTICS)}
                    for chunk in group[footer.GROUP_COLUMNS]]}
        for group in meta.get(footer.FILE_ROW_GROUPS, [])]
    return keep


# An independent copy of the integer generator in tools/delta_fixture.cpp,
# so the edge files are checked against absolute values and not only against
# their PLAIN twins. (name, bits, kind, nulls, base, step, noise) in column
# order. The FLOAT and GEOMETRY columns sit at the end of kEdges.
I32_MIN, I64_MIN = -(1 << 31), -(1 << 63)
UTC_BASE, TEN_SECONDS, FALL = 1762646400000000, 10000000, 1000000
EDGE_INTEGERS = (
    ("i32_const", 32, "const", "required", 7, 0, 0),
    ("i32_step", 32, "linear", "required", -100, 3, 0),
    ("i32_wrap", 32, "wrap", "required", 0, 0, 0),
    ("i32_full", 32, "full", "required", 0, 0, 0),
    ("i32_random", 32, "random", "required", 0, 0, 0),
    ("i32_zero_then_wide", 32, "zero_then_wide", "required", 0, 0, 0),
    ("i32_wide_zero_wide", 32, "wide_zero_wide", "required", 0, 0, 0),
    ("i32_nulls", 32, "linear", "run", I32_MIN, 5, 2),
    ("i32_wrap_nulls", 32, "wrap", "every3", 0, 0, 0),
    ("i32_all_null", 32, "linear", "all", 0, 1, 0),
    ("i32_all_present", 32, "noise", "all_present", 1000, 0, 50),
    ("i32_uint8", 32, "noise", "some", 128, 0, 127),
    ("i64_utc", 64, "linear", "required", UTC_BASE, TEN_SECONDS, 3000),
    ("i64_wrap", 64, "wrap", "required", 0, 0, 0),
    ("i64_full", 64, "full", "required", 0, 0, 0),
    ("i64_random", 64, "random", "required", 0, 0, 0),
    ("i64_zero_then_wide", 64, "zero_then_wide", "required", 0, 0, 0),
    ("i64_wide_zero_wide", 64, "wide_zero_wide", "required", 0, 0, 0),
    ("i64_nulls", 64, "linear", "run", -UTC_BASE, -TEN_SECONDS, 7),
    ("i64_wrap_nulls", 64, "wrap", "every3", 0, 0, 0),
    ("i64_all_null", 64, "linear", "all", 0, 1, 0),
    ("i64_full_nulls", 64, "full", "some", 0, 0, 0),
)
MASK64 = (1 << 64) - 1


def mix(x: int) -> int:
    """splitmix64 finalizer, as mix() in delta_fixture.cpp."""
    x = (x + 0x9E3779B97F4A7C15) & MASK64
    x = ((x ^ (x >> 30)) * 0xBF58476D1CE4E5B9) & MASK64
    x = ((x ^ (x >> 27)) * 0x94D049BB133111EB) & MASK64
    return x ^ (x >> 31)


def edge_value(column: int, row: int, bits: int, kind: str, base: int, step: int, noise: int) -> int:
    mask = (1 << bits) - 1
    low = 1 << (bits - 1)
    random = mix((column << 32) ^ row)
    symmetric = random % (2 * noise + 1) - noise if noise > 0 else 0
    if kind == "const":
        pattern = base
    elif kind == "linear":
        pattern = base + step * row + symmetric
    elif kind == "noise":
        pattern = base + symmetric
    elif kind == "wrap":
        pattern = low - 1 if row % 2 else low
    elif kind == "full":
        pattern = low
        for i in range(1, row + 1):
            pattern = (pattern + (low if i % 2 else low - 1)) & mask
    elif kind == "random":
        pattern = random
    elif kind == "zero_then_wide":
        pattern = -FALL * row if row <= 32 else random & 0xFFFF
    else:  # wide_zero_wide
        pattern = -FALL * (row - 32) if 32 <= row <= 64 else random & 0xFFFF
    return signed(pattern & mask, bits)


def edge_present(nulls: str, row: int) -> bool:
    return {"required": True, "all_present": True, "every3": row % 3 != 0,
            "run": row % 3 != 0 and (row < 40 or row >= 80), "all": False,
            "some": row % 17 != 5}[nulls]


def expected_edges(rows: int) -> dict[str, list[int | None]]:
    return {name: [edge_value(column, row, bits, kind, base, step, noise) if edge_present(nulls, row) else None
                   for row in range(rows)]
            for column, (name, bits, kind, nulls, base, step, noise) in enumerate(EDGE_INTEGERS)}


def check_edges(directory: Path) -> int:
    """Absolute integer values and statistics of every DELTA edge file, in both readers."""
    checked = 0
    for path in sorted(directory.glob("edge-*-delta-*.parquet")):
        rows = int(path.name.split("-")[1])
        expected = expected_edges(rows)
        table = pq.read_table(path)
        require(table.column_names[:len(EDGE_INTEGERS)] == list(expected), f"{path.name}: integer columns in order")
        with duckdb.connect() as connection:
            names = ", ".join(f"epoch_us({name})" if name in UTC_COLUMNS else name for name in expected)
            duck = connection.execute(f"SELECT {names} FROM read_parquet(?)", [str(path)]).fetchall()
        require(len(duck) == rows, f"{path.name}: DuckDB row count {rows}")
        chunks = {chunk[footer.CHUNK_META][footer.META_PATH][0].decode(): chunk[footer.CHUNK_META]
                  for chunk in footer.read_footer(path)[footer.FILE_ROW_GROUPS][0][footer.GROUP_COLUMNS]}
        for index, (name, values) in enumerate(expected.items()):
            column = table.column(name)
            if pa.types.is_timestamp(column.type):
                column = column.cast(pa.int64())
            require(column.to_pylist() == values, f"{path.name}: PyArrow {name} equals the generator")
            require([row[index] for row in duck] == values, f"{path.name}: DuckDB {name} equals the generator")
            bits = EDGE_INTEGERS[index][1]
            present = [value for value in values if value is not None]
            stats = chunks[name][footer.META_STATISTICS]
            require(stats[footer.STAT_NULLS] == rows - len(present), f"{path.name}: null_count of {name}")
            if not present:
                require(footer.STAT_MIN not in stats and footer.STAT_MAX not in stats,
                        f"{path.name}: all null {name} has no bounds")
                continue
            pack = "<i" if bits == 32 else "<q"
            require((stats[footer.STAT_MIN], stats[footer.STAT_MAX]) ==
                    (struct.pack(pack, min(present)), struct.pack(pack, max(present))),
                    f"{path.name}: min_value and max_value of {name}")
        checked += 1
    # The named edges, so a generator drift cannot hide them.
    one = expected_edges(1)
    require(one["i32_wrap"] == [I32_MIN] and one["i64_wrap"] == [I64_MIN] and one["i64_full"] == [I64_MIN]
            and one["i32_all_null"] == [None] and one["i32_wrap_nulls"] == [None], "one row edge values")
    many = expected_edges(257)
    require(many["i32_wrap"][:2] == [I32_MIN, -I32_MIN - 1] and many["i64_wrap"][:2] == [I64_MIN, -I64_MIN - 1],
            "INT32 and INT64 wrap rows alternate min and max")
    require(all(value is None for value in many["i64_all_null"] + many["i32_all_null"]), "all null columns")
    require(many["i64_full"][:3] == [I64_MIN, 0, -I64_MIN - 1] and many["i32_full"][:3] == [I32_MIN, 0, -I32_MIN - 1],
            "full width deltas start at min, then wrap through 0 to max")
    require([row for row, value in enumerate(many["i32_nulls"][:90]) if value is None] ==
            [row for row in range(90) if row % 3 == 0 or 40 <= row < 80], "run of nulls in rows 40 to 79")
    return checked


def check_pair(plain: Path, delta: Path) -> None:
    left, right = pq.read_table(plain), pq.read_table(delta)
    require(right.num_rows == left.num_rows and right.schema.equals(left.schema, check_metadata=True),
            f"{delta.name}: schema and rows equal the PLAIN file")
    require(right.equals(left), f"{delta.name}: PyArrow values equal the PLAIN file")
    utc = next(name for name in UTC_COLUMNS if name in left.column_names)
    with duckdb.connect() as connection:
        expected = duck_rows(connection, plain, utc)
        require(len(expected) == left.num_rows, f"{plain.name}: DuckDB row count")
        require(duck_rows(connection, delta, utc) == expected, f"{delta.name}: DuckDB values equal the PLAIN file")
    require(layout_free(footer.read_footer(delta)) == layout_free(footer.read_footer(plain)),
            f"{delta.name}: footer outside the page layout, statistics included, equals the PLAIN file")
    for path, delta_on in ((plain, False), (delta, True)):
        meta = footer.read_footer(path)
        for group in meta.get(footer.FILE_ROW_GROUPS, []):
            for chunk in group[footer.GROUP_COLUMNS]:
                column = chunk[footer.CHUNK_META]
                integer = column[footer.META_TYPE] in (INT32, INT64)
                encoding = DELTA_BINARY_PACKED if delta_on and integer else PLAIN
                require(column[footer.META_ENCODINGS] == [encoding, RLE],
                        f"{path.name}: encodings of {column[footer.META_PATH][0].decode()}")


def check_streams(plain: Path, delta: Path, scratch: Path) -> tuple[int, int]:
    """Byte checks on an UNCOMPRESSED pair. Returns (INT32 single group pages, those matched with PyArrow)."""
    source = pq.ParquetFile(plain)
    groups = [source.read_row_group(g) for g in range(source.metadata.num_row_groups)]
    optional = {field.name: field.nullable for field in source.schema_arrow}
    single, arrow_matched = 0, 0
    for group, name, kind, header, payload in pages(delta):
        table = groups[group]
        data = header[PAGE_DATA]
        require(header[PAGE_UNCOMPRESSED] == header[PAGE_COMPRESSED] == len(payload), f"{delta.name}: page sizes")
        require(data[DATA_VALUES] == table.num_rows, f"{delta.name}: page num_values of {name}")
        require(data[DATA_DEFINITION] == RLE, f"{delta.name}: RLE definition levels on {name}")
        if kind not in (INT32, INT64):
            require(data[DATA_ENCODING] == PLAIN, f"{delta.name}: {name} stays PLAIN")
            continue
        require(data[DATA_ENCODING] == DELTA_BINARY_PACKED, f"{delta.name}: page encoding of {name}")
        stream = value_stream(payload, optional[name])
        require(stream == delta_reference(present_integers(table, name), 32 if kind == INT32 else 64),
                f"{delta.name}: {name} group {group} equals the Encodings.md reference")
        if kind != INT32 or len(groups) != 1:
            continue
        # PyArrow writes the same column as DELTA_BINARY_PACKED. Its INT32
        # encoder also uses 128 value blocks, so the streams must match.
        single += 1
        arrow = scratch / f"arrow-{delta.stem}-{name}.parquet"
        pq.write_table(table.select([name]), arrow, use_dictionary=False, compression="NONE",
                       column_encoding={name: "DELTA_BINARY_PACKED"}, data_page_version="1.0",
                       write_statistics=False)
        (_, _, _, arrow_header, arrow_payload), = pages(arrow)
        require(arrow_header[PAGE_DATA][DATA_ENCODING] == DELTA_BINARY_PACKED, "PyArrow wrote DELTA")
        require(value_stream(arrow_payload, optional[name]) == stream,
                f"{delta.name}: {name} equals the PyArrow DELTA stream")
        arrow_matched += 1
    return single, arrow_matched


def first_block_widths(stream: bytes) -> list[int]:
    """The 4 miniblock widths of the first block, after 4 header varints and the min delta."""
    reader = footer.Reader(stream)
    for _ in range(5):
        reader.varint()
    return list(stream[reader.pos:reader.pos + MINIBLOCKS])


def delta_bound(values: int, value_bytes: int) -> int:
    """delta_binary_packed_bound() in firmware/common/src/parquet_writer.h."""
    blocks = (max(values - 1, 0) + BLOCK - 1) // BLOCK
    return 2 + 1 + len(uleb128(values)) + 10 + blocks * (10 + 4 + MINIBLOCKS * MINIBLOCK * value_bytes)


def run(root: Path, directory: Path, compile_flags: list[str], keep: Path | None = None) -> list[dict]:
    executable = directory / "delta_fixture"
    command = ["clang++", *compile_flags, "-I", str(root / "firmware/common/vendor/lz4"),
               str(root / "tools/delta_fixture.cpp"),
               str(root / "firmware/common/src/parquet_writer.cpp"),
               str(root / "firmware/common/src/lz4_codec.cpp"), "-o", str(executable)]
    subprocess.run(command, check=True)
    output = directory / "delta"
    output.mkdir()
    # The binary checks the bound formula, refuses an unknown encoding and
    # proves LZ4 scratch one byte short of the worst page fails closed.
    completed = subprocess.run([str(executable), str(output)], check=True, capture_output=True, text=True)
    sizes = [json.loads(line) for line in completed.stdout.splitlines()]
    plains = sorted(output.glob("*-plain-*.parquet"))
    require(len(plains) == 26, "delta fixture wrote every PLAIN file")
    single = arrow_matched = 0
    with tempfile.TemporaryDirectory(prefix="m5-parquet-delta-") as temporary:
        for plain in plains:
            delta = plain.with_name(plain.name.replace("-plain-", "-delta-"))
            check_pair(plain, delta)
            if plain.name.endswith("-none.parquet"):
                pair = check_streams(plain, delta, Path(temporary))
                single, arrow_matched = single + pair[0], arrow_matched + pair[1]
    require(arrow_matched and arrow_matched == single, "every single group INT32 stream was compared with PyArrow")
    edges = check_edges(output)
    require(edges == 22, "every edge row count was checked in both codecs")
    # 129 values whose 128 deltas alternate INT64 min and max need 64 bits in
    # every miniblock, which is the bound exactly.
    worst = [page for page in pages(output / "edge-129-delta-none.parquet") if page[1] == "i64_full"]
    require(len(worst) == 1 and worst[0][3][PAGE_UNCOMPRESSED] == delta_bound(129, 8),
            "worst case INT64 page equals delta_binary_packed_bound(129, 8)")
    # A zero width miniblock before or between wide ones must be written as
    # no bytes. These columns put the block minimum in that miniblock.
    edge = {page[1]: page[4] for page in pages(output / "edge-129-delta-none.parquet")}
    for kind in ("i32", "i64"):
        first = first_block_widths(edge[f"{kind}_zero_then_wide"])
        middle = first_block_widths(edge[f"{kind}_wide_zero_wide"])
        require(first[0] == 0 and all(first[1:]), f"{kind}_zero_then_wide widths {first}")
        require(middle[1] == 0 and middle[0] and all(middle[2:]), f"{kind}_wide_zero_wide widths {middle}")
    print(f"PASS DELTA_BINARY_PACKED: {len(plains)} PLAIN and DELTA pairs equal in PyArrow and DuckDB, "
          f"integer streams equal the Encodings.md reference, {arrow_matched} INT32 streams equal PyArrow, "
          f"twin footers equal outside the page layout, {edges} edge files hold the generator values and "
          "statistics in both readers, worst case page equals the bound", flush=True)
    if keep is not None:
        # Fixtures for readers outside this repo, such as the mobile
        # EncodingFixtureTest. The directory must not exist yet.
        shutil.copytree(output, keep)
        print(f"KEPT {len(list(keep.glob('*.parquet')))} DELTA fixture files in {keep}", flush=True)
    for size in sizes:
        print(f"SIZE real shape {size['groups']} x 90 rows {size['encoding']} {size['codec']}: "
              f"{size['bytes']} bytes", flush=True)
    return sizes
