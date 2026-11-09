"""Host tests for the board registry, tools/boards.py (#141). No serial port is opened.

Every failure mode starts from a scratch copy of the real board.json files
that loads, applies one mutation and requires a BoardError naming the file,
so each check is proven to fire. The drift checks hold pixi.toml, the board
wrapper scripts, the CI and release build matrices and the stack budget to
exactly the registry boards. The shell checks run each board script from a
scratch tree whose `python` runs tools/boards.py and nothing else, so no
esptool command can ever start.
"""

from __future__ import annotations

import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import tomllib
from typing import Callable

from jsonschema import Draft7Validator
import yaml

import boards

ROOT = boards.ROOT
SCHEMA = ROOT / "docs/shared/plugins/v0.1.0/board.json"
BACKUP_BYTES = 16 * 1024 * 1024


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def scratch(directory: Path) -> Path:
    """A root holding every real board.json and an empty folder per variant."""
    root = directory / "repo"
    if root.exists():
        shutil.rmtree(root)
    for board in boards.boards():
        target = root / board.manifest
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(ROOT / board.manifest, target)
        for variant in board.variants:
            (root / board.project / variant).mkdir(parents=True, exist_ok=True)
    return root


def edit_json(path: Path, change: Callable[[dict], None]) -> None:
    manifest = json.loads(path.read_text())
    change(manifest)
    path.write_text(json.dumps(manifest, indent=2) + "\n")


def tooling(key: str, value: object) -> Callable[[dict], None]:
    def change(manifest: dict) -> None:
        manifest["tooling"][key] = value
    return change


def rejects(directory: Path, mutation: str, edit: Callable[[Path, list[boards.Board]], None],
            expected: list[str], action: Callable[[Path], object] = boards.boards) -> None:
    """A fresh scratch root loads, then edit makes action raise BoardError holding every expected text."""
    root = scratch(directory)
    every = list(boards.boards(root))
    require(len(every) == len(boards.boards()), f"{mutation}: the scratch root loads before the mutation")
    edit(root, every)
    try:
        action(root)
    except boards.BoardError as error:
        missing = [text for text in expected if text not in str(error)]
        require(not missing, f"{mutation}: error lacks {missing}, {error}")
    else:
        raise AssertionError(f"{mutation}: accepted")
    print(f"PASS rejects {mutation}", flush=True)


