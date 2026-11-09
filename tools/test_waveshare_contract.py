"""Check the Waveshare PMS5003T dictionary and synthetic files with two readers.

Host fixtures establish field mapping and file contracts, not SD endurance,
power-loss durability, environmental accuracy or device sampling timing.
"""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import tempfile

from test_location import compile_h3

import duckdb
import board_template
import footer_contract
import live_key_fixture
import pyarrow as pa
import pyarrow.parquet as pq
from host_cxx import CXX_STD

FIRMWARE = Path(__file__).resolve().parents[1] / "firmware/esp-idf-waveshare-sim7670g/logger"

def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def check_file(path: Path, dictionary: dict, anchored: bool, compressed: bool, located: bool) -> None:
    parquet = pq.ParquetFile(path)
    table = parquet.read()
    fields = dictionary["fields"]
    require(table.num_rows == 90, "row count")
    require(table.column_names == [field["name"] for field in fields], "dictionary order")
    utc_fields = {field["name"] for field in fields if field["unit"] == "us_since_unix_epoch"}
    require(utc_fields == {"event_time_utc_us", "clock_anchor_utc_us"}, "UTC annotation scope")
    types = {1: pa.int32(), 2: pa.int64(), 4: pa.float32(), 6: pa.binary()}
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
    footer_contract.check_document(footer, dictionary, "waveshare-esp32-s3-sim7670g", compressed=compressed,
                                   rotation=900, row_groups=1)
    require(dictionary["schema"] == "waveshare-sim7670g-telemetry-v6", "schema identity")
    require(dictionary["dictionary"] == "waveshare-sim7670g-telemetry-v8", "dictionary identity")
    require(parquet.metadata.created_by ==
            f"opensensor-space-firmware version 0.3 (build {dictionary['firmware']})", "Waveshare writer identity")
    config = json.loads(footer["acquisition"]["config"])
    require(config["sample_interval_ms"] == 10000 and config["pms_stale_after_ms"] == 5000 and
            config["pms_warmup_us"] == 30000000 and config["pms_model"] == "PMS5003T", "configuration")
    require((config["pms_uart_rx"], config["pms_uart_tx"], config["pms_baud"]) == (1, 2, 9600),
            "board UART mapping")
    require(config["battery_health"] == {
        "source": "max17048-vcell", "external_power_mv": 4240, "external_power_hysteresis_mv": 20,
        "low_enter_mv": 3500, "critical_enter_mv": 3350, "hysteresis_mv": 75,
        "debounce_samples": 3, "settling_minutes": 60, "calibration": "uncalibrated-defaults",
        "critical_action": "finalize-open-file-once"}, "battery health thresholds")
    group = parquet.metadata.row_group(0)
    require(parquet.metadata.num_row_groups == 1 and group.num_rows == 90, "row groups")
    by_name = {field["name"]: i for i, field in enumerate(fields)}
    require([(column.column_index, column.descending) for column in group.sorting_columns] ==
            [(by_name["sequence"], False)], "sequence ordering declaration")
    sequence = group.column(by_name["sequence"]).statistics
    require((sequence.min, sequence.max, sequence.null_count) == (0, 89, 0), "sequence bounds")
    ambient = group.column(by_name["ambient_temperature_c"]).statistics
    require(ambient.null_count == 5 and abs(ambient.min + 3.7) < 1e-6 and
            abs(ambient.max + 3.7) < 1e-6, "signed temperature bounds")
    utc = group.column(by_name["event_time_utc_us"]).statistics
    require(utc.null_count == (0 if anchored else 90) and utc.has_min_max == anchored, "UTC bounds")
    for name, expected, nulls in (("battery_mv", 4000, 2), ("battery_percent", 75, 3)):
        stats = group.column(by_name[name]).statistics
        require(stats.null_count == nulls and stats.min == stats.max == expected,
                f"battery null and valid statistics: {name}")
    for i in range(len(fields)):
        require(group.column(i).compression == ("LZ4" if compressed else "UNCOMPRESSED"), "codec")
    table = pa.table({name: column.cast(pa.int64()) if name in utc_fields else column
                      for name, column in zip(table.column_names, table.columns, strict=True)})
    for i, row in enumerate(table.to_pylist()):
        now = 10000000 + i * 10000000
        require(row["schema_version"] == 6 and row["sequence"] == i, "row identity")
        require(row["monotonic_us"] == now and row["scheduled_us"] == now - 123 and
                row["sample_jitter_us"] == 123, "synthetic cadence")
        require(row["collection_completed_mono_us"] == now + 1234, "collection completion")
        require(row["pms_received_mono_us"] == (now - 250000 if i else None), "frame receipt")
        require(row["pms_status"] == ([0, 1, 2, 3, 5][i] if i < 5 else 4), "PMS gates")
        require(row["pms_frames"] == 1 and row["pms_checksum_errors"] == 2 and
                row["pms_length_errors"] == 3, "visible parser counters")
        for offset, name in enumerate(("pm1_cf1_ug_m3", "pm25_cf1_ug_m3", "pm10_cf1_ug_m3",
                                       "pm1_atmospheric_ug_m3", "pm25_atmospheric_ug_m3",
                                       "pm10_atmospheric_ug_m3", "particles_gt03_per_01l",
                                       "particles_gt05_per_01l", "particles_gt10_per_01l",
                                       "particles_gt25_per_01l")):
            require(row[name] == (offset + 1 if i >= 5 else None), f"PMS word mapping: {name}")
        require(row["sensor_serial_code"] == 202604081332, "exact provisioned serial code")
        require(row["pm25_batch_candidate_status"] == (0 if i == 5 else 2 if i < 5 else 3),
                "candidate gate")
        candidate = row["pm25_batch_candidate_ug_m3"]
        require(candidate is None if i < 6 else abs(candidate - 0.003964 * 7) < 1e-7,
                "count-based candidate, never raw mass multiplied")
        require(row["ambient_temperature_c"] is None if i < 5 else
                abs(row["ambient_temperature_c"] + 3.7) < 1e-6, "temperature validity")
        require(row["relative_humidity_percent"] is None if i < 5 or i == 6 else
                abs(row["relative_humidity_percent"] - 63.4) < 1e-5, "RH validity and range")
        require(row["gauge_status"] == (i if i < 2 else 1 if i == 2 else 2) and
                row["battery_mv"] == (None if i < 2 else 4000) and
                row["battery_percent"] == (None if i < 3 else 75),
                "absent, read-error, voltage-only and valid gauge values")
        raw_soc = row["battery_soc_raw_percent"]
        crate = row["battery_charge_rate_percent_per_h"]
        require((raw_soc is None) if i < 3 else raw_soc == 75.5, "uncapped raw SOC")
        require((crate is None) if i < 2 else abs(crate + 2.08) < 1e-5, "signed CRATE")
        require(row["gauge_status_flags"] == (None if i < 2 else 0x40) and
                row["gauge_hibernating"] == (None if i < 2 else 1), "per-register gauge diagnostics")
        require(row["battery_health_state"] == (None if i == 0 else 0 if i < 4 else 2),
                "battery health null, unknown until debounced, then ok")
        require(row["battery_minutes_to_empty"] == (None if i < 4 else 2177),
                "minutes to empty only once trusted")
        require(row["clock_status"] == row["clock_epoch"] == int(anchored), "clock provenance")
        require(row["clock_anchor_mono_us"] == (15000000 if anchored else None) and
                row["clock_anchor_utc_us"] == (1788890000000000 if anchored else None), "anchor")
        require(row["event_time_utc_us"] ==
                (1788890000000000 + (now - 15000000) if anchored else None), "UTC estimate")
    with duckdb.connect() as connection:
        other = connection.sql(
            "SELECT * REPLACE (epoch_us(event_time_utc_us) AS event_time_utc_us, "
            "epoch_us(clock_anchor_utc_us) AS clock_anchor_utc_us) "
            "FROM read_parquet(?, hive_partitioning=false)",
            params=[str(path)]).to_arrow_table()
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
    root = Path(__file__).resolve().parents[1]
    firmware = root / "firmware/esp-idf-waveshare-sim7670g/logger"
    common = root / "firmware/common"
    with tempfile.TemporaryDirectory(prefix="waveshare-contract-") as temporary:
        directory = Path(temporary)
        executable = directory / "fixture"
        command = ["clang++", CXX_STD, "-Wall", "-Wextra", "-Werror"]
        for include in ("src", "sensors/src", "sensors/modules", "logger/src", "connectivity/src", "runtime/src",
                        "location/src", "power/src"):
            command += ["-I", str(common / include)]
        if args.sanitize:
            command += ["-fsanitize=address,undefined", "-fno-sanitize-recover=all"]
        command += ["-I", str(common / "power/src"), "-I", str(directory / "h3"),
                    *compile_h3(root, directory, args.sanitize),
                    str(common / "location/src/os_location.cpp"),
                    str(root / "tools/test_waveshare_contract.cpp"),
                    str(common / "logger/src/os_logger_live.cpp"),
                    str(common / "power/src/os_battery_health.cpp"),
                    str(common / "src/parquet_writer.cpp"), str(common / "src/lz4_codec.cpp"),
                    str(common / "sensors/src/pms_frame.cpp"), "-o", str(executable)]
        subprocess.run(command, check=True)
        dictionary = json.loads(subprocess.check_output([str(executable), "dictionary"], text=True))
        require(dictionary["sha256"] == hashlib.sha256((firmware / "telemetry_fields.inc").read_bytes()).hexdigest(),
                "stale compiled dictionary digest")
        if board_template.run(executable, firmware, dictionary, [firmware / "main.cpp"], args.write_template,
                              "pixi run python tools/test_waveshare_contract.py --write-template") is None:
            return
        fields = dictionary["fields"]
        names = {field["name"] for field in fields}
        require(hashlib.sha256(json.dumps([(f["name"], f["type"]) for f in fields[:52]],
                                         separators=(",", ":")).encode()).hexdigest()
                == "01c9db4e628e33367f0bfd594afa10f088d5b1c75d3d64c8616fccdd23654209",
                "original 52-column prefix changed")
        # The Thing release (#83) is the field table baseline. Later fields
        # append only, so this 62-column prefix never changes.
        thing_baseline = hashlib.sha256(json.dumps([(f["name"], f["type"]) for f in fields[:62]],
                                                   separators=(",", ":")).encode()).hexdigest()
        require(thing_baseline == "c02fb9a145db666870c571b459ad2e86f862a4a296a70789e664727dc1c7593c",
                "Thing baseline 62-column prefix changed")
        require([(f["name"], f["type"], f["live"]) for f in fields[56:]] == [
            ("battery_soc_raw_percent", 4, ""), ("battery_charge_rate_percent_per_h", 4, ""),
            ("gauge_status_flags", 1, ""), ("gauge_hibernating", 1, ""),
            ("battery_health_state", 1, "bstate"), ("battery_minutes_to_empty", 1, "")],
                "schema v5 battery columns")
        require(len(fields) == len(names) == 62, "unique versioned fields")
        require([f["live"] for f in fields if f["live"]] == [
            "seq", "mono", "utc", "pms", "c1", "c25", "c10", "pm1", "pm25", "pm10", "n03", "n05", "n1", "n25",
            "t", "rh", "bat", "pct", "bstate"], "LIVE keys in field order")
        inc = (firmware / "telemetry_fields.inc").read_text()
        rows = re.findall(r'^FIELD\((\w+), .*, "([a-z0-9_]*)", ([0-9])\)$', inc, re.M)
        require(rows == [(f["name"], f["live"], str(f["decimals"])) for f in fields],
                "dictionary carries every LIVE key and decimals")
        require(all(f["decimals"] == 0 if f["type"] != 4 else 0 <= f["decimals"] <= live_key_fixture.MAX_LIVE_DECIMALS
                    for f in fields),
                f"decimals is 0 for integer fields and at most {live_key_fixture.MAX_LIVE_DECIMALS} for Float")
        require([f["field_id"] for f in fields] == list(range(1, 63)), "append-only 1-based field ids")
        require(len({field["property"] for field in fields}) == len(fields), "property identifiers")
        require(not names.intersection({"particles_gt50_per_01l", "particles_gt100_per_01l",
                                        "imu_temperature_c", "accel_x_g", "rtc_read_ok", "touch_points"}),
                "no invented PMS5003T bins or CoreS3 peripherals")
        require(dictionary["firmware"] == "idf-waveshare-parquet-v2.22", "firmware identity")
        footer_contract.check_board(executable, firmware, dictionary, "waveshare-esp32-s3-sim7670g")
        live = subprocess.check_output([str(executable), "live"], text=True).strip()
        live_key_fixture.assert_rejects_drift("waveshare", dictionary["sha256"], dictionary["firmware"], live)
        live_key_fixture.check("waveshare", dictionary["sha256"], dictionary["firmware"], live)
        print("PASS Waveshare LIVE keys match the frozen fixture for digest " + dictionary["sha256"][:8], flush=True)
        live_key_fixture.check_key_meaning("waveshare", dictionary["sha256"], fields)
        print("PASS each LIVE key maps to one field across all digests and is in the protocol LIVE table", flush=True)
        for field in fields:
            require(re.fullmatch(r"[a-z][a-z0-9_]*", field["name"]) is not None, "safe field name")
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
                    print(f"PASS Waveshare 90 x 62: anchored={anchored} lz4={compressed} location={located}; both readers", flush=True)
        if args.dictionary_out:
            with args.dictionary_out.open("x") as output:
                json.dump(dictionary, output, indent=2)
                output.write("\n")
    print("PASS PMS5003T mapping, null gates, dictionary, clock epochs and provenance")


if __name__ == "__main__":
    main()
