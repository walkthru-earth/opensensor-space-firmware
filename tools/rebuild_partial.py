#!/usr/bin/env python3
"""Rebuild a readable Parquet file from a quarantined or retained `.partial`.

The board writes a file as PAR1, then each row group as one DataPage V1 per
column in the order of its telemetry_fields.inc, then the footer. A power cut
before the footer leaves every completed row group on the card with no
metadata. This tool walks the page headers from offset 4, keeps each row group
whose pages are all complete and decode cleanly, drops the torn tail, and
writes a new footer that mirrors firmware/common/src/parquet_writer.cpp.

The schema comes from the board field table, never from the bytes, so a page
is accepted only when its header has exactly the writer shape and its payload
decodes to the declared row count with the declared value width. The input is
never modified. The output goes to a separate directory, never overwrites an
existing file, and is checked with parquet_device.validate_file, which makes
PyArrow and DuckDB agree before the tool reports success.

Usage examples

    pixi run rebuild-partial --board waveshare --out rebuilt/ a.partial
    pixi run rebuild-partial --fields path/telemetry_fields.inc --columns 60 \
        --codec lz4 --out rebuilt/ quarantine/*.partial

The `columns=` and `codec=` values of the `PARQUET BEGIN` line for the boot
that wrote the file give `--columns` and `--codec` when the field table has
grown since. Rebuilt files carry `rebuild_*` key values naming the source and
the dropped tail. Rows that never reached a row group were never on the card
and cannot be recovered by any tool.
"""

from __future__ import annotations

import argparse
from dataclasses import dataclass, field
import hashlib
import json
import math
from pathlib import Path
import re
import struct
import sys

import pyarrow as pa

import boards
from parquet_footer import Reader

ROOT = Path(__file__).resolve().parent.parent
TOOL_ID = "opensensor-space-firmware rebuild_partial version 1"
MAX_ROW_GROUPS = 8  # telemetry::kMaxRowGroups
MAX_ROWS = 65536  # telemetry::kMaxRows

# Parquet physical types and codecs as the writer emits them. The only
# BYTE_ARRAY the writer emits is a 2D little endian WKB Point, stored as a
# four byte length then 21 bytes.
INT32, INT64, FLOAT, BYTE_ARRAY = 1, 2, 4, 6
TYPES = {"Int32": INT32, "Int64": INT64, "Float": FLOAT, "ByteArray": BYTE_ARRAY}
WKB_POINT_BYTES = 21
WIDTH = {INT32: 4, INT64: 8, FLOAT: 4, BYTE_ARRAY: 4 + WKB_POINT_BYTES}
WKB_POINT = 1  # ISO WKB and Geospatial.md geometry type code
CONVERTED_UINT8 = 11  # ConvertedType UINT_8
# Unit labels that pick a logical type, telemetry::logical_for in
# firmware/common/src/telemetry_logical.h.
UTC_MICROS_UNIT = "us_since_unix_epoch"
UINT8_CODE_UNIT = "uint8_code"
WKB_POINT_UNIT = "wkb_point_crs84"
UNCOMPRESSED, LZ4_RAW = 0, 7
CODECS = {"none": UNCOMPRESSED, "lz4": LZ4_RAW}
CODEC_NAMES = {UNCOMPRESSED: "none", LZ4_RAW: "lz4"}

FIELD_RE = re.compile(
    r"^\s*FIELD\(\s*([A-Za-z_][A-Za-z0-9_]*)\s*,\s*(Int32|Int64|Float|ByteArray)\s*,"
    r"[^,]*,\s*\"([^\"]*)\"",
    re.MULTILINE,
)


class RebuildError(Exception):
    """The input cannot be rebuilt (not a board file, or the schema is wrong)."""