def check_failure_modes(directory: Path) -> None:
    first = boards.boards()[0]
    second = boards.boards()[1]
    manifest = first.manifest

    def on_first(change: Callable[[dict], None]) -> Callable[[Path, list[boards.Board]], None]:
        return lambda root, every: edit_json(root / manifest, change)

    def write_first(text: str) -> Callable[[Path, list[boards.Board]], None]:
        return lambda root, every: (root / manifest).write_text(text)

    rejects(directory, "no board project", lambda root, every: shutil.rmtree(root / "firmware"),
            ["no board project matches firmware/esp-idf-*"])
    rejects(directory, "a project without board.json", lambda root, every: (root / manifest).unlink(),
            [f"{first.project}: no */board.json"])

    def second_manifest(root: Path, every: list[boards.Board]) -> None:
        (root / first.project / "extra").mkdir()
        shutil.copy2(root / manifest, root / first.project / "extra/board.json")

    rejects(directory, "two board.json in one project", second_manifest,
            [f"{first.project}: more than one board.json", manifest, f"{first.project}/extra/board.json"])
    rejects(directory, "malformed JSON", write_first('{"board": "m5stack-cores3",'),
            [f"{manifest}: not readable JSON"])
    rejects(directory, "a JSON array", write_first("[]\n"), [f"{manifest}: not a JSON object"])
    rejects(directory, "non UTF-8 bytes", lambda root, every: (root / manifest).write_bytes(b"\xff\xfe{}"),
            [f"{manifest}: not readable JSON"])
    rejects(directory, "a board slug with a space", on_first(lambda m: m.update(board="m5stack cores3")),
            [f"{manifest}: board 'm5stack cores3' is not a slug"])
    rejects(directory, "a missing board slug", on_first(lambda m: m.pop("board")),
            [f"{manifest}: board None is not a slug"])
    rejects(directory, "a missing tooling object", on_first(lambda m: m.pop("tooling")),
            [f"{manifest}: no tooling object"])
    rejects(directory, "tooling that is a list", on_first(lambda m: m.update(tooling=[])),
            [f"{manifest}: no tooling object"])
    rejects(directory, "an unknown tooling key", on_first(tooling("port", "/dev/x")),
            [f"{manifest}: tooling keys, unknown ['port'], missing []"])
    rejects(directory, "a missing tooling key", on_first(lambda m: m["tooling"].pop("psram_mode")),
            [f"{manifest}: tooling keys, unknown [], missing ['psram_mode']"])
    rejects(directory, "an alias with a dash", on_first(tooling("alias", "core-s3")),
            [f"{manifest}: tooling.alias 'core-s3'"])
    rejects(directory, "an alias that is not a string", on_first(tooling("alias", 3)),
            [f"{manifest}: tooling.alias 3"])
    rejects(directory, "no variants", on_first(tooling("variants", [])),
            [f"{manifest}: tooling.variants must be a non empty list"])
    rejects(directory, "variants that do not start with the board.json folder",
            on_first(tooling("variants", ["other", first.telemetry_variant])),
            [f"{manifest}: tooling.variants starts with 'other'"])
    rejects(directory, "a repeated variant",
            on_first(tooling("variants", [first.telemetry_variant, first.telemetry_variant])),
            [f"{manifest}: tooling.variants repeats an entry"])
    rejects(directory, "a variant with a slash", on_first(tooling("variants", [first.telemetry_variant, "a/b"])),
            [f"{manifest}: tooling.variants entry 'a/b'"])
    rejects(directory, "no backup prefix", on_first(tooling("backup_prefixes", [])),
            [f"{manifest}: tooling.backup_prefixes must be a non empty list"])
    rejects(directory, "a backup prefix with a glob", on_first(tooling("backup_prefixes", ["m5stack-*"])),
            [f"{manifest}: tooling.backup_prefixes entry 'm5stack-*'"])
    for value in (8 * 1024 * 1024, float(BACKUP_BYTES), True, str(BACKUP_BYTES)):
        rejects(directory, f"flash_bytes {value!r}", on_first(tooling("flash_bytes", value)),
                [f"{manifest}: tooling.flash_bytes {value!r} is not 16777216"])
    rejects(directory, "an unknown PSRAM mode", on_first(tooling("psram_mode", "OPI")),
            [f"{manifest}: tooling.psram_mode 'OPI'"])
    rejects(directory, "an sdkconfig line that is not a setting", on_first(tooling("sdkconfig", ["rm -rf /"])),
            [f"{manifest}: tooling.sdkconfig entry 'rm -rf /'"])
    rejects(directory, "a component with a space", on_first(tooling("components_required", ["m5 unified"])),
            [f"{manifest}: tooling.components_required entry 'm5 unified'"])
    rejects(directory, "a forbidden component that is not a string", on_first(tooling("components_forbidden", [1])),
            [f"{manifest}: tooling.components_forbidden entry 1"])

    # A name two boards share is ambiguous, the error names both manifests.
    def shared(key: str, value: object) -> Callable[[Path, list[boards.Board]], None]:
        return lambda root, every: edit_json(root / second.manifest, tooling(key, value))

    rejects(directory, "an alias two boards share", shared("alias", first.alias),
            [f"name {first.alias!r} is already used by", first.manifest, second.manifest])
    rejects(directory, "a backup prefix that is another board's slug", shared("backup_prefixes", [first.slug]),
            [f"name {first.slug!r} is already used by", first.manifest, second.manifest])
    rejects(directory, "a restore only prefix two boards share",
            shared("backup_prefixes", [second.backup_prefix, first.backup_prefixes[-1]]),
            [f"name {first.backup_prefixes[-1]!r} is already used by"])
    rejects(directory, "a slug two boards share",
            lambda root, every: edit_json(root / second.manifest, lambda m: m.update(board=first.slug)),
            [f"name {first.slug!r} is already used by"])

    # Names nobody declares and variants a board does not list never default.
    for name in ("nope", "", "M5STACK-CORES3", f"{first.alias} "):
        rejects(directory, f"unknown board name {name!r}", lambda root, every: None,
                [f"unknown board {name!r}, expected one of"], lambda root, name=name: boards.find(name, root))
    rejects(directory, "a variant the board does not list", lambda root, every: None,
            [f"invalid board variant {first.alias}/nope"], lambda root: boards.resolve(first.alias, "nope", root))
    multi = next(b for b in boards.boards() if len(b.variants) > 1)
    rejects(directory, "a listed variant missing on disk",
            lambda root, every: shutil.rmtree(root / multi.project / multi.variants[1]),
            [f"{multi.manifest}: variant directory {multi.project}/{multi.variants[1]} is missing"],
            lambda root: boards.resolve(multi.alias, multi.variants[1], root))
    rejects(directory, "a project outside git", lambda root, every: None,
            [f"git ls-files failed for {first.project}"], lambda root: boards.tracked_sources(first, root))


