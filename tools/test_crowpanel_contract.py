"""Check the CrowPanel SEN63C dictionary and synthetic files with two readers.

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

from h3_library import library_flags

import duckdb
import board_template
import footer_contract
import live_key_fixture
import pyarrow as pa
import pyarrow.parquet as pq
from host_cxx import CXX_STD

FIRMWARE = Path(__file__).resolve().parents[1] / "firmware/esp-idf-crowpanel-epaper/logger"
TEMPLATES = "elecrow-crowpanel-epaper-42"
MASS = ("pm1_mass_ug_m3", "pm25_mass_ug_m3", "pm4_mass_ug_m3", "pm10_mass_ug_m3")
MASS_VALUES = (1.2, 3.4, 5.6, 7.8)
# SHA-256 of the (name, type) pairs of the first 39 fields, the schema v1
# Thing baseline. Fields append only, so later fields never change it.
BASELINE_FIELDS = 39
BASELINE_SHA256 = "c18588b4d31725ecd24cf1f6422a8799eb31e4e336addc2efd8d651e715fe1aa"


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def close(value: float | None, expected: float) -> bool:
    return value is not None and abs(value - expected) < 1e-5


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
    bbox = footer_contract.check_location(path, located)
    dict_sha, desc = footer_contract.board_digests(FIRMWARE)
    footer = footer_contract.check_file(path, dict_sha=dict_sha, desc=desc, bbox=bbox)
    footer_contract.check_document(footer, dictionary, TEMPLATES, compressed=compressed, rotation=900, row_groups=1)
    require(dictionary["schema"] == "crowpanel-epaper-telemetry-v1", "schema identity")
    require(dictionary["dictionary"] == "crowpanel-epaper-telemetry-v1", "dictionary identity")
    require(parquet.metadata.created_by ==
            f"opensensor-space-firmware version 0.3 (build {dictionary['firmware']})", "CrowPanel writer identity")
    config = json.loads(footer["acquisition"]["config"])
    require(config["sample_interval_ms"] == 10000 and config["sen6x_model"] == "SEN63C" and
            config["pm_warmup_us"] == 30000000 and config["co2_warmup_us"] == 24000000 and
            config["stale_after_us"] == 3000000, "configuration")
    require((config["sen6x_i2c_sda"], config["sen6x_i2c_scl"], config["sen6x_address"], config["sen6x_i2c_hz"]) ==
            (8, 9, 0x6B, 100000), "board I2C mapping")
    group = parquet.metadata.row_group(0)
    require(parquet.metadata.num_row_groups == 1 and group.num_rows == 90, "row groups")
    by_name = {field["name"]: i for i, field in enumerate(fields)}
    require([(column.column_index, column.descending) for column in group.sorting_columns] ==
            [(by_name["sequence"], False)], "sequence ordering declaration")
    sequence = group.column(by_name["sequence"]).statistics
    require((sequence.min, sequence.max, sequence.null_count) == (0, 89, 0), "sequence bounds")
    ambient = group.column(by_name["ambient_temperature_c"]).statistics
    require(ambient.null_count == 3 and close(ambient.min, -3.7) and close(ambient.max, -3.7),
            "signed temperature bounds")
    for name, nulls in (("relative_humidity_percent", 4), ("pm25_mass_ug_m3", 5), ("co2_ppm", 6),
                        ("sen6x_device_status", 1)):
        require(group.column(by_name[name]).statistics.null_count == nulls, f"null count: {name}")
    utc = group.column(by_name["event_time_utc_us"]).statistics
    require(utc.null_count == (0 if anchored else 90) and utc.has_min_max == anchored, "UTC bounds")
    for i in range(len(fields)):
        require(group.column(i).compression == ("LZ4" if compressed else "UNCOMPRESSED"), "codec")
    table = pa.table({name: column.cast(pa.int64()) if name in utc_fields else column
                      for name, column in zip(table.column_names, table.columns, strict=True)})
    status = {0: 0, 1: 1, 2: 2, 3: 3, 4: 3}
    for i, row in enumerate(table.to_pylist()):
        now = 10000000 + i * 10000000
        require(row["schema_version"] == 1 and row["sequence"] == i, "row identity")
        require(row["monotonic_us"] == now and row["scheduled_us"] == now - 123 and
                row["sample_jitter_us"] == 123, "synthetic cadence")
        require(row["collection_completed_mono_us"] == now + 1234, "collection completion")
        require(row["sen6x_status"] == status.get(i, 4), "SEN63C status gates")
        require(row["sen6x_device_status"] == (None if i == 0 else 0x800 if i == 3 else 0x40 if i == 4 else 0),
                "device status bits")
        for name, value in zip(MASS, MASS_VALUES, strict=True):
            require(row[name] is None if i < 5 else close(row[name], value), f"mass mapping: {name}")
        require(row["co2_ppm"] == (None if i < 5 or i == 6 else 612), "CO2 validity")
        require(row["ambient_temperature_c"] is None if i in (0, 2, 4) else
                close(row["ambient_temperature_c"], -3.7), "temperature through warm-up and PM error")
        require(row["relative_humidity_percent"] is None if i in (0, 2, 4, 5) else
                close(row["relative_humidity_percent"], 63.4), "RH validity and range")
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


def check_phone_keys(firmware: Path) -> None:
    """board.json declares the page 7 keys the shared epaper_ui settings parse (#184)."""
    shared = firmware.parents[1] / "common" / "epaper_ui"
    header = (shared / "include" / "epaper_ui" / "settings.h").read_text()
    source = (shared / "src" / "settings.cpp").read_text()
    block = re.search(r"kPhoneKeys\{(.*?)\};", header, re.S)
    require(block is not None, "settings.h has no kPhoneKeys")
    names = re.findall(r'"([a-z0-9_.]+)"', block.group(1))
    keys = json.loads((firmware / "board.json").read_text())["config_keys"]
    require(sorted(names) == sorted(keys), f"phone keys {names} differ from board.json {sorted(keys)}")

    def listed(array: str) -> list[str]:
        found = re.search(array + r"\{(.*?)\};", source, re.S)
        require(found is not None, f"settings.cpp has no {array}")
        # Every string literal counts, so a value with a dash or any other
        # character can never slip past the comparison.
        body = found.group(1)
        values = re.findall(r'"((?:[^"\\]|\\.)*)"', body)
        rest = re.sub(r'"((?:[^"\\]|\\.)*)"', "", body)
        require(re.fullmatch(r"[\s,]*", rest) is not None,
                f"{array} holds something other than string literals")
        return values

    for key, array in (("panel.theme", "kThemeKeys"), ("panel.refresh_s", "kRefreshKeys"),
                       ("panel.start_page", "kStartKeys"), ("panel.sleep_s", "kSleepKeys")):
        require(keys[key]["type"] == "enum" and keys[key]["values"] == listed(array),
                f"{key} values differ from {array}")
    constant = {name: int(value) for name, value in
                re.findall(r"constexpr std::u?int16_t (k\w+) = (-?\d+);", header)}
    require(keys["sen6x.altitude_m"]["values"] == {"min": 0, "max": constant["kAltitudeMaxM"]},
            "sen6x.altitude_m range differs from kAltitudeMaxM")
    require(keys["sen6x.temp_offset_dc"]["values"] == {"min": constant["kTempOffsetMinDc"],
                                                       "max": constant["kTempOffsetMaxDc"]},
            "sen6x.temp_offset_dc range differs from kTempOffsetMinDc and kTempOffsetMaxDc")
    require(keys["sen6x.co2_asc"]["type"] == "boolean", "sen6x.co2_asc is a boolean")
    require(keys["sen6x.fan_clean"] == {**keys["sen6x.fan_clean"], "type": "boolean", "applies": "action"},
            "sen6x.fan_clean is an action")
    print(f"PASS CrowPanel page 7 keys match board.json, {len(keys)} keys", flush=True)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sanitize", action="store_true")
    parser.add_argument("--dictionary-out", type=Path)
    parser.add_argument("--write-template", action="store_true",
                        help="regenerate telemetry_template.h and telemetry_descriptor_digest.h")
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    firmware = FIRMWARE
    check_phone_keys(firmware)
    common = root / "firmware/common"
    with tempfile.TemporaryDirectory(prefix="crowpanel-contract-") as temporary:
        directory = Path(temporary)
        executable = directory / "fixture"
        command = ["clang++", CXX_STD, "-Wall", "-Wextra", "-Werror"]
        for include in ("src", "sensors/src", "sensors/modules", "logger/src", "connectivity/src", "runtime/src",
                        "gnss/src", "power/src"):
            command += ["-I", str(common / include)]
        if args.sanitize:
            command += ["-fsanitize=address,undefined", "-fno-sanitize-recover=all"]
        command += [*library_flags(root, directory, args.sanitize),
                    str(root / "tools/test_crowpanel_contract.cpp"),
                    str(common / "logger/src/os_logger_live.cpp"),
                    str(common / "gnss/src/os_gnss.cpp"),
                    str(common / "src/parquet_writer.cpp"), str(common / "src/lz4_codec.cpp"),
                    "-o", str(executable)]
        subprocess.run(command, check=True)
        dictionary = json.loads(subprocess.check_output([str(executable), "dictionary"], text=True))
        require(dictionary["sha256"] == hashlib.sha256((firmware / "telemetry_fields.inc").read_bytes()).hexdigest(),
                "stale compiled dictionary digest")
        if board_template.run(executable, firmware, dictionary, [firmware / "main.cpp"], args.write_template,
                              "pixi run python tools/test_crowpanel_contract.py --write-template") is None:
            return
        fields = dictionary["fields"]
        names = {field["name"] for field in fields}
        # Schema v1 is the Thing baseline of this board. Later fields append
        # only, so this 39-column prefix never changes.
        baseline = hashlib.sha256(json.dumps([(f["name"], f["type"]) for f in fields[:BASELINE_FIELDS]],
                                             separators=(",", ":")).encode()).hexdigest()
        require(len(fields) >= BASELINE_FIELDS and baseline == BASELINE_SHA256,
                f"39-column baseline changed, got {baseline}")
        print(f"PASS CrowPanel 39-column baseline {baseline[:8]}", flush=True)
        require(len(fields) == len(names) == 39, "unique versioned fields")
        require([f["live"] for f in fields if f["live"]] == [
            "seq", "mono", "utc", "sen", "m1", "m25", "m4", "m10", "t", "rh", "co2"], "LIVE keys in field order")
        require([(f["name"], f["type"], f["decimals"]) for f in fields if f["procedure"] == "sen63c"] == [
            ("sen6x_status", 1, 0), ("sen6x_device_status", 2, 0), ("pm1_mass_ug_m3", 4, 1),
            ("pm25_mass_ug_m3", 4, 1), ("pm4_mass_ug_m3", 4, 1), ("pm10_mass_ug_m3", 4, 1),
            ("ambient_temperature_c", 4, 1), ("relative_humidity_percent", 4, 1), ("co2_ppm", 1, 0)],
                "SEN63C Float mass columns, never the Int32 Plantower columns")
        inc = (firmware / "telemetry_fields.inc").read_text()
        rows = re.findall(r'^FIELD\((\w+), .*, "([a-z0-9_]*)", ([0-9])\)$', inc, re.M)
        require(rows == [(f["name"], f["live"], str(f["decimals"])) for f in fields],
                "dictionary carries every LIVE key and decimals")
        require(all(f["decimals"] == 0 if f["type"] != 4 else 0 <= f["decimals"] <= live_key_fixture.MAX_LIVE_DECIMALS
                    for f in fields),
                f"decimals is 0 for integer fields and at most {live_key_fixture.MAX_LIVE_DECIMALS} for Float")
        require([f["field_id"] for f in fields] == list(range(1, 40)), "append-only 1-based field ids")
        require(len({field["property"] for field in fields}) == len(fields), "property identifiers")
        require(not names.intersection({"pm25_atmospheric_ug_m3", "particles_gt03_per_01l", "battery_mv",
                                        "imu_temperature_c", "touch_points"}),
                "no Plantower, battery or CoreS3 columns")
        require(dictionary["firmware"] == "idf-crowpanel-parquet-v0.2", "firmware identity")
        footer_contract.check_board(executable, firmware, dictionary, TEMPLATES)
        live = subprocess.check_output([str(executable), "live"], text=True).strip()
        live_key_fixture.assert_rejects_drift("crowpanel", dictionary["sha256"], dictionary["firmware"], live)
        live_key_fixture.check("crowpanel", dictionary["sha256"], dictionary["firmware"], live)
        print("PASS CrowPanel LIVE keys match the frozen fixture for digest " + dictionary["sha256"][:8], flush=True)
        live_key_fixture.check_key_meaning("crowpanel", dictionary["sha256"], fields)
        print("PASS each LIVE key maps to one field across all digests and is in the protocol LIVE table", flush=True)
        for field in fields:
            require(re.fullmatch(r"[a-z][a-z0-9_]*", field["name"]) is not None, "safe field name")
            require(all(field[key] for key in ("procedure", "unit", "validity")), "complete metadata")
            require(re.fullmatch(r"http://vocab\.nerc\.ac\.uk/(collection/P07/current/[A-Z0-9]+|standard_name/[a-z][a-z0-9_]*)/"
                                 r"|urn:walkthru-earth:property:[a-z][a-z0-9_]*", field["property"]) is not None
                    and re.search(r"crowpanel|elecrow|epaper|uc8276|esp32|espressif|sen6|sensirion|pms|plantower",
                                  field["property"]) is None,
                    "board neutral property")
        for anchored in (False, True):
            for compressed in (False, True):
                for located in (False, True):
                    path = directory / f"{anchored}-{compressed}-{located}.parquet"
                    subprocess.run([str(executable), str(path), "lz4" if compressed else "none",
                                    "anchored" if anchored else "unsynced",
                                    "located" if located else "unset"], check=True)
                    check_file(path, dictionary, anchored, compressed, located)
                    print(f"PASS CrowPanel 90 x 39: anchored={anchored} lz4={compressed} location={located}, "
                          "both readers", flush=True)
        if args.dictionary_out:
            with args.dictionary_out.open("x") as output:
                json.dump(dictionary, output, indent=2)
                output.write("\n")
    print("PASS SEN63C mapping, null gates, dictionary, clock epochs and provenance")


if __name__ == "__main__":
    main()
