"""Board neutral footer, cadence and BLE name checks (#90).

Compiles firmware/common/logger/tests/test_footer.cpp with the shared writer.
The C++ part pins the default `opensensor` footer JSON (10000, 90, unknown,
#88), its escaping and its fail closed errors, and the OS- BLE name and mDNS
host. It then writes a file for a non air quality fixture board (no PMS, 30 s
period, SL- prefix). PyArrow and DuckDB read it here and its one footer key
validates against the pinned stac-opensensor footer.json.

A source check also pins the begin() gate. footer::check_config() runs in
logger::begin() before the writer state, the storage worker or any file open,
and a failure logs `PARQUET ERROR operation=footer-config` and returns false.
Removing the call, the log line or the early return fails here.

A second source check pins finalize_file() to footer::metadata(), the helper
the board contract fixtures finish through. Those fixtures write a geometry
column and read the raw Thrift key-value list, so opensensor before geo is
proven on the bytes, and swapping the keys in the logger call fails here.

A third source check pins the deployment id path (#148). collect()
snapshots it per sample, the open file keeps the first row's copy, the footer
writes that copy or "unknown", and fits_batch() closes the file when it
changes. Each listed mutant must fail the check.
"""
from __future__ import annotations

import argparse
from pathlib import Path
import re
import subprocess
import sys
import tempfile

import duckdb
import pyarrow.parquet as pq

sys.path.insert(0, str(Path(__file__).resolve().parent))
import footer_contract  # noqa: E402
from host_cxx import CXX_STD  # noqa: E402

# kDictSha and kDescSha of test_footer.cpp.
DICT_SHA = "0" * 63 + "1"
DESC_SHA = "0" * 63 + "2"

FOOTER_GATE = re.compile(
    r"if \(const auto footer_config = footer::check_config\(config\); "
    r"!footer_config\) \{\s*"
    r"oslog\.printf\(\"PARQUET ERROR operation=footer-config reason=%s \"\s*"
    r"\"logging=false\\n\",\s*"
    r"footer::error_name\(footer_config\.error\(\)\)\);\s*"
    r"return false;\s*\}")
# finalize_file() takes its key-value list from footer::metadata(), the same
# helper the board contract fixtures finish through, and passes it unchanged.
FOOTER_METADATA = re.compile(
    r"const auto metadata = footer::metadata\(writer_state->footer_json,\s*"
    r"writer_state->geo_json, geometry\);\s*"
    r"const auto started = esp_timer_get_time\(\);\s*"
    r"const auto result =\s*target\.writer\.finish\(metadata\.entries\.data\(\), metadata\.count,")
# Anything in begin() that opens a file or starts the code that does.
FILE_WORK = ("fopen(", "::open(", "opendir(", "mkdir(", "create_file(",
             "heap_caps_calloc(", "xTaskCreate(", "storage_worker")


def begin_body(source: str) -> str:
    start = source.find("\nbool begin(const Config &configuration")
    assert start >= 0, "logger::begin() not found in os_logger.cpp"
    end = source.find("\n}\n", start)
    assert end > start, "end of logger::begin() not found"
    return source[start:end]


def check_footer_gate(source: str) -> None:
    """footer::check_config() gates begin() before any file work (#88)."""
    body = begin_body(source)
    gates = list(FOOTER_GATE.finditer(body))
    assert len(gates) == 1, "begin() must call footer::check_config(config) once, " \
        "log PARQUET ERROR operation=footer-config reason=<name> logging=false " \
        "and return false"
    before = body[:gates[0].start()]
    early = [token for token in FILE_WORK if token in before]
    assert not early, f"begin() reaches {early} before footer::check_config()"
    assert "xTaskCreate(storage_worker" in body[gates[0].end():], \
        "storage worker start not found after the footer gate"


def prove_footer_gate(source: str) -> None:
    check_footer_gate(source)
    body = begin_body(source)
    gate = FOOTER_GATE.search(body)
    assert gate
    text = gate.group(0)
    mutants = {
        "call removed": source.replace(text, ""),
        "log removed": source.replace(text, text.replace(
            "operation=footer-config", "operation=footer")),
        "return removed": source.replace(text, text.replace("return false;", "")),
        "moved after the worker": source.replace(text, "").replace(
            "  if (xTaskCreate(storage_worker", text + "\n  if (xTaskCreate(storage_worker"),
    }
    for name, mutant in mutants.items():
        assert mutant != source, name
        try:
            check_footer_gate(mutant)
        except AssertionError:
            continue
        raise AssertionError(f"footer gate check missed the mutant: {name}")


