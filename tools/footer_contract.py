"""The `opensensor` and `geo` Parquet footer (#88, #83) checks shared by the
footer tests.

A telemetry file with a `geometry` column carries exactly two key-value
entries, `opensensor` then `geo`. A file without one, such as the soil fixture,
carries only `opensensor`.

`opensensor` is compact UTF-8 JSON valid against the pinned stac-opensensor
json-schema/footer.json v0.1.0 (commit 83787768cc8451109d48a03d4ef5493ffcd403dd)
in tools/stac_opensensor/v0.1.0. The
schema sets no additionalProperties, so problems() also holds every object to
the schema's members in the schema's order, with at least the members of the
pinned example footers/m5stack-cores3.json. dict and desc come from the
caller, read from the board digest headers, never hardcoded here. PROFILE
also makes thing.board the board slug and firmware.boot_id 32 hex.

`geo` is GeoParquet 2.0.0 file metadata valid against the GeoParquet
schema.json pinned in tools/geoparquet/v2.0.0 (opengeospatial/geoparquet
1edd56f, blob 99b8312), for the one native GEOMETRY column `geometry` with
2D Points, no crs (OGC:CRS84) and a bbox only when a row has a location.

mutation_proof() shows each rule fails on a broken footer.
"""

from __future__ import annotations

import copy
import json
from pathlib import Path
import re
import struct

import duckdb
from jsonschema import Draft7Validator, FormatChecker
import pyarrow.parquet as pq

import parquet_footer

PINNED = Path(__file__).resolve().parent / "stac_opensensor"
SCHEMA = PINNED / "v0.1.0/footer.json"
EXAMPLE = PINNED / "footers/m5stack-cores3.json"
SPEC = "https://github.com/walkthru-earth/stac-opensensor (83787768cc8451109d48a03d4ef5493ffcd403dd)"
KEY = "opensensor"
GEO_KEY = "geo"
GEO_SCHEMA = PINNED.parent / "geoparquet/v2.0.0/schema.json"
GEO_SPEC = "https://github.com/opengeospatial/geoparquet (1edd56f, schema.json 2.0.0)"
GEOMETRY = "geometry"
# The `geo` value a board writes before any row has a location.
GEO_NO_BBOX = ('{"version":"2.0.0","primary_column":"geometry","columns":'
               '{"geometry":{"encoding":"WKB","geometry_types":["Point"]}}}')
# telemetry::Writer::finish caps one value at 4096 bytes, NUL not counted.
MAX_JSON_BYTES = 4096


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def schema() -> dict:
    return json.loads(SCHEMA.read_text())


def example() -> dict:
    return json.loads(EXAMPLE.read_text())


def validator() -> Draft7Validator:
    require("uri" in FormatChecker().checkers, "format uri is enforced (rfc3986-validator)")
    return Draft7Validator(schema(), format_checker=FormatChecker())


def geo_validator() -> Draft7Validator:
    return Draft7Validator(json.loads(GEO_SCHEMA.read_text()))


def geo_problems(text: str, bbox: list[float] | None = None) -> list[str]:
    """Every reason a `geo` value breaks GeoParquet 2.0.0 or the board profile.
    `bbox`, when given, is the bbox the rows imply."""
    try:
        document = json.loads(text)
    except ValueError as error:
        return [f"geo not JSON: {error}"]
    found = [f"geo schema {'/'.join(map(str, e.path)) or '/'}: {e.message}"
             for e in sorted(geo_validator().iter_errors(document), key=lambda e: list(e.path))]
    if not isinstance(document, dict) or found:
        return found or ["geo is not an object"]
    if json.dumps(document, separators=(",", ":"), ensure_ascii=False) != text:
        found.append("geo is not compact JSON, or has a duplicate member")
    if document.get("primary_column") != GEOMETRY or list(document["columns"]) != [GEOMETRY]:
        found.append(f"geo describes {list(document['columns'])}, want only {GEOMETRY!r}")
    column = document["columns"].get(GEOMETRY, {})
    if sorted(column) not in (["encoding", "geometry_types"], ["bbox", "encoding", "geometry_types"]):
        found.append(f"geo column members {sorted(column)}, want encoding, geometry_types, optional bbox")
    if column.get("geometry_types") != ["Point"]:
        found.append("geometry_types is not exactly 2D Point")
    box = column.get("bbox")
    if box is not None:
        if len(box) != 4 or not (-180 <= box[0] <= box[2] <= 180 and -90 <= box[1] <= box[3] <= 90):
            found.append(f"bbox {box} is not 2D [xmin, ymin, xmax, ymax] in CRS84 range")
    if (bbox is not None or box is not None) and box != bbox:
        found.append(f"bbox {box} is not the rows' bbox {bbox}")
    return found