@dataclass(frozen=True)
class Field:
    name: str
    type: int
    unit: str

    @property
    def timestamp(self) -> bool:
        return self.type == INT64 and self.unit == UTC_MICROS_UNIT

    @property
    def uint8(self) -> bool:
        return self.type == INT32 and self.unit == UINT8_CODE_UNIT

    @property
    def geometry(self) -> bool:
        return self.type == BYTE_ARRAY and self.unit == WKB_POINT_UNIT


def load_fields(path: Path) -> list[Field]:
    text = Path(path).read_text()
    fields = [Field(m[1], TYPES[m[2]], m[3]) for m in FIELD_RE.finditer(text)]
    if not fields:
        raise RebuildError(f"no FIELD rows in {path}")
    for f in fields:
        if f.type == BYTE_ARRAY and not f.geometry:
            raise RebuildError(f"{f.name} is a BYTE_ARRAY the writer does not emit")
    names = [f.name for f in fields]
    if len(set(names)) != len(names):
        raise RebuildError(f"duplicate field names in {path}")
    return fields


# ----------------------------------------------------------------------------
# Page parsing. Every check mirrors what the writer emits, so random bytes or a
# torn page are rejected instead of decoded into wrong rows.


class Torn(Exception):
    """The bytes end inside this page or do not form a writer page."""


@dataclass
class Chunk:
    offset: int
    size: int  # header + stored payload
    uncompressed_size: int  # header + raw payload
    present: list[bool]
    values: list  # ints, float bit patterns or (lon, lat) points, present rows only


@dataclass
class Group:
    offset: int
    end: int
    rows: int
    chunks: list[Chunk] = field(default_factory=list)


def page_header(data: bytes, pos: int) -> tuple[int, int, int, int]:
    """Return (payload_start, raw_size, stored_size, rows) or raise Torn."""
    reader = Reader(data, pos)
    try:
        header = reader.struct()
    except (IndexError, ValueError, struct.error) as error:
        raise Torn(f"page header at {pos}: {error}") from None
    body = header.get(5)
    if (
        set(header) != {1, 2, 3, 5}
        or header[1] != 0
        or not isinstance(body, dict)
        or set(body) != {1, 2, 3, 4}
        or (body[2], body[3], body[4]) != (0, 3, 3)
    ):
        raise Torn(f"page header at {pos} is not a writer DATA_PAGE")
    raw, stored, rows = header[2], header[3], body[1]
    if not (0 < rows <= MAX_ROWS) or raw <= 0 or stored <= 0:
        raise Torn(f"page header at {pos} has impossible sizes")
    return reader.pos, raw, stored, rows


def decode_payload(raw: bytes, rows: int, kind: int) -> tuple[list[bool], list]:
    """Decode an OPTIONAL flat column payload exactly as the writer lays it out."""
    if len(raw) < 4:
        raise Torn("payload shorter than its level length")
    length = struct.unpack_from("<I", raw)[0]
    if 4 + length > len(raw):
        raise Torn("definition levels overrun the payload")
    levels = Reader(raw[4 : 4 + length])
    present: list[bool] = []
    try:
        while levels.pos < length:
            header = levels.varint()
            if header & 1 or not header >> 1:
                raise Torn("definition levels are not writer RLE runs")
            value = levels.byte()
            if value > 1:
                raise Torn("definition level above the max level")
            present.extend([value == 1] * (header >> 1))
    except IndexError:
        raise Torn("definition levels end inside a run") from None
    if levels.pos != length or len(present) != rows:
        raise Torn("definition levels do not cover the page rows")
    width = WIDTH[kind]
    count = sum(present)
    if len(raw) != 4 + length + count * width:
        raise Torn("payload size does not match the present values")
    if kind == BYTE_ARRAY:
        values = []
        for record in struct.iter_unpack("<IBIdd", raw[4 + length :]):
            size, order, shape, lon, lat = record
            if (size, order, shape) != (WKB_POINT_BYTES, 1, WKB_POINT):
                raise Torn("BYTE_ARRAY value is not a writer WKB Point")
            if not (-180.0 <= lon <= 180.0 and -90.0 <= lat <= 90.0):
                raise Torn("WKB Point lies outside CRS84 bounds")
            values.append((lon, lat))
        return present, values
    fmt = {INT32: "<i", INT64: "<q", FLOAT: "<I"}[kind]
    values = [v[0] for v in struct.iter_unpack(fmt, raw[4 + length :])]
    return present, values


