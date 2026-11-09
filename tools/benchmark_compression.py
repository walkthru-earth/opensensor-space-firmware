"""Host-only, identical-row page-codec comparison. Never alters input files.

Outputs are diagnostic duplicates, not new telemetry. PyArrow's writer differs
from our firmware writer; these timings and absolute sizes are not ESP32 results.
"""
import argparse
import hashlib
import io
import json
from pathlib import Path
import statistics
import tempfile
import time

import duckdb
import pyarrow.parquet as pq


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("input", type=Path)
    parser.add_argument("--repeats", type=int, default=10)
    args = parser.parse_args()
    if not 1 <= args.repeats <= 1000:
        parser.error("repeats must be 1..1000")
    table = pq.ParquetFile(args.input).read()
    results = []
    with tempfile.TemporaryDirectory(prefix="m5-codec-bench-") as temp:
        for codec in ("NONE", "snappy", "lz4", "zstd"):
            times = []
            for _ in range(args.repeats):
                sink = io.BytesIO()
                start = time.perf_counter_ns()
                pq.write_table(table, sink, compression=codec,
                               compression_level=1 if codec == "zstd" else None,
                               use_dictionary=False, write_statistics=False,
                               data_page_version="1.0", store_schema=False)
                times.append((time.perf_counter_ns() - start) / 1000)
            payload = sink.getvalue()
            path = Path(temp) / f"{codec}.parquet"
            path.write_bytes(payload)
            reader = pq.ParquetFile(path)
            if not reader.read().equals(table):
                raise ValueError(f"PyArrow mismatch: {codec}")
            with duckdb.connect() as connection:
                decoded = connection.sql(
                    "SELECT * FROM read_parquet(?, hive_partitioning=false)",
                    params=[str(path)]).to_arrow_table()
            if not decoded.equals(table, check_metadata=False):
                raise ValueError(f"DuckDB mismatch: {codec}")
            results.append({"codec": "LZ4_RAW" if codec == "lz4" else codec,
                            "bytes": len(payload), "host_median_write_us": statistics.median(times),
                            "footer_bytes": reader.metadata.serialized_size,
                            "both_readers_match": True})
    print(json.dumps({"scope": "host PyArrow writer; not firmware timing or peak-memory benchmark",
                      "input": str(args.input),
                      "input_sha256": hashlib.file_digest(args.input.open("rb"), "sha256").hexdigest(),
                      "rows": table.num_rows, "columns": table.num_columns,
                      "repeats": args.repeats, "results": results}, indent=2))


if __name__ == "__main__":
    main()