def digest_of(header: Path, name: str) -> str:
    """The 64 hex digits of `constexpr const char *<name> =` in a header."""
    match = re.search(name + r'\s*=\s*"([0-9a-f]{64})"', header.read_text())
    require(match is not None, f"{name} in {header}")
    return match[1]


def order_problems(document: dict, spec: dict, sample: dict, where: str = "") -> list[str]:
    """Members are a subset of the schema's, a superset of the example's and
    in schema order, at every level."""
    allowed = list(spec["properties"])
    keys = list(document)
    problems = []
    extra = [k for k in keys if k not in allowed]
    if extra:
        problems.append(f"{where or '/'}: members not in the schema {extra}")
    missing = [k for k in sample if k not in keys]
    if missing:
        problems.append(f"{where or '/'}: members of the example missing {missing}")
    known = [k for k in keys if k in allowed]
    if known != [k for k in allowed if k in keys]:
        problems.append(f"{where or '/'}: members out of schema order {keys}")
    for key in known:
        child = spec["properties"][key]
        if isinstance(document[key], dict) and "properties" in child:
            problems += order_problems(document[key], child, sample.get(key, {}), f"{where}/{key}")
    return problems


def problems(text: str, *, dict_sha: str, desc: str) -> list[str]:
    """Every reason the footer JSON breaks the contract, empty when it holds."""
    try:
        document = json.loads(text)
    except ValueError as error:
        return [f"not JSON: {error}"]
    found = [f"schema {'/'.join(map(str, e.path)) or '/'}: {e.message}"
             for e in sorted(validator().iter_errors(document), key=lambda e: list(e.path))]
    if not isinstance(document, dict):
        return found or ["footer is not an object"]
    found += order_problems(document, schema(), example())
    if json.dumps(document, separators=(",", ":"), ensure_ascii=False) != text:
        found.append("not compact JSON, or a duplicate member")
    if len(text.encode()) > MAX_JSON_BYTES:
        found.append(f"{len(text.encode())} bytes is over the writer cap {MAX_JSON_BYTES}")
    if document.get("dictionary", {}).get("sha256") != dict_sha:
        found.append("dictionary.sha256 is not dict")
    if document.get("template", {}).get("sha256") != desc:
        found.append("template.sha256 is not desc")
    # PROFILE rules the JSON schema leaves to prose.
    board = document.get("thing", {}).get("board")
    if not isinstance(board, str) or re.fullmatch(r"[a-z0-9-]+", board) is None:
        found.append("thing.board is not a board slug")
    boot = document.get("firmware", {}).get("boot_id")
    if not isinstance(boot, str) or re.fullmatch(r"[0-9a-f]{32}", boot) is None:
        found.append("firmware.boot_id is not 32 hex")
    return found


def kv_problems(kv: dict[str, str], *, dict_sha: str, desc: str, geometry: bool = True,
                bbox: list[float] | None = None) -> list[str]:
    """The whole key-value metadata, exactly opensensor then geo when the file
    has a geometry column, else exactly opensensor."""
    want = [KEY, GEO_KEY] if geometry else [KEY]
    if list(kv) != want:
        return [f"footer keys {list(kv)}, want exactly {want}"]
    found = problems(kv[KEY], dict_sha=dict_sha, desc=desc)
    if geometry:
        found += geo_problems(kv[GEO_KEY], bbox)
    return found


def kv_dict(pairs: list[tuple[str, str]]) -> dict[str, str]:
    """The footer pairs as a dict in footer order. A repeated key fails here,
    since a dict would keep only one of the two values."""
    keys = [key for key, _ in pairs]
    repeated = sorted({key for key in keys if keys.count(key) > 1})
    require(not repeated, f"footer repeats keys {repeated}")
    return dict(pairs)