def check_resolution() -> None:
    every = boards.boards()
    require(len(every) >= 2, "the repo has at least the two shipped boards")
    require([b.slug for b in every] == sorted(b.slug for b in every), "boards are sorted by slug")
    for board in every:
        require(board.app == f"{board.project}/{board.telemetry_variant}", f"{board.slug} app")
        require((ROOT / board.app / "board.json").is_file(), f"{board.slug} board.json is in the telemetry app")
        require(board.fixture == board.alias and (ROOT / "tools/fixtures/live_keys" / f"{board.alias}.json").is_file(),
                f"{board.slug} LIVE fixture")
        require((ROOT / "docs/boards" / board.slug).is_dir(), f"{board.slug} has docs/boards/<slug>")
        require((ROOT / "docs/shared/templates" / board.slug).is_dir(), f"{board.slug} has a published template")
        for name in board.names:
            require(boards.find(name) == board, f"{name} resolves to {board.slug}")
        for variant in board.variants:
            require(boards.resolve(board.slug, variant) == (board, variant), f"{board.slug}/{variant} resolves")
        require(boards.resolve(board.alias) == (board, board.telemetry_variant), f"{board.alias} defaults to telemetry")
        telemetry, other = boards.tracked_sources(board)
        require(f"{board.app}/main.cpp" in telemetry, f"{board.slug} telemetry sources hold main.cpp")
        require(not set(telemetry) & set(other), f"{board.slug} source lists are disjoint")
    # The CLI prints the same assignments for every name of a board and exits 2 otherwise.
    tool = [sys.executable, str(ROOT / "tools/boards.py")]
    for board in every:
        outputs = {subprocess.run([*tool, "resolve", name], capture_output=True, text=True, check=True).stdout
                   for name in board.names}
        require(outputs == {boards.shell_assignments(board, board.telemetry_variant)},
                f"{board.slug} resolves the same by every name")
        require(f"alias={board.alias}\n" in next(iter(outputs)) and f"slug={board.slug}\n" in next(iter(outputs)),
                f"{board.slug} assignments")
    for arguments, text in ((["resolve", "nope"], "board registry: unknown board 'nope'"),
                            (["resolve", every[0].alias, "nope"], "board registry: invalid board variant")):
        result = subprocess.run([*tool, *arguments], capture_output=True, text=True)
        require(result.returncode == 2 and text in result.stderr and result.stdout == "", f"CLI {arguments}, {result}")
    listed = subprocess.run([*tool, "list"], capture_output=True, text=True, check=True).stdout.splitlines()
    require(listed == [f"{b.slug} {b.alias} {b.project} {','.join(b.variants)}" for b in every], "CLI list")
    print(f"PASS {len(every)} boards resolve by alias, slug and backup prefix, CLI exits 2 on a bad name", flush=True)