def parse_page(data: bytes, pos: int, kind: int, codec: int, rows: int | None) -> Chunk:
    start, raw_size, stored_size, page_rows = page_header(data, pos)
    if rows is not None and page_rows != rows:
        raise Torn(f"page at {pos} has {page_rows} rows, the group has {rows}")
    if start + stored_size > len(data):
        raise Torn(f"page at {pos} ends past the bytes on the card")
    # The writer emits at most one RLE run per row (a varint of at most 3
    # bytes for 65536 rows, plus the value byte) and one value per row, so a
    # larger raw size is not a writer page and is never allocated.
    if raw_size > 4 + page_rows * (4 + WIDTH[kind]):
        raise Torn(f"page at {pos} declares more bytes than the writer layout allows")
    stored = data[start : start + stored_size]
    if codec == UNCOMPRESSED:
        if stored_size != raw_size:
            raise Torn(f"page at {pos} is compressed in an uncompressed file")
        raw = stored
    else:
        try:
            raw = pa.decompress(stored, decompressed_size=raw_size, codec="lz4_raw", asbytes=True)
        except (OSError, ValueError) as error:
            raise Torn(f"page at {pos} does not decompress: {error}") from None
        # Arrow returns raw_size bytes even when the block decodes to fewer
        # and leaves the tail uninitialized. A block that also fits in one
        # byte less is short, so only an exact length passes.
        try:
            pa.decompress(stored, decompressed_size=raw_size - 1, codec="lz4_raw", asbytes=True)
        except (OSError, ValueError):
            pass
        else:
            raise Torn(f"page at {pos} decompresses to fewer bytes than its header says")
    present, values = decode_payload(raw, page_rows, kind)
    header_size = start - pos
    return Chunk(pos, header_size + stored_size, header_size + raw_size, present, values)


def parse_group(data: bytes, pos: int, fields: list[Field], codec: int) -> Group:
    group = Group(pos, pos, 0)
    rows = None
    for f in fields:
        chunk = parse_page(data, group.end, f.type, codec, rows)
        rows = len(chunk.present)
        group.chunks.append(chunk)
        group.end = chunk.offset + chunk.size
    group.rows = rows or 0
    return group


def tail_kind(tail: bytes) -> str:
    if not tail:
        return "none"
    # A page header starts with field 1 i32 = 0, a footer with field 1 i32 = 1.
    if tail == b"\x15" or tail[:2] == b"\x15\x00":
        return "page"
    if tail[:2] == b"\x15\x02":
        return "footer"
    return "unknown"


# ----------------------------------------------------------------------------
# Footer encoding, a mirror of the writer's Thrift compact output.


class Out:
    def __init__(self) -> None:
        self.buf = bytearray()

    def varint(self, value: int) -> None:
        while True:
            byte = value & 0x7F
            value >>= 7
            if value:
                self.buf.append(byte | 0x80)
            else:
                self.buf.append(byte)
                return

    def integer(self, value: int) -> None:
        self.varint((value << 1) ^ (value >> 63))  # zigzag

    def binary(self, value: bytes) -> None:
        self.varint(len(value))
        self.buf += value

    def list(self, kind: int, count: int) -> None:
        if count < 15:
            self.buf.append((count << 4) | kind)
        else:
            self.buf.append(0xF0 | kind)
            self.varint(count)