def read_kv(path: Path) -> dict[str, str]:
    """Key-value metadata in footer order. pyarrow hands the keys back sorted,
    so the raw Thrift list is decoded to check that opensensor comes first."""
    entries = parquet_footer.read_footer(path).get(parquet_footer.FILE_KV, [])
    return kv_dict([(entry[1].decode(), entry.get(2, b"").decode()) for entry in entries])


# The configured cell the board contract fixtures write when located.
FIXTURE_CELL = 0x85283473fffffff
FIXTURE_RESOLUTION = 5
FIXTURE_LON_E7 = -1219763760
FIXTURE_LAT_E7 = 373457934
CONFIGURED = 1  # telemetry::LocationSource::Configured


def point_wkb(lon_e7: int, lat_e7: int) -> bytes:
    """The 21 byte little endian 2D WKB Point the writer emits."""
    return struct.pack("<BIdd", 1, 1, lon_e7 / 1e7, lat_e7 / 1e7)


def check_location(path: Path, located: bool) -> list[float] | None:
    """Checks the location columns of a board contract fixture file through
    PyArrow and DuckDB and returns the bbox the rows imply, None when unset.
    location_source is UINT8, geometry is native GEOMETRY with no crs, a WKB
    Point at the cell centre, null exactly when h3_cell_id is null, with
    GeoStatistics and no ordinary min or max."""
    parquet = pq.ParquetFile(path)
    names = parquet.schema_arrow.names
    source, geometry = names.index("location_source"), names.index(GEOMETRY)
    require(str(parquet.schema.column(source).logical_type) == "Int(bitWidth=8, isSigned=false)",
            "location_source is UINT8")
    require(str(parquet.schema.column(geometry).logical_type) == "Geometry(crs=)",
            "geometry is GEOMETRY in OGC:CRS84")
    table = parquet.read(columns=["h3_cell_id", "h3_resolution", "location_source", GEOMETRY])
    point = point_wkb(FIXTURE_LON_E7, FIXTURE_LAT_E7)
    want = ((FIXTURE_CELL, FIXTURE_RESOLUTION, CONFIGURED, point) if located
            else (None, None, None, None))
    for row in table.to_pylist():
        require(tuple(row.values()) == want, f"location row {row}")
    chunk = parquet.metadata.row_group(0).column(geometry)
    require(chunk.statistics is None or not chunk.statistics.has_min_max,
            "geometry has no ordinary min or max")
    box = [FIXTURE_LON_E7 / 1e7, FIXTURE_LAT_E7 / 1e7] * 2 if located else None
    stats = chunk.geo_statistics
    if located:
        require(stats is not None and stats.geospatial_types == [1] and
                [stats.xmin, stats.ymin, stats.xmax, stats.ymax] == box, f"GeoStatistics {stats}")
    else:
        require(stats is None, "no GeoStatistics without a location")
    with duckdb.connect() as connection:
        types = dict(connection.execute(
            "SELECT column_name, column_type FROM (DESCRIBE SELECT * FROM read_parquet(?))",
            [str(path)]).fetchall())
        require(types[GEOMETRY] == "GEOMETRY" and types["location_source"] == "UTINYINT",
                f"DuckDB location types {types[GEOMETRY]} {types['location_source']}")
        text = {value for (value,) in connection.execute(
            "SELECT geometry::VARCHAR FROM read_parquet(?)", [str(path)]).fetchall()}
    wkt = f"POINT ({FIXTURE_LON_E7 / 1e7!r} {FIXTURE_LAT_E7 / 1e7!r})"
    require(text == ({wkt} if located else {None}), f"DuckDB geometry {text}")
    return box


def check_file(path: Path, *, dict_sha: str, desc: str, geometry: bool = True,
               bbox: list[float] | None = None) -> dict:
    """Validate a real file's footer and return the parsed opensensor JSON."""
    kv = read_kv(path)
    found = kv_problems(kv, dict_sha=dict_sha, desc=desc, geometry=geometry, bbox=bbox)
    require(not found, f"{path.name} footer: " + "; ".join(found))
    return json.loads(kv[KEY])


