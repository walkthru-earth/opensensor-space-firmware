#!/usr/bin/env python3
"""Gate and mutation tests of the plugin contract (#17).

The checked-in manifests, locks and board files must pass
tools/plugin_contract.py check(), including the base branch lock. Each rule
is then proved on a scratch copy of the files the gate reads, with the base
lock injected. The gate must reject each mutation with its own message, and
must accept a clean lock history.
"""

from __future__ import annotations

import contextlib
import copy
import io
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

import plugin_contract as gate

ROOT = gate.ROOT
CORES3 = Path("firmware/esp-idf-cores3/bringup")
WAVESHARE = Path("firmware/esp-idf-waveshare-sim7670g/logger")
PMSA003 = gate.COMMON_MODULES / "pmsa003/module.json"
IMU = CORES3 / "modules/m5_imu/module.json"
IDENTITY = WAVESHARE / "modules/waveshare/module.json"
BOARD_FILES = ("board.json", gate.LOCK, "telemetry_fields.inc", "telemetry_semantics.inc", "telemetry_classes.inc")
# An older CoreS3 dict the migration notes name. The history tests use it as an earlier release.
OLDER_DICT = "1180e7273bf1dd4cce94ba02b84af61bdddeeb11dc7ddcef61967aca7031c472"


def no_base(_: Path) -> None:
    return None


def rejects(action, expected: str, mutation: str) -> None:
    try:
        action()
    except AssertionError as error:
        if expected not in str(error):
            raise AssertionError(f"{mutation} failed for another reason, {error}") from error
        return
    raise AssertionError(f"{mutation} passed the gate")


def scratch_copy(scratch: Path) -> Path:
    out = scratch / "repo"
    for directory in (gate.PLUGINS, gate.STAC, gate.SCALES.parent, gate.TEMPLATES, gate.COMMON_MODULES):
        shutil.copytree(ROOT / directory, out / directory)
    for path in (gate.MIGRATION, gate.WRITER, gate.SYSTEM_MODULE, gate.SENSOR_GLUE):
        (out / path).parent.mkdir(parents=True, exist_ok=True)
        shutil.copy(ROOT / path, out / path)
    for board in (CORES3, WAVESHARE):
        (out / board).mkdir(parents=True, exist_ok=True)
        for name in BOARD_FILES:
            shutil.copy(ROOT / board / name, out / board / name)
        shutil.copytree(ROOT / board / "modules", out / board / "modules")
    return out


def read(path: Path) -> dict:
    return json.loads(path.read_text())


def edit_json(relative: Path, change):
    def apply(root: Path) -> None:
        document = read(root / relative)
        change(document)
        (root / relative).write_text(json.dumps(document, indent=2, ensure_ascii=False) + "\n")
    return apply


def replace_text(relative: Path, old: str, new: str):
    def apply(root: Path) -> None:
        text = (root / relative).read_text()
        gate.require(old in text, f"mutation anchor {old!r} missing in {relative}")
        (root / relative).write_text(text.replace(old, new, 1))
    return apply


def mutate(scratch: Path, edit, expected: str, mutation: str, base=no_base) -> None:
    root = scratch_copy(scratch)
    try:
        edit(root)
        rejects(lambda: gate.check(root, base), expected, mutation)
    finally:
        shutil.rmtree(root)
    print(f"PASS rejects {mutation}", flush=True)


def column(document: dict, segment: str, name: str) -> dict:
    return next(c for c in document["segments"][segment] if c["name"] == name)


def older_release(lock: dict) -> dict:
    """A shipped release that is a strict prefix of today's CoreS3 release."""
    newest = lock["releases"][-1]
    segments = copy.deepcopy(newest["segments"][:-1])
    return {"dict": OLDER_DICT, "rows": sum(s["rows"] for s in segments), "segments": segments}


def with_history(change=None):
    def apply(root: Path) -> None:
        def edit(lock: dict) -> None:
            lock["releases"].insert(0, older_release(lock))
            if change:
                change(lock)
        edit_json(CORES3 / gate.LOCK, edit)(root)
    return apply


def cores3_base(document: dict):
    return lambda path: document if path.parent.name == "bringup" else None