def check_schema() -> None:
    """The plugin board schema and boards.py agree on every tooling rule."""
    schema = json.loads(SCHEMA.read_text())
    tool = schema["properties"]["tooling"]
    properties = tool["properties"]
    require(set(tool["required"]) == set(properties) == boards.TOOLING_KEYS and
            tool["additionalProperties"] is False, "schema tooling keys are boards.TOOLING_KEYS")
    require("tooling" not in schema["required"], "tooling stays optional in the plugin schema, a patch change")
    pairs = {
        "alias": (properties["alias"]["pattern"], boards.ALIAS.pattern),
        "variants": (properties["variants"]["items"]["pattern"], boards.VARIANT.pattern),
        "backup_prefixes": (properties["backup_prefixes"]["items"]["pattern"], boards.SLUG.pattern),
        "sdkconfig": (properties["sdkconfig"]["items"]["pattern"], boards.SDKCONFIG.pattern),
        "components_required": (properties["components_required"]["items"]["pattern"], boards.COMPONENT.pattern),
        "components_forbidden": (properties["components_forbidden"]["items"]["pattern"], boards.COMPONENT.pattern),
    }
    for key, (in_schema, in_tool) in pairs.items():
        require(in_schema == in_tool, f"tooling.{key} pattern differs, schema {in_schema}, boards.py {in_tool}")
    require(properties["flash_bytes"]["enum"] == [boards.FLASH_BYTES], "flash_bytes enum")
    require(properties["psram_mode"]["enum"] == list(boards.PSRAM_MODES), "psram_mode enum")
    validator = Draft7Validator(tool)
    for board in boards.boards():
        manifest = json.loads((ROOT / board.manifest).read_text())
        errors = [e.message for e in validator.iter_errors(manifest["tooling"])]
        require(not errors, f"{board.manifest} tooling, {errors}")
        for key, value in (("alias", "core-s3"), ("flash_bytes", 8388608), ("psram_mode", "OPI"),
                           ("variants", []), ("extra", 1)):
            bad = {**manifest["tooling"], key: value}
            require(any(validator.iter_errors(bad)), f"schema accepts tooling.{key}={value!r}")
    print("PASS the plugin board schema and boards.py agree on tooling", flush=True)