def mutation_proof(text: str, *, dict_sha: str, desc: str, geo: str = GEO_NO_BBOX) -> list[str]:
    """Break a valid footer one rule at a time and require each to fail."""
    require(not kv_problems({KEY: text, GEO_KEY: geo}, dict_sha=dict_sha, desc=desc),
            "baseline footer is valid")
    good = json.loads(text)
    good_geo = json.loads(geo)

    def mutated(change) -> str:
        document = copy.deepcopy(good)
        change(document)
        return json.dumps(document, separators=(",", ":"), ensure_ascii=False)

    def string_number(d: dict) -> None:
        d["acquisition"]["sample_interval_ms"] = str(d["acquisition"]["sample_interval_ms"])

    def wrong_desc(d: dict) -> None:
        d["template"]["sha256"] = "0" * 64

    def missing(d: dict) -> None:
        del d["firmware"]["boot_id"]

    def free_text_board(d: dict) -> None:
        d["thing"]["board"] = "CoreS3 ESP32-S3 rev0.2"

    def short_boot(d: dict) -> None:
        d["firmware"]["boot_id"] = "0f3a"

    def long_rotation(d: dict) -> None:
        d["file"]["rotation_interval_s"] = 3601

    def extra_member(d: dict) -> None:
        d["thing"]["country_iso3166_1_alpha2"] = "EG"

    def bad_mobility(d: dict) -> None:
        d["thing"]["mobility"] = "parked"

    def no_mobility(d: dict) -> None:
        del d["thing"]["mobility"]

    def geo_mutated(change) -> str:
        document = copy.deepcopy(good_geo)
        change(document)
        return json.dumps(document, separators=(",", ":"), ensure_ascii=False)

    def with_crs(d: dict) -> None:
        d["columns"][GEOMETRY]["crs"] = None

    def polygon(d: dict) -> None:
        d["columns"][GEOMETRY]["geometry_types"] = ["Polygon"]

    def three_d(d: dict) -> None:
        d["columns"][GEOMETRY]["bbox"] = [31.0, 30.0, 0.0, 31.5, 30.5, 0.0]

    def outside(d: dict) -> None:
        d["columns"][GEOMETRY]["bbox"] = [181.0, 30.0, 182.0, 30.5]

    def second_column(d: dict) -> None:
        d["columns"]["other"] = d["columns"][GEOMETRY]

    def old_version(d: dict) -> None:
        d["version"] = "1.1.0"

    cases = {
        "stray third footer key": {KEY: text, GEO_KEY: geo,
                                   "schema_version": good["firmware"]["schema"]},
        "geo key missing": {KEY: text},
        "geo before opensensor": {GEO_KEY: geo, KEY: text},
        "number written as a string": {KEY: mutated(string_number), GEO_KEY: geo},
        "wrong template.sha256": {KEY: mutated(wrong_desc), GEO_KEY: geo},
        "missing required field": {KEY: mutated(missing), GEO_KEY: geo},
        "thing.board free text": {KEY: mutated(free_text_board), GEO_KEY: geo},
        "firmware.boot_id not 32 hex": {KEY: mutated(short_boot), GEO_KEY: geo},
        "rotation_interval_s over 3600": {KEY: mutated(long_rotation), GEO_KEY: geo},
        "unspecified extra JSON member": {KEY: mutated(extra_member), GEO_KEY: geo},
        "thing.mobility outside the enum": {KEY: mutated(bad_mobility), GEO_KEY: geo},
        "thing.mobility missing": {KEY: mutated(no_mobility), GEO_KEY: geo},
        "geo version 1.1.0": {KEY: text, GEO_KEY: geo_mutated(old_version)},
        "geo with a crs member": {KEY: text, GEO_KEY: geo_mutated(with_crs)},
        "geo geometry_types Polygon": {KEY: text, GEO_KEY: geo_mutated(polygon)},
        "geo 3D bbox": {KEY: text, GEO_KEY: geo_mutated(three_d)},
        "geo bbox outside CRS84": {KEY: text, GEO_KEY: geo_mutated(outside)},
        "geo second column": {KEY: text, GEO_KEY: geo_mutated(second_column)},
    }
    results = []
    try:
        kv_dict([(KEY, text), (GEO_KEY, geo), (KEY, text)])
    except AssertionError as error:
        results.append(f"opensensor key repeated: fails ({error})")
    else:
        raise AssertionError("mutation not caught: opensensor key repeated")
    for name, kv in cases.items():
        found = kv_problems(kv, dict_sha=dict_sha, desc=desc)
        require(bool(found), f"mutation not caught: {name}")
        results.append(f"{name}: fails ({found[0]})")
    return results