def check_footer_metadata(source: str) -> None:
    """The logger writes opensensor then geo through footer::metadata() (#83)."""
    assert len(FOOTER_METADATA.findall(source)) == 1, \
        "finalize_file() must finish with footer::metadata(footer_json, geo_json, geometry) " \
        "and pass metadata.entries.data() and metadata.count unchanged"
    assert source.count("writer.finish(") == 1, "one writer.finish() call in os_logger.cpp"


def prove_footer_metadata(source: str) -> None:
    check_footer_metadata(source)
    text = FOOTER_METADATA.search(source).group(0)
    mutants = {
        "keys swapped": source.replace(text, text.replace(
            "writer_state->footer_json,", "writer_state->geo_json,", 1).replace(
            "writer_state->geo_json, geometry", "writer_state->footer_json, geometry", 1)),
        "geo always written": source.replace(text, text.replace(", geometry);", ", true);")),
        "count fixed": source.replace(text, text.replace("metadata.count,", "2,")),
        "inline array": source.replace(text, text.replace(
            "footer::metadata(", "my_metadata(")),
    }
    for name, mutant in mutants.items():
        assert mutant != source, name
        try:
            check_footer_metadata(mutant)
        except AssertionError:
            continue
        raise AssertionError(f"footer metadata check missed the mutant: {name}")


def function_body(source: str, signature: str) -> str:
    start = source.find(signature)
    assert start >= 0, f"{signature} not found"
    end = source.find("\n}\n", start)
    assert end > start, f"end of {signature} not found"
    return source[start:end]


# The deployment id path (#148). collect() snapshots it per sample,
# create_file() copies the first row's value into the open file,
# finalize_file() hands that copy to the footer, and fits_batch() closes the
# file when it changes, exactly as it does for the location.
DEPLOYMENT_WIRING = (
    ("void collect(std::int64_t now, std::int64_t scheduled)",
     "row.deployment = ::config::deployment_id();", 1),
    ("bool create_file(OutputFile &target", "target.deployment = first_row.deployment;", 1),
    ("bool finalize_file(OutputFile &target)", ".deployment = target.deployment.text.data()}", 1),
    ("bool fits_batch(const Sample &row, std::size_t count)", "same_file_labels(row, open);", 1),
    ("bool fits_batch(const Sample &row, std::size_t count)", "same_file_labels(row, previous);", 1),
)
FILE_LABELS = re.compile(
    r"\[\[nodiscard\]\] bool same_file_labels\(const A &a, const B &b\) \{\s*"
    r"return config::same_location\(a\.location, b\.location\) &&\s*"
    r"config::same_deployment\(a\.deployment, b\.deployment\);\s*\}")
FOOTER_DEPLOYMENT = ('json.text("deployment_id",\n'
                     '            present(ids.deployment) ? ids.deployment : config.deployment_id);')


def check_deployment(logger: str, labels: str, footer: str) -> None:
    """The deployment id is snapshotted per file and a change splits it."""
    for signature, text, count in DEPLOYMENT_WIRING:
        body = function_body(logger, signature)
        assert body.count(text) == count, f"{signature} must contain {text!r} {count} time(s)"
    assert "row.deployment = " not in function_body(
        logger, "bool fits_batch(const Sample &row, std::size_t count)"), \
        "fits_batch() never rewrites a sample's deployment id"
    assert logger.count("row.deployment =") == 1, "only collect() assigns row.deployment"
    assert logger.count("same_location(") == 0, \
        "the logger compares file labels only through same_file_labels()"
    assert len(FILE_LABELS.findall(labels)) == 1, \
        "same_file_labels() must compare the location and the deployment id"
    assert footer.count(FOOTER_DEPLOYMENT) == 1, \
        "the footer writes the file's deployment id, else Config::deployment_id"