class Struct:
    def __init__(self, out: Out) -> None:
        self.out = out
        self.previous = 0

    def field(self, field_id: int, kind: int) -> None:
        delta = field_id - self.previous
        if field_id > self.previous and delta <= 15:
            self.out.buf.append((delta << 4) | kind)
        else:
            self.out.buf.append(kind)
            self.out.integer(field_id)
        self.previous = field_id

    def number(self, field_id: int, kind: int, value: int) -> None:
        self.field(field_id, kind)
        self.out.integer(value)

    def binary(self, field_id: int, value: bytes) -> None:
        self.field(field_id, 8)
        self.out.binary(value)

    def boolean(self, field_id: int, value: bool) -> None:
        self.field(field_id, 1 if value else 2)

    def byte(self, field_id: int, value: int) -> None:
        self.field(field_id, 3)
        self.out.buf.append(value)

    def real(self, field_id: int, value: float) -> None:
        self.field(field_id, 7)
        self.out.buf += struct.pack("<d", value)

    def end(self) -> None:
        self.out.buf.append(0)


I16, I32, I64, BINARY, LIST, STRUCT = 4, 5, 6, 8, 9, 12


def f32(bits: int) -> float:
    return struct.unpack("<f", struct.pack("<I", bits))[0]


def statistics(kind: int, chunk: Chunk) -> tuple[int, int, bytes | None, bytes | None]:
    """Return (null_count, nan_count, max, min) by the writer TYPE_ORDER rules."""
    nulls = len(chunk.present) - len(chunk.values)
    if kind == FLOAT:
        numbers = [f32(bits) for bits in chunk.values]
        finite = [v for v in numbers if not math.isnan(v)]
        nans = len(numbers) - len(finite)
        if not finite:
            return nulls, nans, None, None
        low, high = min(finite), max(finite)
        low = -0.0 if low == 0.0 else low
        high = 0.0 if high == 0.0 else high
        return nulls, nans, struct.pack("<f", high), struct.pack("<f", low)
    if not chunk.values or kind == BYTE_ARRAY:
        return nulls, 0, None, None  # GEOMETRY has no sort order
    fmt = "<i" if kind == INT32 else "<q"
    return nulls, 0, struct.pack(fmt, max(chunk.values)), struct.pack(fmt, min(chunk.values))


def schema_element(out: Out, f: Field, field_id: int) -> None:
    leaf = Struct(out)
    leaf.number(1, I32, f.type)
    leaf.number(3, I32, 1)  # OPTIONAL, every board leaf has a validity byte
    leaf.binary(4, f.name.encode())
    if f.uint8:
        leaf.number(6, I32, CONVERTED_UINT8)
    leaf.number(9, I32, field_id)
    if f.uint8:
        leaf.field(10, STRUCT)
        logical = Struct(out)
        logical.field(10, STRUCT)  # INTEGER
        integer = Struct(out)
        integer.byte(1, 8)  # bitWidth
        integer.boolean(2, False)  # isSigned
        integer.end()
        logical.end()
    elif f.geometry:
        leaf.field(10, STRUCT)
        logical = Struct(out)
        logical.field(17, STRUCT)  # GEOMETRY, no crs means OGC:CRS84
        Struct(out).end()
        logical.end()
    elif f.timestamp:
        leaf.field(10, STRUCT)
        logical = Struct(out)
        logical.field(8, STRUCT)
        stamp = Struct(out)
        stamp.boolean(1, True)
        stamp.field(2, STRUCT)
        unit = Struct(out)
        unit.field(2, STRUCT)
        Struct(out).end()
        unit.end()
        stamp.end()
        logical.end()
    leaf.end()