def board_digests(firmware: Path) -> tuple[str, str]:
    """dict and desc of a board, read from its generated digest headers."""
    return (digest_of(firmware / "telemetry_dictionary_digest.h", "kDictionarySha256"),
            digest_of(firmware / "telemetry_descriptor_digest.h", "kDescriptorSha256"))


def check_document(document: dict, dictionary: dict, templates: str, *, compressed: bool,
                   rotation: int, row_groups: int) -> None:
    """The values a board writes, against its compiled dictionary. `templates`
    is the board folder under docs/shared/templates."""
    require(document["firmware"]["version"] == dictionary["firmware"], "firmware.version")
    require(document["firmware"]["schema"] == dictionary["schema"], "firmware.schema")
    require(document["dictionary"] == {"sha256": dictionary["sha256"], "version": dictionary["dictionary"],
                                       "uri": dictionary["uri"]}, "dictionary group")
    # Both URIs are raw at a full commit (#89). pinned-uris-test proves the
    # commit is on HEAD and its bytes hash to dict and desc.
    require(re.fullmatch(r"https://raw\.githubusercontent\.com/walkthru-earth/opensensor-space-firmware/[0-9a-f]{40}/.+",
                         document["dictionary"]["uri"]) is not None, "dictionary.uri is raw at a full commit")
    require(re.fullmatch(r"https://raw\.githubusercontent\.com/walkthru-earth/opensensor-space-firmware/[0-9a-f]{40}/"
                         + re.escape(f"docs/shared/templates/{templates}/{dictionary['sha256']}.json"),
                         document["template"]["href"]) is not None,
            "template.href is the published template of dict at a full commit")
    require(document["template"]["sha256"] == dictionary["descriptor_sha256"], "template.sha256 is desc")
    require(document["thing"]["deployment_id"] == document["acquisition"]["calibration_id"] == "unknown",
            "no fabricated deployment or calibration")
    require(document["thing"]["mobility"] == "fixed", "a board Thing defaults to fixed")
    require(document["thing"]["board"] == templates, "thing.board is the template folder slug")
    require(document["acquisition"]["sample_interval_ms"] == 10000, "sample interval")
    require(document["acquisition"]["time_semantics"] != "", "time semantics")
    require(json.loads(document["acquisition"]["config"])["sample_interval_ms"] == 10000,
            "acquisition.config is the configuration JSON as a string")
    require(document["file"] == {"rotation_interval_s": rotation, "row_groups": row_groups,
                                 "row_group_rows_max": 90,
                                 "compression": "LZ4_RAW" if compressed else "UNCOMPRESSED",
                                 "purpose": "telemetry"}, "file group")


def check_board(executable: Path, firmware: Path, dictionary: dict, templates: str) -> str:
    """The fixture's `footer` mode prints what the firmware builds for a full
    one hour LZ4 file. Validate it, prove each mutation fails and return it."""
    import subprocess

    dict_sha, desc = board_digests(firmware)
    require((dict_sha, desc) == (dictionary["sha256"], dictionary["descriptor_sha256"]),
            "compiled digests equal the digest headers")
    text = subprocess.check_output([str(executable), "footer"], text=True)
    found = kv_problems({KEY: text, GEO_KEY: GEO_NO_BBOX}, dict_sha=dict_sha, desc=desc)
    require(not found, "board footer: " + "; ".join(found))
    check_document(json.loads(text), dictionary, templates, compressed=True, rotation=3600, row_groups=4)
    # template.href names the file #86 publishes, whose SHA-256 is desc.
    import hashlib

    published = (Path(__file__).resolve().parents[1] / "docs/shared/templates" / templates /
                 f"{dict_sha}.json")
    require(published.is_file() and hashlib.sha256(published.read_bytes()).hexdigest() == desc,
            f"template.href target {published.name} is published with SHA-256 desc")
    for line in mutation_proof(text, dict_sha=dict_sha, desc=desc):
        print(f"PASS mutation {line}", flush=True)
    print(f"PASS {dictionary['schema']} opensensor footer {len(text.encode())} bytes, "
          f"valid against footer.json {SPEC}", flush=True)
    return text