def check_drift() -> None:
    """Static lists that name boards hold exactly the registry boards."""
    every = boards.boards()
    by_alias = {b.alias: b for b in every}
    findings: list[str] = []

    tasks = tomllib.loads((ROOT / "pixi.toml").read_text())["tasks"]
    for board in every:
        for action in ("setup", "build", "flash"):
            name = f"{board.alias}-{action}"
            if tasks.get(name) != f"{board.project}/{action}.sh":
                findings.append(f"pixi.toml {name} is {tasks.get(name)!r}, expected {board.project}/{action}.sh")
        for action, tool in (("build", "build-idf-board.sh"), ("flash", "flash-idf-board.sh")):
            wrapper = (ROOT / board.project / f"{action}.sh").read_text()
            if f'tools/{tool}" {board.alias} "$@"' not in wrapper:
                findings.append(f"{board.project}/{action}.sh does not run tools/{tool} {board.alias}")
    projects = {b.project: b for b in every}
    for name, command in tasks.items():
        if not isinstance(command, str) or not command.startswith("firmware/esp-idf-"):
            continue
        words = command.split()
        project = str(Path(words[0]).parent)
        board = projects.get(project)
        if board is None:
            findings.append(f"pixi.toml {name} runs {words[0]}, which no board.json project owns")
        elif len(words) > 2 or (len(words) == 2 and words[1] not in board.variants):
            findings.append(f"pixi.toml {name} passes {words[1:]}, not one of {board.slug} variants")

    ci = yaml.safe_load((ROOT / ".github/workflows/ci.yml").read_text())
    legs = ci["jobs"]["board-build"]["strategy"]["matrix"]["include"]
    images = [leg for leg in legs if "project" in leg]
    built = sorted((leg["project"], leg["variant"]) for leg in images)
    expected = sorted((b.project, v) for b in every for v in b.variants)
    if built != expected:
        findings.append(f"ci.yml board-build image legs {built}, expected every registry variant {expected}")
    for leg in images:
        alias = leg["name"].split("-")[0]
        board = by_alias.get(alias)
        if board is None or leg["project"] != board.project:
            findings.append(f"ci.yml leg {leg['name']} is not <alias>[-variant] of the board building {leg['project']}")
            continue
        if leg["variant"] == board.telemetry_variant:
            tasks_ok = {f"{alias}-build", f"{alias}-build {leg['variant']}"}
        else:
            tasks_ok = {f"{alias}-build {leg['variant']}"}
        if leg["task"] not in tasks_ok:
            findings.append(f"ci.yml leg {leg['name']} task {leg['task']!r}, expected one of {sorted(tasks_ok)}")
    stall = next((leg for leg in legs if leg.get("bench") == "stall"), None)
    if stall is None:
        findings.append("ci.yml has no bench stall leg")
    else:
        ran = {stall["task"], stall.get("extra_task")} - {None}
        for board in every:
            if not ran & {f"{board.alias}-build", f"{board.alias}-build {board.telemetry_variant}"}:
                findings.append(f"ci.yml bench stall leg does not build {board.alias}")
    repro = ci["jobs"]["repro-paths"]["steps"][-1]["run"].split()
    if repro[:3] != ["pixi", "run", "repro-paths-check"] or len(repro) not in (4, 5):
        findings.append(f"ci.yml repro-paths runs {repro}, expected repro-paths-check <board> [variant]")
    else:
        try:
            boards.resolve(repro[3], repro[4] if len(repro) == 5 else None)
        except boards.BoardError as error:
            findings.append(f"ci.yml repro-paths board, {error}")

    release = yaml.safe_load((ROOT / ".github/workflows/release.yml").read_text())
    release_legs = next(job["strategy"]["matrix"]["include"] for job in release["jobs"].values()
                        if "strategy" in job)
    shipped = sorted((leg["project"], leg["variant"]) for leg in release_legs)
    if shipped != sorted((b.project, b.telemetry_variant) for b in every):
        findings.append(f"release.yml legs {shipped}, expected the telemetry app of every board")
    for leg in release_legs:
        board = projects.get(leg["project"])
        if board is None:
            continue
        cmake = (ROOT / board.project / "CMakeLists.txt").read_text()
        if f"project({leg['app']})" not in cmake:
            findings.append(f"release.yml leg {leg['name']} app {leg['app']} is not the project() of {board.project}")
        if leg["name"].split("-")[0] != board.alias or leg["task"] not in {
                f"{board.alias}-build", f"{board.alias}-build {leg['variant']}"}:
            findings.append(f"release.yml leg {leg['name']} does not build {board.alias}")

    budget = json.loads((ROOT / "tools/stack_budget.json").read_text())["images"]
    if sorted(budget) != sorted(f"{b.alias}/{v}" for b in every for v in b.variants):
        findings.append(f"tools/stack_budget.json images {sorted(budget)} differ from the registry variants")

    require(not findings, "board list drift, add the board everywhere or remove it:\n  " + "\n  ".join(findings))
    print("PASS pixi.toml, wrappers, ci.yml, release.yml and stack_budget.json list exactly the registry boards",
          flush=True)


