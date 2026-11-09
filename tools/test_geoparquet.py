"""Native GEOMETRY column and `geo` footer key, read by PyArrow and DuckDB (#83).

Run with ``pixi run python tools/test_geoparquet.py [--sanitize]``.

tools/geo_fixture.cpp writes three files through the exact firmware writer,
one where three of every four rows have a location, one where none has, and
one whose middle row group is wholly null between two located ones. This test
proves for all three

- the Parquet schema marks `geometry` BYTE_ARRAY with the GEOMETRY logical
  type and no crs, and `location_source` INT32 with INTEGER(8, unsigned) and
  ConvertedType UINT_8,
- every value is a 21 byte little endian 2D WKB Point at the expected
  longitude and latitude, null exactly where the row has no location,
- each chunk holds GeospatialStatistics (bbox and type 1) and no min/max,
  and a chunk with no point holds none,
- PyArrow 26 reports the GEOMETRY logical type and GeoStatistics, and with
  arrow_extensions_enabled reads the column as geoarrow.wkb in OGC:CRS84,
- DuckDB 2.0 reads `geometry` as its core GEOMETRY type and the points as
  WKT without the spatial extension,
- the `geo` value validates against the GeoParquet 2.0.0 schema.json pinned in
  tools/geoparquet/v2.0.0 (opengeospatial/geoparquet 1edd56f, blob 99b8312),
  and its bbox is the rows' bbox, left out when no row has a location.
"""

from __future__ import annotations

import argparse
import json
from pathlib import Path
import struct
import subprocess
import sys
import tempfile

import duckdb
import pyarrow as pa
import pyarrow.parquet as pq

sys.path.insert(0, str(Path(__file__).resolve().parent))
import footer_contract  # noqa: E402
import parquet_footer as footer  # noqa: E402
from host_cxx import CXX_STD  # noqa: E402

GROUP_ROWS = 30
GROUPS = {"with": 2, "none": 2, "mixed": 3}
# parquet.thrift 2.12.0 field ids not named in parquet_footer.py.
SCHEMA_CONVERTED = 6
LOGICAL_INTEGER, LOGICAL_GEOMETRY = 10, 17
META_GEOSPATIAL = 17
BYTE_ARRAY, INT32, UINT_8 = 6, 1, 11