def column_chunk(out: Out, f: Field, chunk: Chunk, rows: int, codec: int) -> None:
    outer = Struct(out)
    outer.number(2, I64, 0)
    outer.field(3, STRUCT)
    meta = Struct(out)
    meta.number(1, I32, f.type)
    meta.field(2, LIST)
    out.list(I32, 2)
    out.integer(0)
    out.integer(3)
    meta.field(3, LIST)
    out.list(BINARY, 1)
    out.binary(f.name.encode())
    meta.number(4, I32, codec)
    meta.number(5, I64, rows)
    meta.number(6, I64, chunk.uncompressed_size)
    meta.number(7, I64, chunk.size)
    meta.number(9, I64, chunk.offset)
    meta.field(12, STRUCT)
    stats = Struct(out)
    nulls, nans, high, low = statistics(f.type, chunk)
    stats.number(3, I64, nulls)
    if high is not None:
        stats.binary(5, high)
        stats.binary(6, low)
        stats.boolean(7, True)
        stats.boolean(8, True)
    if f.type == FLOAT:
        stats.number(9, I64, nans)
    stats.end()
    if f.geometry and chunk.values:
        xs = [lon for lon, _ in chunk.values]
        ys = [lat for _, lat in chunk.values]
        meta.field(17, STRUCT)  # GeospatialStatistics
        geo = Struct(out)
        geo.field(1, STRUCT)  # BoundingBox, 2D
        box = Struct(out)
        box.real(1, min(xs))
        box.real(2, max(xs))
        box.real(3, min(ys))
        box.real(4, max(ys))
        box.end()
        geo.field(2, LIST)  # geospatial_types
        out.list(I32, 1)
        out.integer(WKB_POINT)
        geo.end()
    meta.end()
    outer.end()


def e7_text(degrees: float) -> str:
    """A coordinate as os_logger_footer.h e7_decimal writes it."""
    e7 = round(degrees * 1e7)
    sign = "-" if e7 < 0 else ""
    whole, fraction = divmod(abs(e7), 10_000_000)
    if not fraction:
        return f"{sign}{whole}"
    return f"{sign}{whole}.{fraction:07d}".rstrip("0")


def geo_json(fields: list[Field], groups: list[Group]) -> str | None:
    """The GeoParquet `geo` value the board writes, from the kept groups."""
    columns = [i for i, f in enumerate(fields) if f.geometry]
    if not columns:
        return None
    points = [p for g in groups for p in g.chunks[columns[0]].values]
    column = {"encoding": "WKB", "geometry_types": ["Point"]}
    text = json.dumps(
        {"version": "2.0.0", "primary_column": fields[columns[0]].name,
         "columns": {fields[columns[0]].name: column}},
        separators=(",", ":"))
    if not points:
        return text
    xs = [x for x, _ in points]
    ys = [y for _, y in points]
    bbox = ",".join(e7_text(v) for v in (min(xs), min(ys), max(xs), max(ys)))
    return text[:-3] + f',"bbox":[{bbox}]' + "}}}"


def sorted_by(fields: list[Field], group: Group) -> int | None:
    """The logger sorts each group by `sequence`, so claim it only when true."""
    names = [f.name for f in fields]
    if "sequence" not in names:
        return None
    chunk = group.chunks[names.index("sequence")]
    present = chunk.present
    # Nulls last, values non decreasing.
    if any(not a and b for a, b in zip(present, present[1:])):
        return None
    if any(a > b for a, b in zip(chunk.values, chunk.values[1:])):
        return None
    return names.index("sequence")