def shell_tree(directory: Path) -> tuple[Path, dict[str, str]]:
    """A scratch tree with the board scripts, the registry and a python that refuses everything else."""
    root = scratch(directory)
    (root / "tools").mkdir()
    for name in ("boards.py", "build-idf-board.sh", "flash-idf-board.sh", "check_repro_paths.sh",
                 "restore-flash.sh", "backup-flash.sh"):
        shutil.copy2(ROOT / "tools" / name, root / "tools" / name)
    lz4 = Path("firmware/common/vendor/lz4")
    (root / lz4).mkdir(parents=True)
    for name in ("lz4.c", "lz4.h", "LICENSE"):
        shutil.copy2(ROOT / lz4 / name, root / lz4 / name)
    for board in boards.boards():
        # build-idf-board.sh sources this right after it resolves the board,
        # so the dry run prints the resolution and stops before any build.
        # OS_DRY_CONTINUE goes on with the real pins to reach later checks.
        shutil.copy2(ROOT / board.project / "dependencies.env", root / board.project / "dependencies.real.env")
        (root / board.project / "dependencies.env").write_text(
            'printf "RESOLVED board=%s variant=%s project=%s telemetry=%s\\n" '
            '"${board}" "${variant}" "${trial_dir#"${repo_root}/"}" "${telemetry}"\n'
            '[ -n "${OS_DRY_CONTINUE:-}" ] || exit 0\n'
            'source "${trial_dir}/dependencies.real.env"\n')
    fake = directory / "fake-bin"
    fake.mkdir(exist_ok=True)
    (fake / "python").write_text(
        "#!/bin/sh\n"
        'case "$1" in */tools/boards.py) exec "$OS_REAL_PYTHON" "$@" ;; esac\n'
        'printf "BLOCKED python %s\\n" "$*" >&2\n'
        "exit 97\n")
    (fake / "python").chmod(0o755)
    env = {**os.environ, "PATH": f"{fake}{os.pathsep}{os.environ['PATH']}", "OS_REAL_PYTHON": sys.executable}
    env.pop("OS_BENCH", None)
    return root, env


def run(root: Path, env: dict[str, str], script: str, *arguments: str) -> subprocess.CompletedProcess[str]:
    return subprocess.run(["bash", str(root / "tools" / script), *arguments], env=env, capture_output=True,
                          text=True, check=False)


def expect(result: subprocess.CompletedProcess[str], code: int, text: str, what: str) -> None:
    require(result.returncode == code and text in result.stdout + result.stderr,
            f"{what}, expected exit {code} with {text!r}, got {result.returncode}\n{result.stdout}{result.stderr}")
    require("BLOCKED python" not in result.stderr or code == 97, f"{what} reached a python command, {result.stderr}")