def row(n: int, mode: str) -> tuple[int | None, tuple[int, int] | None]:
    """The rule of row() in tools/geo_fixture.cpp, location in degrees e7."""
    if mode == "none" or (mode == "mixed" and n // GROUP_ROWS == 1) or n % 4 == 0:
        return None, None
    step = n % 3
    return 1, (312345678 + step * 100000, 300444000 - step * 50000)


def wkb(point: tuple[int, int]) -> bytes:
    return struct.pack("<BIdd", 1, 1, point[0] / 1e7, point[1] / 1e7)


def bbox(points: list[tuple[int, int]]) -> list[float] | None:
    if not points:
        return None
    xs, ys = [p[0] for p in points], [p[1] for p in points]
    return [min(xs) / 1e7, min(ys) / 1e7, max(xs) / 1e7, max(ys) / 1e7]


class GeoArrowWkb(pa.ExtensionType):
    """Minimal geoarrow.wkb, so PyArrow can attach the Parquet GEOMETRY type."""

    def __init__(self, metadata: bytes = b"") -> None:
        self.metadata = metadata
        super().__init__(pa.binary(), "geoarrow.wkb")

    def __arrow_ext_serialize__(self) -> bytes:
        return self.metadata

    @classmethod
    def __arrow_ext_deserialize__(cls, storage_type, serialized):
        return cls(serialized)


def check_schema(meta: dict) -> None:
    leaves = {e[footer.SCHEMA_NAME].decode(): e for e in meta[footer.FILE_SCHEMA][1:]}
    source, geometry = leaves["location_source"], leaves["geometry"]
    assert source[footer.SCHEMA_TYPE] == INT32 and source[SCHEMA_CONVERTED] == UINT_8, source
    assert source[footer.SCHEMA_LOGICAL] == {LOGICAL_INTEGER: {1: 8, 2: False}}, source
    assert geometry[footer.SCHEMA_TYPE] == BYTE_ARRAY, geometry
    # GEOMETRY with no crs member, which means OGC:CRS84.
    assert geometry[footer.SCHEMA_LOGICAL] == {LOGICAL_GEOMETRY: {}}, geometry
    assert SCHEMA_CONVERTED not in geometry, geometry


def check_chunks(meta: dict, expected: list[list[tuple[int, int]]]) -> None:
    for group, points in zip(meta[footer.FILE_ROW_GROUPS], expected):
        chunk = group[footer.GROUP_COLUMNS][2][footer.CHUNK_META]
        stats = chunk[footer.META_STATISTICS]
        assert stats[footer.STAT_NULLS] == GROUP_ROWS - len(points), stats
        assert not {footer.STAT_MIN, footer.STAT_MAX, footer.STAT_MIN_DEPRECATED,
                    footer.STAT_MAX_DEPRECATED} & set(stats), "geometry has no min/max"
        box = bbox(points)
        if box is None:
            assert META_GEOSPATIAL not in chunk, "no GeospatialStatistics without a point"
            continue
        geo = chunk[META_GEOSPATIAL]
        # BoundingBox is xmin, xmax, ymin, ymax, then the WKB type code list.
        assert geo == {1: {1: box[0], 2: box[2], 3: box[1], 4: box[3]}, 2: [1]}, geo


def check_file(path: Path, mode: str, printed: str) -> None:
    sources, points = [], []
    for n in range(GROUP_ROWS * GROUPS[mode]):
        source, point = row(n, mode)
        sources.append(source)
        points.append(point)
    present = [p for p in points if p is not None]
    groups = [[p for p in points[g * GROUP_ROWS:(g + 1) * GROUP_ROWS] if p is not None]
              for g in range(GROUPS[mode])]
    assert len(footer.read_footer(path)[footer.FILE_ROW_GROUPS]) == GROUPS[mode]
    if mode == "mixed":
        assert groups[0] and not groups[1] and groups[2], "null group between located ones"

    meta = footer.read_footer(path)
    check_schema(meta)
    check_chunks(meta, groups)

    # The geo key, against the pinned GeoParquet schema and the rows' bbox.
    kv = footer_contract.read_kv(path)
    assert list(kv) == ["geo"] and kv["geo"] == printed.strip(), kv
    found = footer_contract.geo_problems(kv["geo"], bbox(present))
    assert not found, found
    document = json.loads(kv["geo"])
    assert ("bbox" in document["columns"]["geometry"]) == bool(present), document

    # PyArrow, the Parquet logical type, values and GeoStatistics.
    parquet = pq.ParquetFile(path)
    assert str(parquet.schema.column(2).logical_type) == "Geometry(crs=)", parquet.schema
    assert str(parquet.schema.column(1).logical_type) == "Int(bitWidth=8, isSigned=false)"
    table = parquet.read()
    assert table.schema.field("location_source").type == pa.uint8()
    assert table.column("location_source").to_pylist() == sources
    assert table.column("geometry").to_pylist() == [wkb(p) if p else None for p in points]
    for g, group_points in enumerate(groups):
        stats = parquet.metadata.row_group(g).column(2).geo_statistics
        box = bbox(group_points)
        if box is None:
            assert stats is None, stats
            continue
        assert stats.geospatial_types == [1], stats
        assert [stats.xmin, stats.ymin, stats.xmax, stats.ymax] == box, stats
        assert stats.zmin is None and stats.mmin is None, stats
    extended = pq.read_table(path, arrow_extensions_enabled=True)
    kind = extended.schema.field("geometry").type
    assert isinstance(kind, GeoArrowWkb), kind
    assert json.loads(kind.metadata)["crs"] == "OGC:CRS84", kind.metadata

    # DuckDB, core GEOMETRY without the spatial extension.
    connection = duckdb.connect()
    types = dict(connection.execute(
        "SELECT column_name, column_type FROM (DESCRIBE SELECT * FROM read_parquet(?))",
        [str(path)]).fetchall())
    assert types["geometry"] == "GEOMETRY" and types["location_source"] == "UTINYINT", types
    rows = connection.execute(
        "SELECT location_source, geometry::VARCHAR FROM read_parquet(?) ORDER BY sequence",
        [str(path)]).fetchall()
    want = [(s, f"POINT ({p[0] / 1e7!r} {p[1] / 1e7!r})" if p else None)
            for s, p in zip(sources, points)]
    assert rows == want, (rows[:4], want[:4])


def mutation_proof() -> None:
    """Each geo rule fails on a broken value."""
    good = footer_contract.GEO_NO_BBOX
    assert not footer_contract.geo_problems(good)
    boxed = json.loads(good)
    boxed["columns"]["geometry"]["bbox"] = [31.0, 30.0, 31.5, 30.5]
    boxed_text = json.dumps(boxed, separators=(",", ":"))
    assert not footer_contract.geo_problems(boxed_text, [31.0, 30.0, 31.5, 30.5])
    for name, text, expected in [
        ("bbox present without locations", boxed_text, None),
        ("bbox differs from the rows", boxed_text, [31.0, 30.0, 31.5, 30.6]),
        ("bbox missing with locations", good, [31.0, 30.0, 31.5, 30.5]),
        ("not compact", json.dumps(json.loads(good)), None),
        ("WKT encoding", good.replace('"WKB"', '"WKT"'), None),
        ("GeoParquet 1.1.0", good.replace('"2.0.0"', '"1.1.0"'), None),
        ("Point Z", good.replace('["Point"]', '["Point Z"]'), None),
        ("other primary column", good.replace('"primary_column":"geometry"',
                                              '"primary_column":"geom"'), None),
    ]:
        assert footer_contract.geo_problems(text, expected), f"mutation not caught: {name}"
        print(f"PASS mutation geo {name}: fails")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sanitize", action="store_true")
    args = parser.parse_args()
    root = Path(__file__).resolve().parent.parent
    common = root / "firmware/common"
    pa.register_extension_type(GeoArrowWkb())
    mutation_proof()
    with tempfile.TemporaryDirectory(prefix="aq-geoparquet-") as directory:
        executable = Path(directory) / "geo-fixture"
        command = ["clang++", CXX_STD, "-Wall", "-Wextra", "-Werror"]
        for library in ["src", "runtime/src", "connectivity/src", "logger/src",
                        "power/src", "location/src"]:
            command += ["-I", str(common / library)]
        command += [str(root / "tools/geo_fixture.cpp"), str(common / "src/parquet_writer.cpp"),
                    "-o", str(executable)]
        if args.sanitize:
            command += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
        subprocess.run(command, check=True)
        for mode in GROUPS:
            output = Path(directory) / f"{mode}.parquet"
            printed = subprocess.run([str(executable), mode, str(output)], capture_output=True,
                                     text=True, check=True).stdout
            check_file(output, mode, printed)
            print(f"PASS {mode} locations, GEOMETRY and UINT8 read by PyArrow {pa.__version__} "
                  f"and DuckDB {duckdb.__version__}, geo valid against {footer_contract.GEO_SPEC}")


if __name__ == "__main__":
    main()