def footer(fields: list[Field], groups: list[Group], codec: int, kv: list[tuple[str, str]]) -> bytes:
    out = Out()
    file = Struct(out)
    file.number(1, I32, 1)
    file.field(2, LIST)
    out.list(STRUCT, len(fields) + 1)
    root = Struct(out)
    root.binary(4, b"telemetry")
    root.number(5, I32, len(fields))
    root.end()
    for i, f in enumerate(fields):
        schema_element(out, f, i + 1)
    file.number(3, I64, sum(g.rows for g in groups))
    file.field(4, LIST)
    out.list(STRUCT, len(groups))
    for ordinal, group in enumerate(groups):
        rg = Struct(out)
        rg.field(1, LIST)
        out.list(STRUCT, len(fields))
        for f, chunk in zip(fields, group.chunks):
            column_chunk(out, f, chunk, group.rows, codec)
        rg.number(2, I64, sum(c.uncompressed_size for c in group.chunks))
        rg.number(3, I64, group.rows)
        sort = sorted_by(fields, group)
        if sort is not None:
            rg.field(4, LIST)
            out.list(STRUCT, 1)
            sorting = Struct(out)
            sorting.number(1, I32, sort)
            sorting.boolean(2, False)
            sorting.boolean(3, False)
            sorting.end()
        rg.number(5, I64, group.offset)
        rg.number(6, I64, sum(c.size for c in group.chunks))
        rg.number(7, I16, ordinal)
        rg.end()
    if kv:
        file.field(5, LIST)
        out.list(STRUCT, len(kv))
        for key, value in kv:
            pair = Struct(out)
            pair.binary(1, key.encode())
            pair.binary(2, value.encode())
            pair.end()
    file.binary(6, TOOL_ID.encode())
    file.field(7, LIST)
    out.list(STRUCT, len(fields))
    for _ in fields:
        order = Struct(out)
        order.field(1, STRUCT)
        Struct(out).end()
        order.end()
    file.end()
    return bytes(out.buf)


# ----------------------------------------------------------------------------


@dataclass
class Plan:
    status: str  # rebuilt, complete, empty
    codec: int | None
    groups: list[Group]
    good_end: int
    tail_bytes: int
    tail: str
    reason: str


def has_footer(data: bytes) -> bool:
    if len(data) < 12 or data[-4:] != b"PAR1":
        return False
    length = struct.unpack_from("<I", data, len(data) - 8)[0]
    if length <= 0 or length > len(data) - 12:
        return False
    try:
        reader = Reader(data[-8 - length : -8])
        meta = reader.struct()
    except (IndexError, ValueError, struct.error):
        return False
    return reader.pos == length and isinstance(meta.get(4), list)


def plan(data: bytes, fields: list[Field], codec: str = "auto") -> Plan:
    if data[:4] != b"PAR1":
        if len(data) < 4 and b"PAR1".startswith(data):
            return Plan("empty", None, [], len(data), 0, "none", "cut before the magic")
        raise RebuildError("the file does not start with PAR1")
    if has_footer(data):
        return Plan("complete", None, [], len(data), 0, "none", "footer is intact")
    candidates = [CODECS[codec]] if codec != "auto" else [UNCOMPRESSED, LZ4_RAW]
    chosen = None
    groups: list[Group] = []
    reason = "no complete row group"
    for candidate in candidates:
        try:
            groups = [parse_group(data, 4, fields, candidate)]
            chosen = candidate
            break
        except Torn as error:
            reason = str(error)
    pos = 4
    if chosen is not None:
        pos = groups[0].end
        while len(groups) < MAX_ROW_GROUPS:
            try:
                group = parse_group(data, pos, fields, chosen)
            except Torn as error:
                reason = str(error)
                break
            groups.append(group)
            pos = group.end
        else:
            reason = "row group capacity reached"
    tail = data[pos:]
    status = "rebuilt" if groups else "empty"
    return Plan(status, chosen, groups, pos, len(tail), tail_kind(tail), reason)


def output_name(source: Path) -> str:
    name = source.name
    if name.endswith(".partial"):
        name = name[: -len(".partial")]
    elif name.endswith(".parquet"):
        name = name[: -len(".parquet")]
    return name + ".rebuilt.parquet"