def check_shell(directory: Path) -> None:
    root, env = shell_tree(directory)
    every = boards.boards()
    for board in every:
        for name in board.names:
            for variant in (None, *board.variants):
                chosen = variant or board.telemetry_variant
                telemetry = "1" if chosen == board.telemetry_variant else "0"
                arguments = [name] if variant is None else [name, variant]
                expect(run(root, env, "build-idf-board.sh", *arguments), 0,
                       f"RESOLVED board={board.alias} variant={chosen} project={board.project} telemetry={telemetry}",
                       f"build-idf-board.sh {' '.join(arguments)}")
    for arguments, text in (([], "usage"), (["nope"], "unknown board 'nope'"),
                            ([every[0].alias, "nope"], "invalid board variant"),
                            ([every[0].alias, every[0].telemetry_variant, "x"], "usage")):
        expect(run(root, env, "build-idf-board.sh", *arguments), 2, text, f"build-idf-board.sh {arguments}")
    # OS_DRY_CONTINUE lets the dry run go past dependencies.env and the lz4
    # pins. A diagnostic variant has no digest check, so the bench refusal is next.
    stall = {**env, "OS_BENCH": "stall", "OS_DRY_CONTINUE": "1"}
    refused = 0
    for board in every:
        for variant in board.variants[1:]:
            result = subprocess.run(["bash", str(root / "tools/build-idf-board.sh"), board.slug, variant], env=stall,
                                    capture_output=True, text=True, check=False)
            expect(result, 2, f"OS_BENCH=stall needs the logger runtime of the telemetry app, not {variant}",
                   f"build-idf-board.sh {board.slug} {variant} with OS_BENCH=stall")
            refused += 1
    require(refused > 0, "some board has a diagnostic variant to refuse a bench build")

    # flash-idf-board.sh. /dev/null passes the character device test and is
    # never opened. The scratch tree has no build, so a resolved board stops at
    # the build check, before check_idf_target and esptool.
    backups = directory / "backups"
    backups.mkdir(exist_ok=True)

    def image(name: str) -> str:
        path = backups / name
        with path.open("wb") as stream:
            stream.truncate(BACKUP_BYTES)
        return str(path)

    for board in every:
        good = image(f"{board.backup_prefix}-flash-20261109T000000Z.bin")
        for name in board.names:
            expect(run(root, env, "flash-idf-board.sh", name, "/dev/null", good), 2,
                   "build and verify the selected native firmware first", f"flash-idf-board.sh {name}")
        other = next(b for b in every if b != board)
        wrong = image(f"{other.backup_prefix}-flash-20261109T000000Z.bin")
        expect(run(root, env, "flash-idf-board.sh", board.alias, "/dev/null", wrong), 2,
               f"expected {board.backup_prefix}-flash-*.bin", f"flash-idf-board.sh {board.alias} wrong backup")
    for arguments, text in (([], "usage"), (["nope", "/dev/null", good], "unknown board 'nope'"),
                            ([every[0].alias, "/dev/null", good, "nope"], "invalid board variant")):
        expect(run(root, env, "flash-idf-board.sh", *arguments), 2, text, f"flash-idf-board.sh {arguments[:1]}")

    # restore-flash.sh accepts every backup prefix of the board, then stops at
    # the blocked python that would run esptool.
    for board in every:
        for prefix in board.backup_prefixes:
            path = image(f"{prefix}-flash-20261109T000000Z.bin")
            expect(run(root, env, "restore-flash.sh", "--board", board.slug, "--port", "/dev/null", path), 97,
                   "BLOCKED python -m esptool", f"restore-flash.sh {board.slug} {prefix}")
        other = next(b for b in every if b != board)
        path = image(f"{other.backup_prefix}-flash-20261109T000000Z.bin")
        expect(run(root, env, "restore-flash.sh", "--board", board.alias, "--port", "/dev/null", path), 2,
               "backup name does not match board", f"restore-flash.sh {board.alias} wrong image")
    expect(run(root, env, "restore-flash.sh", "--board", "nope", "--port", "/dev/null", path), 2,
           "unknown board 'nope'", "restore-flash.sh unknown board")
    expect(run(root, env, "restore-flash.sh", "--port", "/dev/null", path), 2, "usage", "restore-flash.sh no board")

    # check_repro_paths.sh needs a board, there is no default.
    expect(run(root, env, "check_repro_paths.sh"), 2, "usage", "check_repro_paths.sh with no board")
    expect(run(root, env, "check_repro_paths.sh", "nope"), 2, "unknown board 'nope'", "check_repro_paths.sh nope")
    expect(run(root, env, "check_repro_paths.sh", every[0].alias, "nope"), 2, "invalid board variant",
           "check_repro_paths.sh bad variant")

    # A broken registry stops every script with the file named.
    (root / every[0].manifest).write_text("{")
    for script, arguments in (("build-idf-board.sh", [every[1].alias]),
                              ("flash-idf-board.sh", [every[1].alias, "/dev/null", good]),
                              ("restore-flash.sh", ["--board", every[1].alias, "--port", "/dev/null", path]),
                              ("backup-flash.sh", ["--board", every[1].alias, "--port", "/dev/null"]),
                              ("check_repro_paths.sh", [every[1].alias])):
        expect(run(root, env, script, *arguments), 2, f"{every[0].manifest}: not readable JSON",
               f"{script} with a malformed board.json")
    print("PASS board scripts resolve every name and variant, refuse unknown ones and a broken registry, "
          "and never reach esptool", flush=True)


def main() -> None:
    with tempfile.TemporaryDirectory(prefix="os-boards-") as temporary:
        directory = Path(temporary)
        check_failure_modes(directory)
        check_resolution()
        check_schema()
        check_drift()
        check_shell(directory)
    print("PASS board registry", flush=True)


if __name__ == "__main__":
    main()
