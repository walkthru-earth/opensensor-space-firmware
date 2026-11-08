"""Test the exact firmware dictionary, row layout and clock mapping on the host.

Optional --dictionary-out exports a machine-readable dictionary exclusively.
Synthetic fixtures do not establish device timing, SD durability or OGC compliance.
"""

from __future__ import annotations

import argparse
import contextlib
import datetime
import hashlib
import io
import json
from pathlib import Path
import re
import subprocess
import tempfile

from test_location import compile_h3
from types import SimpleNamespace
from unittest.mock import patch

import duckdb
import board_template
import footer_contract
import live_key_fixture
import pyarrow as pa
import pyarrow.parquet as pq
import parquet_device
from host_cxx import CXX_STD

FIRMWARE = Path(__file__).resolve().parents[1] / "firmware/esp-idf-cores3/bringup"

def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def check_file(path: Path, dictionary: dict, anchored: bool, compressed: bool, located: bool) -> None:
    parquet = pq.ParquetFile(path)
    table = parquet.read()
    fields = dictionary["fields"]
    require(table.num_rows == 90, "row count")
    require(table.column_names == [f["name"] for f in fields], "dictionary/schema order")
    types = {1: pa.int32(), 2: pa.int64(), 4: pa.float32(), 6: pa.binary()}
    utc_fields = {f["name"] for f in fields if f["unit"] == "us_since_unix_epoch"}
    require(utc_fields == {"event_time_utc_us", "clock_anchor_utc_us"}, "UTC instant fields")
    for field, definition in zip(parquet.schema_arrow, fields, strict=True):
        expected = (pa.timestamp("us", tz="UTC") if field.name in utc_fields else
                    pa.uint8() if definition["unit"] == "uint8_code" else types[definition["type"]])
        require(field.type == expected and field.nullable, f"schema: {field.name}")
        require(field.metadata[b"PARQUET:field_id"] == str(definition["field_id"]).encode(),
                f"Iceberg field_id: {field.name}")
    # The `opensensor` key, valid against the pinned footer.json (#88), and
    # the `geo` key with the bbox of the rows (#83).
    bbox = footer_contract.check_location(path, located)
    dict_sha, desc = footer_contract.board_digests(FIRMWARE)
    footer = footer_contract.check_file(path, dict_sha=dict_sha, desc=desc, bbox=bbox)
    footer_contract.check_document(footer, dictionary, "m5stack-cores3", compressed=compressed,
                                   rotation=900, row_groups=1)
    require(dictionary["schema"] == "cores3-telemetry-v6", "schema version")
    require(dictionary["dictionary"] == "cores3-telemetry-v7",
            "dictionary version tracks the field table bytes, now with location_source and geometry")
    require(parquet.metadata.created_by == f"opensensor-space-firmware version 0.3 (build {dictionary['firmware']})",
            "firmware identity in created_by")
    group = parquet.metadata.row_group(0)
    require(parquet.metadata.num_row_groups == 1 and group.num_rows == 90, "one 90-row group")
    require([(s.column_index, s.descending) for s in group.sorting_columns] == [(fields.index(
        next(f for f in fields if f["name"] == "sequence")), False)], "rows declared sorted by sequence")
    by_name = {f["name"]: i for i, f in enumerate(fields)}
    sequence = group.column(by_name["sequence"]).statistics
    require((sequence.min, sequence.max, sequence.null_count) == (0, 89, 0), "sequence statistics")
    utc = group.column(by_name["event_time_utc_us"]).statistics
    require(utc.null_count == (0 if anchored else 90) and utc.has_min_max == anchored, "UTC statistics")
    if anchored:
        # PyArrow presents TIMESTAMP statistics as datetimes (microseconds).
        epoch = datetime.datetime(1970, 1, 1, tzinfo=datetime.timezone.utc)
        micros = [(bound - epoch) // datetime.timedelta(microseconds=1) for bound in (utc.min, utc.max)]
        require(micros == [1788890005000000, 1788890895000000], "UTC range in footer")
    ambient = group.column(by_name["ambient_temperature_c"]).statistics
    require(ambient.null_count == 90 and not ambient.has_min_max, "all-null column has no bounds")
    # Cast the annotated instants back to their INT64 microseconds for value checks.
    table = pa.table({name: column.cast(pa.int64()) if name in utc_fields else column
                      for name, column in zip(table.column_names, table.columns)})
    config = json.loads(footer["acquisition"]["config"])
    require(config["sample_interval_ms"] == 10000 and config["pms_stale_after_ms"] == 5000,
            "configuration contract")
    for i in range(len(fields)):
        require(parquet.metadata.row_group(0).column(i).compression ==
                ("LZ4" if compressed else "UNCOMPRESSED"), "column codec")
    for i, row in enumerate(table.to_pylist()):
        now = 20000000 + i * 10000000
        require(row["schema_version"] == dictionary["schema_version"] == 6 and row["sequence"] == i, "identity")
        require(row["collection_completed_mono_us"] == now + 1234, "completion time")
        require(row["pms_received_mono_us"] == (now - 250000 if i else None), "PMS receipt")
        require(row["clock_anchor_mono_us"] == (15000000 if anchored else None), "anchor mono")
        require(row["clock_anchor_utc_us"] == (1788890000000000 if anchored else None), "anchor UTC")
        require(row["event_time_utc_us"] ==
                (1788890000000000 + (now - 15000000) if anchored else None), "UTC estimate")
        require(row["clock_epoch"] == row["clock_status"] == int(anchored), "clock status")
        require(row["ambient_temperature_c"] is None and row["battery_current_ma"] is None,
                "unsupported measurements stay null")
    with duckdb.connect() as connection:
        # DuckDB presents the annotated columns as TIMESTAMPTZ (microseconds).
        other = connection.execute(
            "SELECT * REPLACE (epoch_us(event_time_utc_us) AS event_time_utc_us, "
            "epoch_us(clock_anchor_utc_us) AS clock_anchor_utc_us) "
            "FROM read_parquet(?, hive_partitioning=false)", [str(path)]).to_arrow_table()
    # check_location compares geometry, which DuckDB presents as GEOMETRY.
    table = table.drop_columns([footer_contract.GEOMETRY])
    other = other.drop_columns([footer_contract.GEOMETRY])
    require(table.equals(other, check_metadata=False), "PyArrow/DuckDB values and nulls")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sanitize", action="store_true")
    parser.add_argument("--dictionary-out", type=Path)
    parser.add_argument("--write-template", action="store_true",
                        help="regenerate telemetry_template.h and telemetry_descriptor_digest.h")
    args = parser.parse_args()
    # Unrelated asynchronous status lines must not terminate a schema reply.
    response = iter(["PARQUET STATUS buffered=2", "PARQUET SCHEMA BEGIN",
                     "PARQUET FIELD name=sequence", "PARQUET SCHEMA END", "AFTER"])
    with patch.object(parquet_device, "send_command") as send, \
            patch.object(parquet_device, "lines_until", return_value=response), \
            contextlib.redirect_stdout(io.StringIO()):
        require(parquet_device.command(None, SimpleNamespace(text="parquet schema", timeout=1)) == 0,
                "schema command completion")
        send.assert_called_once_with(None, "parquet schema")
        require(next(response) == "AFTER", "schema command consumes through END only")
    root = Path(__file__).resolve().parents[1]
    firmware = root / "firmware/esp-idf-cores3/bringup"
    common = root / "firmware/common/src"
    with tempfile.TemporaryDirectory(prefix="m5-contract-") as temporary:
        directory = Path(temporary)
        executable = directory / "fixture"
        command = ["clang++", CXX_STD, "-Wall", "-Wextra", "-Werror"]
        for include in (common, root / "firmware/common/logger/src",
                        root / "firmware/common/connectivity/src",
                        root / "firmware/common/runtime/src", root / "firmware/common/location/src",
                        root / "firmware/common/power/src",
                        directory / "h3"):
            command += ["-I", str(include)]
        if args.sanitize:
            command += ["-fsanitize=address,undefined", "-fno-sanitize-recover=all"]
        command += [*compile_h3(root, directory, args.sanitize),
                    str(root / "firmware/common/location/src/os_location.cpp"),
                    str(root / "tools/telemetry_contract_fixture.cpp"),
                    str(root / "firmware/common/logger/src/os_logger_live.cpp"),
                    str(common / "parquet_writer.cpp"), str(common / "lz4_codec.cpp"),
                    "-o", str(executable)]
        subprocess.run(command, check=True)
        dictionary = json.loads(subprocess.check_output([str(executable), "dictionary"], text=True))
        require(dictionary["sha256"] == hashlib.sha256((firmware / "telemetry_fields.inc").read_bytes()).hexdigest(),
                "stale compiled dictionary digest")
        fields = dictionary["fields"]
        if board_template.run(executable, firmware, dictionary, [firmware / "telemetry_logger.cpp"], args.write_template,
                              "pixi run python tools/test_telemetry_contract.py --write-template") is None:
            return
        baseline = [(f["name"], f["type"]) for f in fields[:73]]
        # Golden names/types/order from the 47d8f83 hardware-baseline schema, with
        # the two UTC instants renamed to *_utc_us for TIMESTAMP(MICROS, UTC).
        require(hashlib.sha256(json.dumps(baseline, separators=(",", ":")).encode()).hexdigest()
                == "9dc2fffac111ff4e7b79b763fbbcb300cad3e2040e1a4c9b6323c14843f534ec",
                "original 73-column schema changed")
        require(hashlib.sha256(json.dumps([(f["name"], f["type"]) for f in fields[:77]],
                                         separators=(",", ":")).encode()).hexdigest()
                == "511b231ecdbf8f93fc3acbca42d5fceef7c3808551af1d035866cc0da4366135",
                "original 77-column prefix changed")
        # The Thing release (#83) is the field table baseline. Later fields
        # append only, so this 81-column prefix never changes.
        thing_baseline = hashlib.sha256(json.dumps([(f["name"], f["type"]) for f in fields[:81]],
                                                   separators=(",", ":")).encode()).hexdigest()
        require(thing_baseline == "c3ebd39ab723436c435c0fbec93cba449fae7544a7c3a04a1fbceb43ea0de4c3",
                "Thing baseline 81-column prefix changed")
        inc = (firmware / "telemetry_fields.inc").read_text()
        rows = re.findall(r'^FIELD\((\w+), .*, "([a-z0-9_]*)", ([0-9])\)$', inc, re.M)
        require(rows == [(f["name"], f["live"], str(f["decimals"])) for f in fields],
                "dictionary carries every LIVE key and decimals")
        require(all(f["decimals"] == 0 if f["type"] != 4 else 0 <= f["decimals"] <= live_key_fixture.MAX_LIVE_DECIMALS
                    for f in fields),
                f"decimals is 0 for integer fields and at most {live_key_fixture.MAX_LIVE_DECIMALS} for Float")
        require(dictionary["firmware"] == "idf-cores3-parquet-v6.25", "firmware identity")
        footer_contract.check_board(executable, firmware, dictionary, "m5stack-cores3")
        live = subprocess.check_output([str(executable), "live"], text=True).strip()
        live_key_fixture.assert_rejects_drift("cores3", dictionary["sha256"], dictionary["firmware"], live)
        require(live_key_fixture.check("cores3", dictionary["sha256"], dictionary["firmware"], live) ==
                [f["live"] for f in fields if f["live"]], "rendered LIVE keys follow the dictionary")
        print("PASS CoreS3 LIVE keys match the frozen fixture for digest " + dictionary["sha256"][:8], flush=True)
        live_key_fixture.check_key_meaning("cores3", dictionary["sha256"], fields)
        print("PASS each LIVE key maps to one field across all digests and is in the protocol LIVE table", flush=True)
        require(len(fields) == len({f["name"] for f in fields}) == 81, "unique dictionary fields")
        require([f["field_id"] for f in fields] == list(range(1, 82)), "append-only 1-based field ids")
        require(len({f["property"] for f in fields}) == 81, "unique property identifiers")
        for field in fields:
            require(re.fullmatch(r"[a-z][a-z0-9_]*", field["name"]) is not None, "safe field token")
            require(all(field[key] for key in ("procedure", "unit", "validity")), "complete metadata")
            require(re.fullmatch(r"http://vocab\.nerc\.ac\.uk/(collection/P07/current/[A-Z0-9]+|standard_name/[a-z][a-z0-9_]*)/"
                                 r"|urn:walkthru-earth:property:[a-z][a-z0-9_]*", field["property"]) is not None
                    and re.search(r"cores3|waveshare|sim76|simcom|m5|esp32|espressif|pms|plantower|bmi|bmm|bosch|ltr|lite_on|liteon|axp|x_powers|xpowers|bm8563|belling|ft6336|focaltech|max1704|maxim|sht|sensirion", field["property"]) is None,
                    "board neutral property")
        for anchored in (False, True):
            for compressed in (False, True):
                for located in (False, True):
                    path = directory / f"{anchored}-{compressed}-{located}.parquet"
                    subprocess.run([str(executable), str(path), "lz4" if compressed else "none",
                                    "anchored" if anchored else "unsynced",
                                    "located" if located else "unset"], check=True)
                    check_file(path, dictionary, anchored, compressed, located)
                    print(f"PASS 90 x 81 contract: anchored={anchored} lz4={compressed} location={located}; both readers", flush=True)
        if args.dictionary_out:
            with args.dictionary_out.open("x") as output:
                json.dump(dictionary, output, indent=2)
                output.write("\n")
    print("PASS dictionary digest, schema, nulls, clock correction and provenance")


if __name__ == "__main__":
    main()