def prove_deployment(logger: str, labels: str, footer: str) -> None:
    check_deployment(logger, labels, footer)
    label_text = FILE_LABELS.search(labels).group(0)
    mutants = {
        "open file ignores labels": (logger.replace(
            "same_file_labels(row, open);", "true;"), labels, footer),
        "batch ignores labels": (logger.replace(
            "same_file_labels(row, previous);", "true;"), labels, footer),
        "location only, open file": (logger.replace(
            "same_file_labels(row, open);",
            "config::same_location(row.location, open.location);"), labels, footer),
        "deployment not compared": (logger, labels.replace(label_text, label_text.replace(
            " &&\n         config::same_deployment(a.deployment, b.deployment)", "")), footer),
        "deployment not snapshotted": (logger.replace(
            "  row.deployment = ::config::deployment_id();\n", ""), labels, footer),
        "file keeps no snapshot": (logger.replace(
            "  target.deployment = first_row.deployment;\n", ""), labels, footer),
        "footer reads live value": (logger.replace(
            ".deployment = target.deployment.text.data()}", "}"), labels, footer),
        "footer ignores snapshot": (logger, labels, footer.replace(
            FOOTER_DEPLOYMENT, 'json.text("deployment_id", config.deployment_id);')),
    }
    for name, (logger_mutant, labels_mutant, footer_mutant) in mutants.items():
        assert (logger_mutant, labels_mutant, footer_mutant) != (logger, labels, footer), name
        try:
            check_deployment(logger_mutant, labels_mutant, footer_mutant)
        except AssertionError:
            continue
        raise AssertionError(f"deployment wiring check missed the mutant: {name}")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sanitize", action="store_true")
    args = parser.parse_args()
    root = Path(__file__).resolve().parent.parent
    common = root / "firmware/common"
    logger_source = (common / "logger/src/os_logger.cpp").read_text()
    prove_footer_gate(logger_source)
    prove_footer_metadata(logger_source)
    prove_deployment(logger_source,
                     (common / "logger/src/os_logger_location.h").read_text(),
                     (common / "logger/src/os_logger_footer.h").read_text())
    with tempfile.TemporaryDirectory(prefix="aq-logger-footer-") as directory:
        executable = Path(directory) / "footer-test"
        output = Path(directory) / "soil.parquet"
        command = ["clang++", CXX_STD, "-Wall", "-Wextra", "-Werror"]
        for library in ["src", "runtime/src", "connectivity/src", "logger/src",
                        "power/src", "location/src"]:
            command += ["-I", str(common / library)]
        command += [str(common / "logger/tests/test_footer.cpp"),
                    str(common / "src/parquet_writer.cpp"),
                    "-o", str(executable)]
        if args.sanitize:
            command += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
        subprocess.run(command, check=True)
        result = subprocess.run([str(executable), str(output)],
                                capture_output=True, text=True, check=True)
        assert result.stdout.split() == ["SL-89ab", "sl-89ab"], result.stdout

        parquet = pq.ParquetFile(output)
        footer = footer_contract.check_file(output, dict_sha=DICT_SHA, desc=DESC_SHA, geometry=False)
        acquisition, file = footer["acquisition"], footer["file"]
        assert acquisition["sample_interval_ms"] == 30000
        assert file == {"rotation_interval_s": 3600, "row_groups": 2, "row_group_rows_max": 90,
                        "compression": "UNCOMPRESSED", "purpose": "telemetry"}, file
        assert footer["thing"]["deployment_id"] == "fixture-plot-7"
        assert acquisition["calibration_id"] == "soil-cal-2026-10"
        assert footer["firmware"]["schema"] == "soil-fixture-v1"
        assert footer["thing"]["board"] == "fixture-soil-board"

        groups = [parquet.metadata.row_group(i).num_rows
                  for i in range(parquet.metadata.num_row_groups)]
        assert groups == [90, 30], groups
        assert max(groups) <= file["row_group_rows_max"]
        names = parquet.schema_arrow.names
        assert not any(name.startswith("pm") for name in names), names
        assert names[-2:] == ["soil_moisture_pct", "soil_temperature_c"]

        table = parquet.read()
        mono = table.column("monotonic_us").to_pylist()
        steps = {b - a for a, b in zip(mono, mono[1:])}
        assert steps == {acquisition["sample_interval_ms"] * 1000}, steps
        assert len(mono) == 120

        connection = duckdb.connect()
        rows, step, nulls = connection.execute(
            "SELECT count(*), max(monotonic_us) - min(monotonic_us), "
            "count(*) - count(soil_moisture_pct) FROM read_parquet(?)",
            [str(output)]).fetchone()
        assert rows == 120 and step == 119 * 30_000_000 and nulls == 24
        keys, interval = connection.execute(
            "SELECT list(decode(key)), any_value(decode(value)::JSON->>'$.acquisition.sample_interval_ms') "
            "FROM parquet_kv_metadata(?)", [str(output)]).fetchone()
        assert keys == ["opensensor"] and interval == "30000", (keys, interval)
    print("begin() runs footer::check_config() before any file work and fails closed. "
          "Default opensensor footer JSON pinned, OS- BLE name and os- mDNS host. "
          "SL- soil fixture at 30 s wrote 90 + 30 rows with matching "
          "sample_interval_ms in one valid opensensor key, read by PyArrow and DuckDB")


if __name__ == "__main__":
    main()