def rebuild(
    source: Path,
    out_dir: Path,
    fields: list[Field],
    *,
    codec: str = "auto",
    fields_path: Path | None = None,
    extra_kv: list[tuple[str, str]] | None = None,
    validate: bool = True,
) -> dict:
    data = Path(source).read_bytes()
    result = {"source": str(source), "source_bytes": len(data), "columns": len(fields)}
    p = plan(data, fields, codec)
    result.update(
        status=p.status,
        row_groups=len(p.groups),
        rows=sum(g.rows for g in p.groups),
        kept_bytes=p.good_end,
        dropped_tail_bytes=p.tail_bytes,
        tail=p.tail,
        stop_reason=p.reason,
        codec=CODEC_NAMES.get(p.codec),
    )
    if p.status == "empty":
        return result
    out_dir.mkdir(parents=True, exist_ok=True)
    target = out_dir / output_name(Path(source))
    if p.status == "complete":
        body = data
    else:
        digest = hashlib.sha256(data).hexdigest()
        kv = [
            ("rebuild_source", Path(source).name),
            ("rebuild_source_bytes", str(len(data))),
            ("rebuild_source_sha256", digest),
            ("rebuild_dropped_tail_bytes", str(p.tail_bytes)),
            ("rebuild_tool", TOOL_ID),
            ("rebuild_note", "footer rebuilt host side from page headers, rows after the last complete row group are not here"),
        ]
        if fields_path is not None:
            kv.append(("rebuild_fields_sha256", hashlib.sha256(Path(fields_path).read_bytes()).hexdigest()))
        geo = geo_json(fields, p.groups)
        if geo is not None:
            kv.append(("geo", geo))
        kv.extend(extra_kv or [])
        meta = footer(fields, p.groups, p.codec, kv)
        body = data[: p.good_end] + meta + struct.pack("<I", len(meta)) + b"PAR1"
    with open(target, "xb") as handle:
        handle.write(body)
    result["output"] = str(target)
    result["output_bytes"] = len(body)
    if validate:
        from parquet_device import validate_file

        try:
            check = validate_file(target, allow_nonfinite=True)
        except Exception:
            target.unlink()
            raise
        result["readers_match"] = check["readers_match"]
        if check.get("rows") != result["rows"] and p.status == "rebuilt":
            target.unlink()
            raise RebuildError(f"readers saw {check.get('rows')} rows, expected {result['rows']}")
    return result


def parse_kv(text: str) -> tuple[str, str]:
    key, sep, value = text.partition("=")
    if not sep or not key:
        raise argparse.ArgumentTypeError("use key=value")
    return key, value


def board_fields() -> dict[str, Path]:
    """The field table of every board.json board (#141), by alias."""
    return {board.alias: ROOT / board.app / "telemetry_fields.inc" for board in boards.boards()}


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    schema = parser.add_mutually_exclusive_group(required=True)
    schema.add_argument("--board", help="the alias, footer slug or backup prefix of a board.json")
    schema.add_argument("--fields", type=Path, help="a board telemetry_fields.inc")
    parser.add_argument("--columns", type=int, help="first N fields, from the PARQUET BEGIN columns= value")
    parser.add_argument("--codec", choices=["auto", "none", "lz4"], default="auto")
    parser.add_argument("--kv", type=parse_kv, action="append", default=[], help="extra footer key=value")
    parser.add_argument("--out", type=Path, required=True, help="output directory, never the source directory")
    parser.add_argument("files", type=Path, nargs="+")
    args = parser.parse_args(argv)
    if args.fields is not None:
        fields_path = args.fields
    else:
        try:
            fields_path = ROOT / boards.find(args.board).app / "telemetry_fields.inc"
        except boards.BoardError as error:
            parser.error(str(error))
    fields = load_fields(fields_path)
    if args.columns is not None:
        if not 0 < args.columns <= len(fields):
            parser.error(f"--columns must be between 1 and {len(fields)}")
        fields = fields[: args.columns]
    failed = False
    for source in args.files:
        if source.resolve().parent == args.out.resolve():
            print(json.dumps({"source": str(source), "status": "error", "error": "--out is the source directory"}))
            failed = True
            continue
        try:
            report = rebuild(source, args.out, fields, codec=args.codec, fields_path=fields_path, extra_kv=args.kv)
        except Exception as error:  # report every file, then fail the run
            report = {"source": str(source), "status": "error", "error": str(error)}
            failed = True
        print(json.dumps(report, sort_keys=True))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
