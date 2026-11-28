# SPDX-License-Identifier: CC-BY-4.0. Copyright Walkthru.Earth.
"""Slice the stand for the Bambu Lab A1 with the Bambu Studio command line.

Builds the three presets from the Bambu Studio system profiles installed with
the app (A1 0.4 nozzle, 0.16mm Optimal @BBL A1, Generic PLA @BBL A1), applies
the changes in PROCESS_CHANGES, and slices each plate. The presets are made
fresh on every run, so no Bambu profile file is kept in the repo.

Run from this folder with plain Python, after the part 3MF files are exported
(README How to run):

    python3 -I slice.py            # every plate in PLATES
    python3 -I slice.py all_parts  # one plate

Each plate lands as <plate>.gcode.3mf in the artifacts folder, with its log
and result.json in slice/<plate>/. Set BAMBU_STUDIO to the app binary when it
is not in /Applications.
"""

import json
import os
from pathlib import Path
import subprocess
import sys

HERE = Path(__file__).resolve().parent
ART = HERE.parents[2] / "artifacts" / "enclosures" / HERE.name
APP = Path(os.environ.get("BAMBU_STUDIO", "/Applications/BambuStudio.app/Contents/MacOS/BambuStudio"))
PROFILES = APP.parents[1] / "Resources" / "profiles" / "BBL"

MACHINE = "Bambu Lab A1 0.4 nozzle"
PROCESS = "0.16mm Optimal @BBL A1"
FILAMENT = "Generic PLA @BBL A1"
PROCESS_CHANGES = {  # DESIGN Print settings
    "wall_loops": "3",
    "sparse_infill_pattern": "gyroid",
    "sparse_infill_density": "15%",
    "top_shell_layers": "6",
    "bottom_shell_layers": "4",
    "enable_support": "0",
    "curr_bed_type": "Textured PEI Plate",
}

# plate name, part 3MF stems placed on it
PLATES = {
    "shell_print": ("shell_print",),
    "bezel_print": ("bezel_print",),
    "sensor_bar_print": ("sensor_bar_print",),
    "all_parts": ("shell_print", "bezel_print", "sensor_bar_print"),
}


def _flat(kind, name):
    """A system profile with its inherits chain merged in, child keys win."""
    d = json.loads((PROFILES / kind / f"{name}.json").read_text())
    base = _flat(kind, d["inherits"]) if d.get("inherits") else {}
    base.update(d)
    return base


def _user(kind, name, changes=None, new_name=None):
    d = _flat(kind, name)
    d.update(changes or {})
    d.update({"from": "User", "inherits": name, "instantiation": "true", "name": new_name or name})
    return d


def write_presets(folder):
    folder.mkdir(parents=True, exist_ok=True)
    out = {
        "printer.json": _user("machine", MACHINE),
        "process.json": _user("process", PROCESS, PROCESS_CHANGES, f"{PROCESS} opensensor"),
        "filament-1.json": _user("filament", FILAMENT),
    }
    for fn, d in out.items():
        (folder / fn).write_text(json.dumps(d, indent=1) + "\n")
    return [folder / fn for fn in out]


def slice_plate(plate, parts, presets):
    out = ART / "slice" / plate
    out.mkdir(parents=True, exist_ok=True)
    meshes = [ART / f"{p}.3mf" for p in parts]
    missing = [str(m) for m in meshes if not m.exists()]
    if missing:
        sys.exit(f"missing {', '.join(missing)}, export the parts first")
    printer, process, filament = presets
    cmd = [str(APP), "--orient", "0", "--arrange", "1",
           "--load-settings", f"{printer};{process}", "--load-filaments", str(filament),
           "--slice", "0", "--outputdir", str(out), "--export-3mf", f"{plate}.gcode.3mf",
           *map(str, meshes)]
    with open(out / "slice.log", "w") as log:
        subprocess.run(cmd, cwd=out, stdout=log, stderr=subprocess.STDOUT, check=False)
    r = json.loads((out / "result.json").read_text())
    if r.get("return_code") != 0:
        sys.exit(f"{plate}: {r.get('error_string')}, see {out / 'slice.log'}")
    pl = r["sliced_plates"][0]
    (ART / f"{plate}.gcode.3mf").write_bytes((out / f"{plate}.gcode.3mf").read_bytes())
    mins = round(pl["total_predication"] / 60)
    warn = pl.get("warning_message") or "none"
    print(f"{plate}: {mins // 60} h {mins % 60} min, {pl['filaments'][0]['total_used_g']:.1f} g, warnings {warn}")
    return warn == "none"


def main(names):
    if not APP.exists():
        sys.exit(f"Bambu Studio not found at {APP}, set BAMBU_STUDIO")
    presets = write_presets(ART / "slice" / "presets")
    unknown = [n for n in names if n not in PLATES]
    if unknown:
        sys.exit(f"unknown plate {', '.join(unknown)}, pick from {', '.join(PLATES)}")
    clean = [slice_plate(n, PLATES[n], presets) for n in names or PLATES]
    if not all(clean):
        sys.exit("a slicer warning came up, see DESIGN Printability")


if __name__ == "__main__":
    main(sys.argv[1:])