def main() -> None:
    results, examples, headers = gate.check()
    for r in results:
        print(f"PASS {r.board} {r.columns} columns in {r.segments} segments, dict {r.dict}", flush=True)
    gate.require(len(headers) > 0, "the gate checks generated module headers")
    print(f"PASS {len(headers)} generated module headers", flush=True)
    for line in examples:
        print(f"PASS example {line}", flush=True)

    # setup_v, as the phone computes it. SHA-256 of the empty string starts
    # e3 b0 c4 42, so the empty list proves the top bit is cleared.
    gate.require(gate.setup_v([]) == 0x63B0C442, f"setup_v of no step is {gate.setup_v([]):#x}")
    for path in sorted(ROOT.glob(gate.BOARD_GLOB)):
        steps = [s["id"] for s in read(path).get("setup", [])]
        value = gate.setup_v(steps)
        gate.require(1 <= value <= 0x7FFFFFFF, f"{path.parent.name} setup_v {value} out of range")
        print(f"PASS {path.parent.parent.name} setup_v {value} ({value:#010x}) over {steps}", flush=True)

    board = CORES3 / "board.json"
    lock = CORES3 / gate.LOCK
    today = read(ROOT / lock)

    with tempfile.TemporaryDirectory(prefix="plugin-contract-") as temporary:
        scratch = Path(temporary)
        root = scratch_copy(scratch)
        gate.check(root, no_base)
        gate.check(root, cores3_base(today))
        shutil.rmtree(root)
        print("PASS the scratch copy passes, so each rejection below comes from its mutation", flush=True)

        # The manifests reproduce the board files byte for byte.
        mutate(scratch, edit_json(IMU, lambda d: column(d, "core", "accel_x_g")["firmware"].update(decimals=4)),
               "FIELD row", "a decimals change in a manifest")
        mutate(scratch, edit_json(PMSA003, lambda d: column(d, "core", "pm1_cf1_ug_m3")["firmware"].update(
            live_key="x1")), "FIELD row", "a LIVE key change in a manifest")
        mutate(scratch, edit_json(PMSA003, lambda d: column(d, "core", "pms_age_ms")["firmware"].update(
            validity="always")), "FIELD row", "a validity change in a manifest")
        mutate(scratch, edit_json(PMSA003, lambda d: column(d, "core", "pm1_cf1_ug_m3").update(
            {"opensensor:plausible_range": [0, 999]})), "SEM row", "a plausible range change")
        mutate(scratch, edit_json(PMSA003, lambda d: column(d, "core", "pm1_cf1_ug_m3").update(
            {"opensensor:aggregate": "maximum"})), "SEM row", "an aggregate change")
        mutate(scratch, edit_json(PMSA003, lambda d: d.update(model="PMS5003")), "SEM row",
               "a module model change")
        mutate(scratch, edit_json(PMSA003, lambda d: column(d, "core", "pms_status")["classification:classes"][1]
                                  .update(name="warm")), "pms_status", "a class name change")
        mutate(scratch, edit_json(PMSA003, lambda d: column(d, "core", "pms_status")["classification:classes"][0]
                                  .update(value=99)), "class values ascending", "a class value out of order")
        mutate(scratch, edit_json(PMSA003, lambda d: d.update(vendor='Plan"tower')), "needs escaping",
               "a string the .inc cannot spell")
        mutate(scratch, edit_json(PMSA003, lambda d: column(d, "core", "pm1_cf1_ug_m3").update(
            {"opensensor:valid_when": {"column": "pms_status", "finite": True}})), "finite gate must name",
               "a finite gate on another column")
        mutate(scratch, edit_json(board, lambda d: d["segments"].insert(2, d["segments"].pop(3))),
               "FIELD row", "a segment reorder")
        mutate(scratch, edit_json(board, lambda d: d["segments"].pop()), "manifest columns",
               "a removed last segment")
        mutate(scratch, edit_json(board, lambda d: d.update(title="Other")), "title differs", "a title change")
        mutate(scratch, edit_json(board, lambda d: d.update(sample_interval_s=5)), "sample_interval_s differs", "a sample interval change")
        mutate(scratch, edit_json(WAVESHARE / "board.json", lambda d: d.pop("class_subsets")),
               "clock_status", "a dropped class subset")
        mutate(scratch, edit_json(WAVESHARE / "board.json", lambda d: d.update(
            class_subsets={"clock_status": ["none", "gps"]})), "class_subsets names a class",
               "a class subset naming an unknown class")

        # Schema rules.
        mutate(scratch, edit_json(PMSA003, lambda d: column(d, "core", "pms_age_ms").update(
            {"opensensor:field_id": 12})), "schema", "a field_id in a manifest")
        mutate(scratch, edit_json(PMSA003, lambda d: column(d, "core", "pms_age_ms").update(type="int64")),
               "schema", "a template type in a manifest")
        mutate(scratch, edit_json(PMSA003, lambda d: d.update(id="PMS-A003")), "schema", "a bad module id")
        mutate(scratch, edit_json(IMU, lambda d: column(d, "core", "accel_x_g")["firmware"].update(decimals=10)),
               "schema", "decimals over 9")
        mutate(scratch, edit_json(PMSA003, lambda d: column(d, "core", "pms_age_ms").update(colour="red")),
               "schema", "an unknown column member")
        mutate(scratch, edit_json(PMSA003, lambda d: d.pop("procedure")), "schema", "a module without procedure")
        mutate(scratch, edit_json(board, lambda d: d.update(board="M5 CoreS3")), "schema", "a bad board slug")
        mutate(scratch, edit_json(lock, lambda d: d["releases"][0].update(dict="nope")), "schema",
               "a lock dict that is not a digest")

        # Cross rules.
        mutate(scratch, edit_json(board, lambda d: d["segments"].append({"module": "nope", "segment": "core"})),
               "unknown module", "an unknown module")
        mutate(scratch, edit_json(board, lambda d: d["segments"].append({"module": "pmsa003", "segment": "nope"})),
               "unknown segment", "an unknown segment")
        mutate(scratch, edit_json(board, lambda d: d["segments"].append({"module": "system", "segment": "head"})),
               "segment system.head repeats", "a repeated segment")
        mutate(scratch, edit_json(board, lambda d: d["local_modules"].append("../../../common/x/module.json")),
               "does not match", "a local module outside the board app")
        mutate(scratch, edit_json(PMSA003, lambda d: column(d, "core", "pms_age_ms").update(name="pms_frames")),
               "column name repeats", "a duplicate column in a module")
        mutate(scratch, edit_json(PMSA003, lambda d: column(d, "core", "pms_frames")["firmware"].update(
            live_key="bat")), "LIVE key repeats", "a LIVE key repeat on one board")
        mutate(scratch, edit_json(IDENTITY, lambda d: column(d, "pms_identity", "sensor_serial_code")["firmware"]
                                  .update(live_key="imu_t")), "LIVE key imu_t", "a LIVE key naming two columns")
        mutate(scratch, edit_json(PMSA003, lambda d: column(d, "core", "pms_age_ms")["firmware"].update(
            decimals=2)), "decimals on a non Float", "decimals on an integer column")
        mutate(scratch, edit_json(PMSA003, lambda d: column(d, "core", "pm1_cf1_ug_m3").update(
            {"opensensor:valid_when": {"column": "nope", "equals": 2}})), "valid_when names an unknown",
               "a valid_when on an unknown column")
        mutate(scratch, replace_text(gate.WRITER, "kMaxColumns = 96;", "kMaxColumns = 80;"), "over kMaxColumns",
               "the column cap")
        mutate(scratch, lambda r: (r / board).unlink(), "has a board.json", "a board app without board.json")

        # Display hints and the setup manifest (#184).
        mutate(scratch, edit_json(PMSA003, lambda d: column(d, "core", "pms_status").update(title="Other")),
               "template column", "a column title change")
        mutate(scratch, edit_json(PMSA003, lambda d: column(d, "core", "pms_status")["classification:classes"][4]
                                  .update(title="Good")), "template column", "a class title change")
        mutate(scratch, edit_json(PMSA003, lambda d: column(d, "core", "pms_age_ms").update(
            {"opensensor:decimals": 1})), "template column", "authored decimals the template lacks")
        mutate(scratch, edit_json(PMSA003, lambda d: column(d, "core", "pms_age_ms").update(
            {"opensensor:decimals": 10})), "schema", "authored decimals over 9")
        mutate(scratch, edit_json(PMSA003, lambda d: column(d, "core", "pms_age_ms").update(
            {"opensensor:group": "weather"})), "schema", "a group outside the vocabulary")
        mutate(scratch, edit_json(PMSA003, lambda d: column(d, "core", "pms_age_ms").update(
            {"classification:bitfields": [{"offset": 0, "length": 1, "classes": [{"value": 1, "name": "set"}]}]})),
               "bit fields on a non", "bit fields on a column that is not a bitmask")
        mutate(scratch, edit_json(IMU, lambda d: column(d, "core", "imu_fresh_mask")["classification:bitfields"]
                                  [1].update(offset=0)), "bit fields overlap", "overlapping bit fields")
        mutate(scratch, edit_json(IMU, lambda d: column(d, "core", "imu_fresh_mask")["classification:bitfields"]
                                  [2].update(offset=40)), "outside Int32", "a bit field outside the column type")
        mutate(scratch, edit_json(PMSA003, lambda d: column(d, "core", "pm25_atmospheric_ug_m3").update(
            {"opensensor:scale": "who-2021-pm10x"})), "is not in", "a scale the registry does not hold")
        mutate(scratch, edit_json(PMSA003, lambda d: column(d, "core", "pm1_atmospheric_ug_m3").update(
            {"opensensor:scale": "who-2021-pm25"})), "template column", "a new scale on a column")
        mutate(scratch, edit_json(board, lambda d: d.update(primary="pms_status")), "not a physicalMeasurement",
               "a primary that is not a measurement")
        mutate(scratch, edit_json(board, lambda d: d.update(primary="pm10_atmospheric_ug_m3")), "primary differs",
               "a primary change")
        mutate(scratch, edit_json(board, lambda d: d.pop("primary")), "no primary metric", "a board without a primary")
        mutate(scratch, edit_json(IMU, lambda d: column(d, "core", "accel_x_g").pop("opensensor:title_i18n")),
               "no Arabic title", "a column without an Arabic title")
        mutate(scratch, edit_json(PMSA003, lambda d: column(d, "core", "pm1_cf1_ug_m3").update(unit="ug/m\u00b3")),
               "not a plain ASCII UCUM code", "a superscript in a unit")
        mutate(scratch, edit_json(board, lambda d: d["setup"].reverse()), "firmware setup differs",
               "a setup reorder")
        mutate(scratch, edit_json(board, lambda d: d["settings"].pop()), "firmware settings differs",
               "a removed settings section")
        mutate(scratch, edit_json(board, lambda d: d["setup"].append({"id": "find"})), "schema",
               "an app owned step")
        mutate(scratch, edit_json(board, lambda d: d["setup"].append({"id": "wifi"})), "setup id repeats",
               "a repeated step")
        mutate(scratch, edit_json(board, lambda d: d["settings"].append({"id": "name"})), "not a settings section",
               "a setup only step in settings")
        mutate(scratch, edit_json(board, lambda d: d["setup"].append({"id": "sensor"})), "not a setup step",
               "a settings only section in setup")
        mutate(scratch, edit_json(board, lambda d: d["setup"].append({"id": "panel"})), "lists no keys",
               "a board step without keys")
        panel = {"type": "enum", "values": ["light", "dark"], "applies": "immediately", "page": 7, "label": "Theme"}
        mutate(scratch, edit_json(board, lambda d: d.update(config_keys={"panel.theme": panel},
                                                            settings=[*d["settings"], {"id": "panel", "keys": ["panel.theme"]}])),
               "has no label", "a board section without a label")
        mutate(scratch, edit_json(board, lambda d: d["settings"][0].update(label="")), "schema", "an empty step label")
        mutate(scratch, edit_json(board, lambda d: d["setup"][0].update(label_i18n={"AR": "x"})), "schema",
               "a step label translation keyed by an upper case tag")
        mutate(scratch, edit_json(board, lambda d: d["settings"][0].update(label="Place")), "firmware settings differs",
               "a section label the template lacks")
        mutate(scratch, edit_json(board, lambda d: d.update(config_keys={"panel.theme": panel},
                                                            settings=[{**s, "keys": ["panel.theme"]} for s in d["settings"]])),
               "lists keys", "an app step with keys")
        mutate(scratch, edit_json(board, lambda d: d["settings"].append({"id": "panel", "label": "Display", "keys": ["panel.theme"]})),
               "undeclared keys", "a step naming an undeclared key")
        mutate(scratch, edit_json(board, lambda d: d.update(config_keys={"panel.theme": panel})), "in no setup step",
               "a key no step shows")
        mutate(scratch, edit_json(board, lambda d: d.update(config_keys={"panel.pin": dict(panel)},
                                                            settings=[*d["settings"], {"id": "panel", "label": "Display", "keys": ["panel.pin"]}])),
               "looks like a secret", "a secret key name")
        for prefix in ("ble", "wifi", "lan", "ntp", "location", "thing", "owner", "sensor"):
            mutate(scratch, edit_json(board, lambda d, k=f"{prefix}.theme": d.update(
                config_keys={k: panel}, settings=[*d["settings"], {"id": "panel", "label": "Display", "keys": [k]}])),
                   "schema", f"a key under the reserved prefix {prefix}.")
        mutate(scratch, edit_json(board, lambda d: d.update(config_keys={"panel.theme": {**panel, "page": 6}},
                                                            settings=[*d["settings"], {"id": "panel", "label": "Display", "keys": ["panel.theme"]}])),
               "schema", "a key on another page")
        mutate(scratch, edit_json(board, lambda d: d.update(config_keys={"panel.sleep": {**panel, "type": "integer"}},
                                                            settings=[*d["settings"], {"id": "panel", "label": "Display", "keys": ["panel.sleep"]}])),
               "schema", "an integer key without a range")
        mutate(scratch, edit_json(board, lambda d: d.update(config_keys={"panel.clean": {**panel, "applies": "action"}},
                                                            settings=[*d["settings"], {"id": "panel", "label": "Display", "keys": ["panel.clean"]}])),
               "schema", "an action that is not a boolean")
        mutate(scratch, edit_json(board, lambda d: d.update(config_keys={"panel.theme": panel},
                                                            settings=[*d["settings"], {"id": "panel", "label": "Display", "keys": ["panel.theme"]}])),
               "firmware settings differs", "a new board key the template lacks")

        # The lock.
        mutate(scratch, replace_text(CORES3 / "telemetry_fields.inc", "// FIELD(", "//  FIELD("),
               "telemetry_fields.inc is", "a comment only edit of telemetry_fields.inc")
        mutate(scratch, lambda r: (r / lock).unlink(), "missing shipped.lock", "a missing lock")
        mutate(scratch, edit_json(lock, lambda d: d["releases"][-1]["segments"][1].update(sha256="0" * 64)),
               "segments differ from the board manifest", "an edited segment hash")
        mutate(scratch, edit_json(lock, lambda d: d["releases"][-1]["segments"].pop()), "do not add up",
               "a removed lock segment")
        mutate(scratch, edit_json(lock, lambda d: d["releases"][-1]["segments"].insert(
            0, d["releases"][-1]["segments"].pop(1))), "not contiguous", "a reordered lock segment")
        mutate(scratch, with_history(lambda d: d["releases"][0].update(dict="1" * 64)), "never names dict",
               "a release the migration notes never name")

        root = scratch_copy(scratch)
        with_history()(root)
        results, _, _ = gate.check(root, cores3_base({"board": "m5stack-cores3", "releases": [older_release(today)]}))
        gate.require(results[0].dict == today["releases"][-1]["dict"], "the newest release is today's")
        shutil.rmtree(root)
        print("PASS accepts a release that appends to an older one", flush=True)

        def reorder_older(lock: dict) -> None:
            older = lock["releases"][0]["segments"]
            older[1], older[2] = older[2], older[1]
            older[1]["first"] = 1 + older[0]["rows"]
            older[2]["first"] = older[1]["first"] + older[1]["rows"]

        mutate(scratch, with_history(reorder_older), "reorders, removes or edits",
               "a newer release that reorders a shipped segment")
        mutate(scratch, with_history(lambda d: d["releases"][0]["segments"][2].update(sha256="2" * 64)),
               "reorders, removes or edits", "a newer release that edits a shipped segment")
        # A baseline release may edit a shipped segment only with its migration entry.
        heading = "Test baseline (plugin contract test)"

        def listed(lock: dict, index: int) -> dict:
            segment = lock["releases"][0]["segments"][index]
            return {"module": segment["module"], "segment": segment["segment"]}

        def baseline(lock: dict) -> None:
            lock["releases"][0]["segments"][2].update(sha256="2" * 64)
            lock["releases"][1]["baseline"] = {"reason": "an owner approved edit", "migration": heading,
                                               "segments": [listed(lock, 2)]}

        def also(change):
            def apply(lock: dict) -> None:
                baseline(lock)
                change(lock)
            return apply

        def note(body: str):
            def apply(root: Path) -> None:
                path = root / gate.MIGRATION
                text = path.read_text()
                first = text.index("\n## ") + 1
                path.write_text(text[:first] + f"## {heading}\n\n{body}\n\n" + text[first:])
            return apply

        def both(*edits):
            return lambda root: [edit(root) for edit in edits]

        mutate(scratch, with_history(baseline), "names no", "a baseline release with no migration entry")
        mutate(scratch, both(with_history(baseline), note("No dict named here.")), "never names its dict",
               "a baseline whose migration entry never names its dict")
        mutate(scratch, with_history(lambda d: d["releases"][0].update(
            baseline={"reason": "x", "migration": heading, "segments": [listed(d, 2)]})), "cannot be a baseline",
               "a first release baseline")
        named = note(f"New dict `{today['releases'][-1]['dict']}`.")
        root = scratch_copy(scratch)
        both(with_history(baseline), named)(root)
        gate.check(root, no_base)
        shutil.rmtree(root)
        print("PASS accepts a baseline release that edits a listed shipped segment", flush=True)

        # A baseline is per segment. Every shipped segment it does not list keeps the old rule.
        mutate(scratch, both(with_history(also(lambda d: d["releases"][0]["segments"][1].update(sha256="4" * 64))),
                             named), "which its baseline does not list",
               "a baseline that also edits a shipped segment it does not list")
        mutate(scratch, both(with_history(also(lambda d: d["releases"][1]["baseline"]["segments"].append(
            listed(d, 0)))), named), "which is not a shipped segment it edits",
               "a baseline that lists a shipped segment it leaves alone")
        mutate(scratch, both(with_history(also(lambda d: d["releases"][1]["baseline"]["segments"].append(
            {"module": "nope", "segment": "core"}))), named), "which is not a shipped segment it edits",
               "a baseline that lists a segment never shipped")
        mutate(scratch, both(with_history(also(lambda d: d["releases"][1]["baseline"].pop("segments"))), named),
               "segments", "a baseline with no segment list")

        def swap_shipped(lock: dict) -> None:
            newer = lock["releases"][1]["segments"]
            newer[0], newer[1] = newer[1], newer[0]
            newer[0]["first"], newer[1]["first"] = 1, 1 + newer[0]["rows"]
            lock["releases"][1]["baseline"]["segments"] = [listed(lock, 2), listed(lock, 0), listed(lock, 1)]

        mutate(scratch, both(with_history(also(swap_shipped)), named), "reorders or removes a shipped segment",
               "a baseline that reorders shipped segments")
        # The probe from the #126 review. The real Waveshare baseline lists the
        # two MAX17048 segments, so an edit to pms5003t core fails.
        mutate(scratch, edit_json(WAVESHARE / gate.LOCK, lambda d: next(
            s for s in d["releases"][0]["segments"] if (s["module"], s["segment"]) == ("pms5003t", "core")).update(
                sha256="5" * 64)), "edits shipped segment pms5003t/core",
               "a Waveshare baseline edit to pms5003t core, which it does not list")

        # The heading must start a line. A ### subheading of the same text is not the entry.
        def subheading(root: Path) -> None:
            path = root / gate.MIGRATION
            text = path.read_text()
            first = text.index("\n## ") + 1
            path.write_text(text[:first] + f"### {heading}\n\nNew dict `{today['releases'][-1]['dict']}`.\n\n"
                            + text[first:])

        mutate(scratch, both(with_history(baseline), subheading), "names no",
               "a baseline whose heading is only a ### subheading")

        # A baseline on release 2 does not cover an edit to release 1. The base
        # lock comparison catches it.
        mutate(scratch, both(with_history(baseline), named), "release 1 differs from the base branch",
               "an edit to release 1 under a release 2 baseline",
               base=cores3_base({"board": "m5stack-cores3", "releases": [older_release(today)]}))
        mutate(scratch, with_history(lambda d: d["releases"].append(copy.deepcopy(d["releases"][0]))),
               "a dict repeats", "a dict that repeats")
        mutate(scratch, lambda r: None, "lost a release present on the base branch", "a release dropped from the base",
               base=cores3_base({"board": "m5stack-cores3", "releases": [older_release(today)] + today["releases"]}))
        edited = copy.deepcopy(today)
        edited["releases"][0]["segments"][0]["sha256"] = "3" * 64
        mutate(scratch, lambda r: None, "differs from the base branch", "a lock entry edited against the base",
               base=cores3_base(edited))
        mutate(scratch, lambda r: None, "names another board", "a base lock of another board",
               base=cores3_base({**today, "board": "other"}))

        # The generated module headers equal their manifests byte for byte (#17 step 4).
        pmsa003_header = PMSA003.parent / "pmsa003_module.h"
        imu_header = IMU.parent / "m5_imu_module.h"
        identity_header = IDENTITY.parent / "waveshare_module.h"
        mutate(scratch, replace_text(pmsa003_header, "kStatusValid = 4;", "kStatusValid = 5;"),
               "stale generated module header", "a hand edited common module header")
        mutate(scratch, replace_text(imu_header, "\n} // namespace", "\n// local note\n} // namespace"),
               "stale generated module header", "a hand edited board local module header")
        mutate(scratch, lambda r: (r / identity_header).unlink(), "missing generated module header",
               "a deleted module header")
        mutate(scratch, edit_json(PMSA003, lambda d: d.update(init_errors=["not_found"])),
               "pmsa003_module.h: stale generated module header", "a manifest edit without a regenerated header")

        def orphan(root: Path) -> None:
            stray = root / gate.COMMON_MODULES / "gone"
            stray.mkdir()
            shutil.copy(root / pmsa003_header, stray / "gone_module.h")

        mutate(scratch, orphan, "generated module header without a module manifest",
               "a module header whose manifest was removed")

        # The write path regenerates every header, removes the orphan, and the
        # check then passes on the same tree.
        root = scratch_copy(scratch)
        edit_json(PMSA003, lambda d: d.update(init_errors=["not_found"]))(root)
        (root / identity_header).unlink()
        orphan(root)
        with contextlib.redirect_stdout(io.StringIO()) as written:
            gate.check(root, no_base, write_headers=True)
        gate.require("WROTE firmware/common/sensors/modules/pmsa003/pmsa003_module.h" in written.getvalue(),
                     f"the write path rewrote the stale header, {written.getvalue()!r}")
        gate.require(not any((root / gate.COMMON_MODULES / "gone").glob("*_module.h")),
                     "the write path removed the orphan header")
        gate.check(root, no_base)
        text = (root / pmsa003_header).read_text()
        gate.require("enum class [[nodiscard]] InitError : std::uint8_t { not_found };" in text,
                     "the regenerated header carries the new InitError")
        gate.require((root / identity_header).is_file(), "the write path restored the deleted header")
        shutil.rmtree(root)
        print("PASS the write path regenerates, removes the orphan, then the check passes", flush=True)

        # Only a file that starts with the generated preamble is ever removed
        # or overwritten. A hand written *_module.h fails in both modes and stays.
        handmade = gate.COMMON_MODULES / "handmade/handmade_module.h"

        def hand_written(root: Path) -> None:
            (root / handmade).parent.mkdir()
            (root / handmade).write_text("#pragma once\n// hand written module code\n")

        mutate(scratch, hand_written, "hand written file with the generated _module.h suffix",
               "a hand written orphan module header")
        root = scratch_copy(scratch)
        hand_written(root)
        rejects(lambda: gate.check(root, no_base, write_headers=True), "nothing removes it",
                "a hand written orphan under the write path")
        gate.require((root / handmade).read_text() == "#pragma once\n// hand written module code\n",
                     "the write path left the hand written orphan alone")
        (root / handmade).unlink()
        (root / identity_header).write_text("#pragma once\n// hand written in place of the generated header\n")
        rejects(lambda: gate.check(root, no_base, write_headers=True), "nothing overwrites it",
                "a hand written file where a generated header goes, under the write path")
        gate.require((root / identity_header).read_text().endswith("in place of the generated header\n"),
                     "the write path did not overwrite the hand written file")
        shutil.rmtree(root)
        print("PASS the write path never removes or overwrites a hand written module header", flush=True)

        # The orphan scan covers a header straight under modules/ and a board
        # modules/ folder whose last local module is gone.
        def loose(root: Path) -> None:
            shutil.copy(root / pmsa003_header, root / gate.COMMON_MODULES / "loose_module.h")

        mutate(scratch, loose, "generated module header without a module manifest",
               "a generated header straight under modules/")
        root = scratch_copy(scratch).resolve()
        checks = gate.validators(root)
        common = gate.common_modules(root, checks)
        boards = [gate.load_board(root, path, checks, common) for path in sorted(root.glob(gate.BOARD_GLOB))]
        for b in boards:
            if b.directory == root / WAVESHARE:
                b.modules.clear()  # as if board.json dropped its last local module
        modules = gate.header_modules(common, boards)
        rejects(lambda: gate.check_headers(root, modules, boards), "waveshare_module.h: generated module header "
                "without a module manifest", "a header left behind by a board's last local module")
        with contextlib.redirect_stdout(io.StringIO()) as written:
            gate.check_headers(root, modules, boards, write=True)
        gate.require(not (root / identity_header).exists() and "REMOVED" in written.getvalue(),
                     "the write path removed the header the board no longer lists")
        shutil.rmtree(root)
        print("PASS rejects then removes a header whose board dropped its last local module", flush=True)

        # A module id that names something in namespace ossensor would collide
        # with ossensor::<id> (#127 review).
        reserved = gate.reserved_ids(ROOT)
        gate.require({"detail", "resolve", "init_error", "std", "telemetry", "oslogger"} <= reserved,
                     f"reserved ids come from os_sensor_module.h, {sorted(reserved)}")

        def module_id(new: str):
            def apply(root: Path) -> None:
                old = root / gate.COMMON_MODULES / "sht20"
                edit_json(old.relative_to(root) / "module.json", lambda d: d.update(id=new))(root)
                shutil.move(old, old.parent / new)
                (old.parent / new / "sht20_module.h").unlink()
                text = (root / CORES3 / "board.json").read_text().replace('"sht20"', f'"{new}"')
                (root / CORES3 / "board.json").write_text(text)
            return apply

        for name in ("detail", "resolve", "std"):
            mutate(scratch, module_id(name), f"module id {name!r} is reserved", f"a module id {name}")
        # A name os_sensor_module.h adds later is reserved with no list to edit.
        mutate(scratch, both(module_id("write_row"), replace_text(
            gate.SENSOR_GLUE, "namespace detail {", "void write_row();\nnamespace detail {")),
               "module id 'write_row' is reserved", "a module id a new ossensor function takes")
        root = scratch_copy(scratch)
        module_id("write_row")(root)
        # The renamed module changes the lock segment names, so a free id
        # gets past every manifest rule and fails at the lock.
        rejects(lambda: gate.check(root, no_base), "segments differ from the board manifest",
                "a free module id passing the reserved rule")
        shutil.rmtree(root)
        print("PASS a free module id passes the reserved rule and fails only at the lock", flush=True)

        # Write mode for a new board (#17 step 7), the section 9 weather
        # example with the example BME280 as a board local module.
        new_board = Path("firmware/esp-idf-example-weather-s3/app")

        def weather(root: Path) -> None:
            directory = root / new_board
            (directory / "modules/bme280").mkdir(parents=True)
            shutil.copy(root / gate.EXAMPLES / "bme280.module.json", directory / "modules/bme280/module.json")
            document = read(root / gate.EXAMPLES / "weather.board.json")
            document["local_modules"] = ["modules/bme280/module.json"]
            (directory / "board.json").write_text(json.dumps(document, indent=2) + "\n")

        root = scratch_copy(scratch)
        weather(root)
        board_json = root / new_board / "board.json"
        written = gate.write_board(root, board_json)
        gate.require(written == [(new_board / n).as_posix() for n in gate.NEW_BOARD_FILES],
                     f"write mode wrote the four board files, {written}")
        files = {n: (root / new_board / n).read_bytes() for n in gate.NEW_BOARD_FILES}
        weather_lock = json.loads(files[gate.LOCK])
        digest = gate.hashlib.sha256(files["telemetry_fields.inc"]).hexdigest()
        gate.require(weather_lock["board"] == "example-weather-s3" and len(weather_lock["releases"]) == 1
                     and weather_lock["releases"][0]["dict"] == digest and "baseline" not in weather_lock["releases"][0],
                     "the first lock has one release at the written dict")
        gate.require([(s["module"], s["segment"]) for s in weather_lock["releases"][0]["segments"]] ==
                     [(s["module"], s["segment"]) for s in read(root / gate.EXAMPLES / "weather.board.json")["segments"]],
                     "the first lock lists the board segments in order")
        rows = files["telemetry_fields.inc"].decode().splitlines()
        gate.require(rows.count("FIELD(bme280_status, Int32, bme280, \"code\", always, \"\", 0)") == 1,
                     "the BME280 status FIELD row is written")
        gate.require('CLASS(bme280_status, 0, "missing", true)' in files["telemetry_classes.inc"].decode(),
                     "the BME280 status classes are written")
        # The full gate accepts every written row and the lock. Without a
        # migration entry it stops at the lock, and with one it stops only at
        # the template the new board has not published yet.
        rejects(lambda: gate.check(root, no_base), f"example-weather-s3: {gate.MIGRATION} never names dict {digest}",
                "a written board with no migration entry")
        notes = root / gate.MIGRATION
        notes.write_text(notes.read_text() + f"\n## Example weather board\n\nNew dict `{digest}`.\n")
        rejects(lambda: gate.check(root, no_base), f"example-weather-s3: no published template for dict {digest}",
                "a written board, which passes FIELD, SEM, CLASS and the lock")
        print("PASS write mode writes the weather board, and the gate accepts its rows and first lock", flush=True)

        # Once written, the board files are frozen. A second run refuses and
        # changes nothing, and an existing lock alone blocks every write.
        rejects(lambda: gate.write_board(root, board_json), "refusing to write a board file that exists",
                "a second write of the same board")
        gate.require({n: (root / new_board / n).read_bytes() for n in gate.NEW_BOARD_FILES} == files,
                     "a refused write changed no board file")
        for name in gate.NEW_BOARD_FILES:
            for other in gate.NEW_BOARD_FILES:
                (root / new_board / other).unlink(missing_ok=True)
            (root / new_board / name).write_bytes(b"kept\n")
            rejects(lambda: gate.write_board(root, board_json), f"{(new_board / name).as_posix()}. Board files",
                    f"a write over an existing {name}")
            gate.require((root / new_board / name).read_bytes() == b"kept\n", f"a refused write kept {name}")
            gate.require([n for n in gate.NEW_BOARD_FILES if (root / new_board / n).exists()] == [name],
                         f"a write refused for {name} wrote no other file")
        print("PASS write mode refuses any existing board file, an existing lock included, and writes nothing",
              flush=True)
        for name in gate.NEW_BOARD_FILES:
            (root / new_board / name).unlink(missing_ok=True)

        # A lock that appears after the exists check is still never
        # overwritten, because each file opens for exclusive create.
        render = gate.board_texts

        def racing(*args, **kwargs):
            (root / new_board / gate.LOCK).write_bytes(b"raced\n")
            return render(*args, **kwargs)

        gate.board_texts = racing
        try:
            try:
                gate.write_board(root, board_json)
            except FileExistsError:
                pass
            else:
                raise AssertionError("a lock created during the write was overwritten")
        finally:
            gate.board_texts = render
        gate.require((root / new_board / gate.LOCK).read_bytes() == b"raced\n", "a racing lock is kept")
        gate.require([n for n in gate.NEW_BOARD_FILES if (root / new_board / n).exists()] == [gate.LOCK],
                     "a failed write removed the files it created and left no partial board")
        print("PASS write mode never overwrites a lock that appears during the write and leaves no partial board",
              flush=True)
        (root / new_board / gate.LOCK).unlink()

        # Write mode takes a board manifest at firmware/esp-idf-*/*/board.json
        # only. Any other board.json in the repo is refused, nothing written.
        for elsewhere in (Path("docs/not-a-board"), Path(".")):
            (root / elsewhere).mkdir(parents=True, exist_ok=True)
            stray = read(board_json)
            stray["board"] = "example-stray"
            (root / elsewhere / "board.json").write_text(json.dumps(stray, indent=2) + "\n")
            shutil.copytree(root / new_board / "modules", root / elsewhere / "modules")
            rejects(lambda: gate.write_board(root, root / elsewhere / "board.json"), "not a board manifest",
                    f"a board.json at {elsewhere.as_posix()}")
            gate.require(not any((root / elsewhere / n).exists() for n in gate.NEW_BOARD_FILES),
                         f"a refused write at {elsewhere.as_posix()} wrote nothing")
            (root / elsewhere / "board.json").unlink()
            shutil.rmtree(root / elsewhere / "modules")
        print("PASS write mode refuses a board.json outside firmware/esp-idf-*/*/", flush=True)

        # Boolean columns print false and true, as the shipped files do. Each
        # boolean CLASS row rendered for CoreS3 is a row of its shipped file.
        resolved = root.resolve()
        checks = gate.validators(resolved)
        common = gate.common_modules(resolved, checks)
        cores3 = gate.load_board(resolved, resolved / CORES3 / "board.json", checks, common)
        rendered = gate.board_texts(resolved, cores3, gate.expand(cores3, common), checks)
        flags = [line for line in rendered["telemetry_classes.inc"].decode().splitlines()
                 if line.startswith("CLASS(") and line.split(", ")[1] in ("false", "true")]
        shipped = (resolved / CORES3 / "telemetry_classes.inc").read_text().splitlines()
        gate.require(len(flags) >= 4 and all(line in shipped for line in flags),
                     f"boolean CLASS rows match the shipped CoreS3 rows, {flags}")
        print("PASS write mode prints boolean classes as false and true like the shipped boards", flush=True)

        # Refusals that need the manifests.
        for path in (CORES3, WAVESHARE):
            before = {n: (root / path / n).read_bytes() for n in BOARD_FILES}
            rejects(lambda: gate.write_board(root, root / path / "board.json"),
                    "refusing to write a board file that exists", f"a write over the shipped {path.parent.name}")
            gate.require({n: (root / path / n).read_bytes() for n in BOARD_FILES} == before,
                         f"a refused write left {path.parent.name} unchanged")
        edit_json(new_board / "board.json", lambda d: d.update(board="m5stack-cores3"))(root)
        rejects(lambda: gate.write_board(root, board_json), "the board slug already exists", "a new board reusing a slug")
        edit_json(new_board / "board.json", lambda d: d.update(board="example-weather-s3"))(root)
        edit_json(new_board / "modules/bme280/module.json", lambda d: column(d, "core", "air_temperature_c")[
            "firmware"].update(live_key="pm25"))(root)
        rejects(lambda: gate.write_board(root, board_json), "LIVE key pm25", "a new board taking another board's LIVE key")
        edit_json(new_board / "modules/bme280/module.json", lambda d: column(d, "core", "air_temperature_c")[
            "firmware"].update(live_key="ta"))(root)
        edit_json(new_board / "board.json", lambda d: d["segments"].append({"module": "system", "segment": "head"}))(root)
        rejects(lambda: gate.write_board(root, board_json), "segment system.head repeats",
                "a new board that repeats a segment")
        gate.require(not any((root / new_board / n).exists() for n in gate.NEW_BOARD_FILES),
                     "a refused new board wrote no file")
        shutil.rmtree(root)
        print("PASS write mode refuses shipped boards, a reused slug, a LIVE key clash and a bad manifest", flush=True)

        # The base loader fails closed when the ref is missing.
        os.environ["PLUGIN_CONTRACT_BASE"] = "refs/heads/plugin-contract-no-such-ref"
        try:
            rejects(lambda: gate.git_base_lock(ROOT)(ROOT / lock), "is missing", "a missing base ref")
        finally:
            del os.environ["PLUGIN_CONTRACT_BASE"]
        print("PASS rejects a missing base ref", flush=True)

        # A lock absent at the base, as after a board directory move, is new
        # and the loader names it in a notice rather than skipping silently.
        first = subprocess.run(["git", "rev-list", "--max-parents=0", "HEAD"], cwd=ROOT, capture_output=True,
                               text=True, check=True).stdout.split()[0]
        os.environ["PLUGIN_CONTRACT_BASE"] = first
        notice = io.StringIO()
        try:
            with contextlib.redirect_stderr(notice):
                found = gate.git_base_lock(ROOT)(ROOT / lock)
        finally:
            del os.environ["PLUGIN_CONTRACT_BASE"]
        gate.require(found is None, "a lock absent at the base reads as new")
        gate.require(f"NOTICE {lock.as_posix()} is not on {first}" in notice.getvalue(),
                     f"no notice for a lock absent at the base, {notice.getvalue()!r}")
        print("PASS notices a lock absent at the base", flush=True)


if __name__ == "__main__":
    main()
