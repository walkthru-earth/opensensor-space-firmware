"""Host test for tools/rebuild_partial.py on board-shaped fixture files.

Run with ``pixi run rebuild-partial-test [--sanitize]``.

tools/partial_rebuild_fixture.cpp writes a four row group file with exactly
the firmware writer and each board's telemetry_fields.inc, in both codecs.
The test cuts that file at every stage a power cut can hit (before the first
group, inside a page header, inside a payload, on each group boundary, inside
the footer, before and inside the trailing length and magic) and checks that
the rebuild keeps exactly the completed row groups. Rebuilt files must read
the same in PyArrow and DuckDB, equal the original rows that were fully
written, and carry the same schema, row group metadata and column orders as
the footer the board would have written. No serial port or board is used.
"""

from __future__ import annotations

import argparse
import contextlib
import io
import json
import math
import struct
from pathlib import Path
import subprocess
import sys
import tempfile

import pyarrow.parquet as pq

sys.path.insert(0, str(Path(__file__).resolve().parent))
import footer_contract  # noqa: E402
import parquet_footer as footer  # noqa: E402
import rebuild_partial as rp  # noqa: E402
from host_cxx import CXX_STD  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent
GROUPS, ROWS = 4, 90


def require(condition: bool, description: str) -> None:
    if not condition:
        raise AssertionError(description)


def compile_fixture(inc: Path, exe: Path, sanitize: bool) -> None:
    command = [
        "clang++", CXX_STD, "-Wall", "-Wextra", "-Werror",
        "-I", str(ROOT / "firmware/common/vendor/lz4"),
        f'-DOS_FIELDS_INC="{inc.resolve()}"',
    ]
    if sanitize:
        command += ["-fsanitize=address,undefined", "-fno-sanitize-recover=all"]
    command += [
        str(ROOT / "tools/partial_rebuild_fixture.cpp"),
        str(ROOT / "firmware/common/src/parquet_writer.cpp"),
        str(ROOT / "firmware/common/src/lz4_codec.cpp"),
        "-o", str(exe),
    ]
    subprocess.run(command, check=True)


def write_fixture(exe: Path, out: Path, codec: str) -> dict:
    run = subprocess.run([str(exe), str(out), codec, str(GROUPS), str(ROWS)],
                         check=True, capture_output=True, text=True)
    info = json.loads(run.stdout)
    require(info["size"] == out.stat().st_size, "fixture size report")
    require(len(info["group_ends"]) == GROUPS, "fixture group ends")
    return info


def same_value(a, b) -> bool:
    if isinstance(a, float) and isinstance(b, float):
        if math.isnan(a) or math.isnan(b):
            return math.isnan(a) and math.isnan(b)
        return a == b and math.copysign(1, a) == math.copysign(1, b)
    return a == b


def same_rows(left: list[dict], right: list[dict]) -> bool:
    return len(left) == len(right) and all(
        l.keys() == r.keys() and all(same_value(l[k], r[k]) for k in l) for l, r in zip(left, right)
    )


def cuts(info: dict) -> list[int]:
    ends, size = info["group_ends"], info["size"]
    points = {0, 2, 4, 5, 6, 40, ends[0] // 2, size - 8, size - 7, size - 5, size - 4, size - 1}
    for end in ends:
        points.update({end - 1, end, end + 1, end + 2, end + 3, end + 9})
    for start, end in zip(ends, ends[1:]):
        points.add((start + end) // 2)
    footer_start = ends[-1]
    footer_length = size - 8 - footer_start
    points.update(footer_start + footer_length * k // 5 for k in range(1, 5))
    return sorted(p for p in points if 0 <= p < size)


def check_cut(src_dir: Path, out_dir: Path, data: bytes, cut: int, info: dict,
              fields: list[rp.Field], fields_path: Path, original: list[dict],
              meta: dict, label: str, columns: int | None = None) -> str:
    source = src_dir / f"Y=2026/M=10/aq_{label}_{cut}-1.partial"
    source.parent.mkdir(parents=True, exist_ok=True)
    source.write_bytes(data[:cut])
    target_dir = out_dir / f"{label}-{cut}"
    report = rp.rebuild(source, target_dir, fields, fields_path=fields_path)
    complete = sum(1 for end in info["group_ends"] if end <= cut)
    where = f"{label} cut={cut}"
    require(source.read_bytes() == data[:cut], f"{where}: source untouched")
    if complete == 0:
        require(report["status"] == "empty" and "output" not in report, f"{where}: empty {report}")
        require(not target_dir.exists(), f"{where}: nothing written for an empty partial")
        return "empty"
    good_end = info["group_ends"][complete - 1]
    require(report["status"] == "rebuilt", f"{where}: status {report}")
    require(report["row_groups"] == complete and report["rows"] == complete * ROWS, f"{where}: counts {report}")
    require(report["kept_bytes"] == good_end and report["dropped_tail_bytes"] == cut - good_end, f"{where}: bytes")
    require(report["readers_match"] is True, f"{where}: readers")
    expected_tail = "none" if cut == good_end else ("footer" if complete == GROUPS else "page")
    if cut - good_end == 1:
        expected_tail = "page"  # one 0x15 byte opens both a page and a footer
    require(report["tail"] == expected_tail, f"{where}: tail {report['tail']} != {expected_tail}")
    output = Path(report["output"])
    require(output.name == f"aq_{label}_{cut}-1.rebuilt.parquet", f"{where}: output name {output.name}")
    body = output.read_bytes()
    require(body[:good_end] == data[:good_end], f"{where}: data pages copied byte for byte")
    rows = pq.read_table(output).to_pylist()
    require(same_rows(rows, original[: complete * ROWS]), f"{where}: rows equal the written prefix")
    rebuilt = footer.read_footer(output)
    width = columns or len(fields)
    require(rebuilt[footer.FILE_VERSION] == 1, f"{where}: version")
    require(rebuilt[footer.FILE_SCHEMA] == meta[footer.FILE_SCHEMA][: width + 1], f"{where}: schema")
    require(rebuilt[footer.FILE_ROWS] == complete * ROWS, f"{where}: num_rows")
    require(rebuilt[footer.FILE_ROW_GROUPS] == meta[footer.FILE_ROW_GROUPS][:complete], f"{where}: row groups")
    require(rebuilt[footer.FILE_COLUMN_ORDERS] == meta[footer.FILE_COLUMN_ORDERS], f"{where}: column orders")
    require(rebuilt[footer.FILE_CREATED_BY] == rp.TOOL_ID.encode(), f"{where}: created_by")
    kv = {pair[1].decode(): pair[2].decode() for pair in rebuilt[footer.FILE_KV]}
    require(kv["rebuild_source_bytes"] == str(cut), f"{where}: kv source bytes")
    require(kv["rebuild_dropped_tail_bytes"] == str(cut - good_end), f"{where}: kv tail")
    require(len(kv["rebuild_source_sha256"]) == 64 and len(kv["rebuild_fields_sha256"]) == 64, f"{where}: kv digests")
    kept = fields[:width]
    if any(f.geometry for f in kept):
        # The rebuilt `geo` value is GeoParquet 2.0.0 and its bbox covers the
        # kept rows, as the board would have written it.
        points = [struct.unpack("<BIdd", r["geometry"])[2:] for r in rows if r["geometry"] is not None]
        bbox = None
        if points:
            xs, ys = [x for x, _ in points], [y for _, y in points]
            bbox = [min(xs), min(ys), max(xs), max(ys)]
        problems = footer_contract.geo_problems(kv.get("geo", ""), bbox)
        require(not problems, f"{where}: rebuilt geo {problems}")
    else:
        require("geo" not in kv, f"{where}: geo without a geometry column")
    return "rebuilt"


def check_board(name: str, inc: Path, exe: Path, work: Path, sanitize: bool) -> int:
    compile_fixture(inc, exe, sanitize)
    fields = rp.load_fields(inc)
    checked = 0
    for codec in ("none", "lz4"):
        label = f"{name}-{codec}"
        original_path = work / f"{label}.parquet"
        info = write_fixture(exe, original_path, codec)
        require(info["columns"] == len(fields), f"{label}: field table parse")
        data = original_path.read_bytes()
        original = pq.read_table(original_path).to_pylist()
        meta = footer.read_footer(original_path)
        require(len(meta[footer.FILE_ROW_GROUPS]) == GROUPS, f"{label}: original groups")
        outcomes = {}
        for cut in cuts(info):
            outcome = check_cut(work / "src", work / "out", data, cut, info, fields, inc, original, meta, label)
            outcomes[outcome] = outcomes.get(outcome, 0) + 1
            checked += 1
        require(outcomes.get("empty") and outcomes.get("rebuilt"), f"{label}: both outcomes covered")

        # Rename stage. A complete file is copied unchanged, never refootered.
        whole = work / "src" / f"{label}-whole-1.partial"
        whole.write_bytes(data)
        report = rp.rebuild(whole, work / "out" / f"{label}-whole", fields, fields_path=inc)
        require(report["status"] == "complete", f"{label}: complete status")
        require(Path(report["output"]).read_bytes() == data, f"{label}: complete copy is identical")

        # A forced codec that does not match the file finds no row group.
        wrong = "lz4" if codec == "none" else "none"
        cut = info["group_ends"][1] + 7
        report = rp.rebuild(_cut_file(work, f"{label}-wrongcodec", data, cut), work / "out" / f"{label}-wc",
                            fields, codec=wrong)
        require(report["status"] == "empty", f"{label}: wrong forced codec {report}")
        report = rp.rebuild(_cut_file(work, f"{label}-rightcodec", data, cut), work / "out" / f"{label}-rc",
                            fields, codec=codec)
        require(report["status"] == "rebuilt" and report["row_groups"] == 2, f"{label}: right forced codec")

        # A corrupt page header in group 3 stops the walk at group 2.
        broken = bytearray(data[: info["group_ends"][3] - 1])
        broken[info["group_ends"][1]] = 0x00  # STOP, an empty header
        report = rp.rebuild(_cut_file(work, f"{label}-corrupt", bytes(broken), len(broken)),
                            work / "out" / f"{label}-corrupt", fields)
        require(report["status"] == "rebuilt" and report["row_groups"] == 2, f"{label}: corrupt header {report}")
        require(report["tail"] == "unknown", f"{label}: corrupt tail kind")

        # A corrupt definition level stream in group 2 also stops the walk.
        if codec == "none":
            broken = bytearray(data[: info["group_ends"][2] + 3])
            start, _, _, _ = rp.page_header(bytes(broken), info["group_ends"][0])
            broken[start + 4] = 0x03  # a bit packed run, never written by the board
            report = rp.rebuild(_cut_file(work, f"{label}-levels", bytes(broken), len(broken)),
                                work / "out" / f"{label}-levels", fields)
            require(report["status"] == "rebuilt" and report["row_groups"] == 1, f"{label}: corrupt levels {report}")
        else:
            broken = bytearray(data[: info["group_ends"][2] + 3])
            start, _, stored, _ = rp.page_header(bytes(broken), info["group_ends"][0])
            broken[start] = 0xFF  # the first token now claims a run past the block
            report = rp.rebuild(_cut_file(work, f"{label}-lz4", bytes(broken), len(broken)),
                                work / "out" / f"{label}-lz4", fields)
            require(report["status"] == "rebuilt" and report["row_groups"] == 1, f"{label}: corrupt lz4 {report}")
        checked += 5 + check_page_guards(work, label, data, info, fields, codec)
    return checked


def page_header_bytes(raw_size: int, stored_size: int, rows: int) -> bytes:
    """Encode a page header the way the writer does, for crafted pages."""
    out = rp.Out()
    header = rp.Struct(out)
    header.number(1, rp.I32, 0)
    header.number(2, rp.I32, raw_size)
    header.number(3, rp.I32, stored_size)
    header.field(5, rp.STRUCT)
    body = rp.Struct(out)
    for field_id, value in ((1, rows), (2, 0), (3, 3), (4, 3)):
        body.number(field_id, rp.I32, value)
    body.end()
    header.end()
    return bytes(out.buf)


def torn(call, *args) -> str | None:
    """Return the Torn message of a call, or None when it was accepted."""
    try:
        call(*args)
    except rp.Torn as error:
        return str(error)
    return None


def check_page_guards(work: Path, label: str, data: bytes, info: dict, fields: list[rp.Field], codec: str) -> int:
    """Crafted pages in group 2 that a strict walk must refuse."""
    code = rp.CODECS[codec]
    pos = info["group_ends"][0]
    index = next(i for i, f in enumerate(fields) if f.name == "sequence")
    for f in fields[:index]:
        start, _, stored, _ = rp.page_header(data, pos)
        pos = start + stored
    start, raw_size, stored_size, rows = rp.page_header(data, pos)
    end = start + stored_size
    require(data[pos:start] == page_header_bytes(raw_size, stored_size, rows), f"{label}: header encoder")
    kind = fields[index].type

    # A page whose row count differs from the first page of its group.
    message = torn(rp.parse_page, data, pos, kind, code, rows - 1)
    require(message is not None and "rows" in message, f"{label}: page row mismatch {message}")
    if codec == "none":
        return 1

    # A short LZ4 block that keeps the header raw size. Arrow fills the
    # missing tail with whatever was in memory, so only an exact length check
    # refuses it.
    raw = rp.pa.decompress(data[start:end], decompressed_size=raw_size, codec="lz4_raw", asbytes=True)
    short = rp.pa.compress(raw[:-8], codec="lz4_raw", asbytes=True)
    page = page_header_bytes(raw_size, len(short), rows) + short
    crafted = data[:pos] + page + data[end : info["group_ends"][1]]
    message = torn(rp.parse_page, crafted, pos, kind, code, rows)
    require(message is not None and "fewer bytes" in message, f"{label}: short lz4 block {message}")
    report = rp.rebuild(_cut_file(work, f"{label}-short", crafted, len(crafted)), work / "out" / f"{label}-short",
                        fields)
    require(report["status"] == "rebuilt" and report["row_groups"] == 1, f"{label}: short lz4 block {report}")

    # A header asking for more bytes than any writer page is never allocated.
    huge = data[:pos] + page_header_bytes(2**31 - 1, stored_size, rows) + data[start:end]
    message = torn(rp.parse_page, huge, pos, kind, code, rows)
    require(message is not None and "writer layout" in message, f"{label}: huge raw size {message}")
    return 3


def check_levels() -> None:
    """A definition level above 1 is refused even when the sizes add up."""
    payload = b"\x02\x00\x00\x00\x06\x02"  # one RLE run of 3 rows at level 2
    message = torn(rp.decode_payload, payload, 3, rp.INT32)
    require(message is not None and "max level" in message, f"level above 1 {message}")
    message = torn(rp.decode_payload, b"\x02\x00\x00\x00\x06\x00", 3, rp.INT32)
    require(message is None, "an all null run decodes")


def _cut_file(work: Path, label: str, data: bytes, cut: int) -> Path:
    path = work / "src" / f"{label}-1.partial"
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(data[:cut])
    return path


def check_older_prefix(inc: Path, exe: Path, work: Path, sanitize: bool) -> None:
    """A boot on an older firmware wrote a prefix of today's field table."""
    lines = inc.read_text().splitlines()
    kept, count = [], 0
    for line in lines:
        if line.startswith("FIELD("):
            count += 1
            if count > 40:
                continue
        kept.append(line)
    older = work / "older_fields.inc"
    older.write_text("\n".join(kept) + "\n")
    compile_fixture(older, exe, sanitize)
    path = work / "older.parquet"
    info = write_fixture(exe, path, "lz4")
    data = path.read_bytes()
    fields = rp.load_fields(inc)[:40]
    original = pq.read_table(path).to_pylist()
    meta = footer.read_footer(path)
    cut = info["group_ends"][2] + 11
    check_cut(work / "src", work / "out", data, cut, info, fields, inc, original, meta, "older", columns=40)


def check_cli(inc: Path, work: Path) -> None:
    source = work / "src" / "cli" / "x-1.partial"
    source.parent.mkdir(parents=True, exist_ok=True)
    source.write_bytes(b"not parquet at all")
    stdout = io.StringIO()
    with contextlib.redirect_stdout(stdout):
        code = rp.main(["--fields", str(inc), "--out", str(work / "out" / "cli"), str(source)])
    report = json.loads(stdout.getvalue())
    require(code == 1 and report["status"] == "error" and "PAR1" in report["error"], "cli rejects non parquet")
    stdout = io.StringIO()
    with contextlib.redirect_stdout(stdout):
        code = rp.main(["--fields", str(inc), "--out", str(source.parent), str(source)])
    require(code == 1 and "source directory" in stdout.getvalue(), "cli refuses to write next to the source")
    # An existing output is never overwritten.
    good = work / "waveshare-none.parquet"
    cut_source = source.parent / "y-1.partial"
    data = good.read_bytes()
    ends = rp.plan(data[: len(data) - 20], rp.load_fields(inc)).good_end
    cut_source.write_bytes(data[: ends + 5])
    out = work / "out" / "cli-twice"
    for attempt, expected in ((1, 0), (2, 1)):
        stdout = io.StringIO()
        with contextlib.redirect_stdout(stdout):
            code = rp.main(["--fields", str(inc), "--out", str(out), str(cut_source)])
        require(code == expected, f"cli attempt {attempt} exit {code}: {stdout.getvalue()}")
    require(pq.read_metadata(out / "y-1.rebuilt.parquet").num_row_groups == GROUPS, "cli output kept")
    # --board takes any board.json name (#141), the footer slug too, and an
    # unknown name stops with a usage error before any file is written.
    out = work / "out" / "cli-slug"
    stdout = io.StringIO()
    with contextlib.redirect_stdout(stdout):
        code = rp.main(["--board", "waveshare-esp32-s3-sim7670g", "--out", str(out), str(cut_source)])
    require(code == 0 and (out / "y-1.rebuilt.parquet").is_file(), f"cli --board slug: {stdout.getvalue()}")
    stderr = io.StringIO()
    try:
        with contextlib.redirect_stderr(stderr):
            rp.main(["--board", "waveshare-v3", "--out", str(work / "out" / "cli-unknown"), str(cut_source)])
    except SystemExit as error:
        require(error.code == 2 and "unknown board 'waveshare-v3'" in stderr.getvalue(), "cli unknown board")
    else:
        raise AssertionError("cli accepted an unknown --board")
    require(not (work / "out" / "cli-unknown").exists(), "cli unknown board wrote nothing")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--sanitize", action="store_true", help="build the fixture with ASan and UBSan")
    args = parser.parse_args()
    checked = 0
    with tempfile.TemporaryDirectory(prefix="aq-rebuild-") as temporary:
        work = Path(temporary)
        for name, inc in rp.board_fields().items():
            checked += check_board(name, inc, work / f"fixture-{name}", work, args.sanitize)
        waveshare = rp.board_fields()["waveshare"]
        check_older_prefix(waveshare, work / "fixture-older", work, args.sanitize)
        check_cli(waveshare, work)
        check_levels()
    suffix = " (ASan + UBSan fixture)" if args.sanitize else ""
    print(f"PASS partial rebuild, {checked} cuts and corruptions across both boards and codecs{suffix}")


if __name__ == "__main__":
    main()
