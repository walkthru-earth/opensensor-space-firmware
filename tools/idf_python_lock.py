#!/usr/bin/env python3
"""Hashed lock of the ESP-IDF Python build tools, and the drift check (#176).

The SDK installer resolves its Python packages at install time against a
constraints file it downloads from the Espressif host. That file moves and has
no checksum. This tool replaces that resolution with a committed lock,
tools/idf-python/idf<X.Y>_py<X.Y>.txt, that names every package of the IDF
venv with its exact version and the SHA-256 of each wheel for the Pixi
platforms. A release with no wheel for one of the platforms on this Python is
pinned by the SHA-256 of its source archive instead, and the `from-source`
header names it.

  install  Runs under the venv's own python. Installs the lock with
           `pip install --require-hashes --no-deps`. Wheels first, binary only.
           Then the from-source packages, built without build isolation so
           the build uses only the locked setuptools and fetches nothing
           unhashed. Then `pip check` and `check`.
  check    Runs under the venv's own python. Fails when the installed package
           set differs from the lock, by name or version. Exit 0 match,
           1 drift, 2 when it cannot run (missing or malformed lock).
  write    Runs under the Pixi python with network. Builds a throwaway venv
           with the pinned SDK installer and a throwaway IDF_TOOLS_PATH, so the
           shared ~/.espressif is never touched, then looks up the hashes of
           each installed version on PyPI and writes the lock.

tools/idf-env.sh runs `install`. Use the pixi tasks `idf-python-check` and
`idf-python-lock`, which pass the paths from tools/idf-env.sh.
"""
from __future__ import annotations

import argparse
import datetime
import hashlib
import json
import os
import re
import subprocess
import sys
import tempfile
import urllib.request
from pathlib import Path

PYPI_JSON = "https://pypi.org/pypi/{name}/{version}/json"
CONSTRAINTS_URL = "https://dl.espressif.com/dl/esp-idf/espidf.constraints.v{idf}.txt"
REQUIREMENTS = "tools/requirements/requirements.core.txt"
# Pixi platform to the wheel platform tags pip picks there. Wheel tags may be
# compressed, such as manylinux_2_17_x86_64.manylinux2014_x86_64.
PLATFORMS = {
    "osx-arm64": (("macosx_", ("_arm64", "_universal2")),),
    "linux-64": (("manylinux", ("_x86_64",)),),
}
HEADER_KEYS = ("esp-idf-version", "esp-idf-commit", "python", "platforms",
               "requirements", "constraints", "from-source", "generated")
# The build backend every from-source package in the lock uses. It must be
# locked itself, since those builds run without isolation.
BUILD_BACKEND = "setuptools"
SHA256 = re.compile(r"[0-9a-f]{64}")
PIN = re.compile(r"^([A-Za-z0-9][A-Za-z0-9._-]*)==([^\s\\]+)\s*\\?$")
HASH = re.compile(r"^--hash=sha256:([0-9a-f]{64})\s*\\?$")


class LockError(Exception):
    """The lock is missing or malformed."""


def canonical(name: str) -> str:
    """PEP 503 normalized project name."""
    return re.sub(r"[-_.]+", "-", name).lower()


def parse_lock(text: str) -> tuple[dict[str, str], dict[str, tuple[str, list[str]]]]:
    """Return the header fields and {canonical name: (version, [sha256])}."""
    header: dict[str, str] = {}
    pins: dict[str, tuple[str, list[str]]] = {}
    current = None
    for number, raw in enumerate(text.splitlines(), 1):
        line = raw.strip()
        if not line:
            current = None
            continue
        if line.startswith("#"):
            key, sep, value = line[1:].strip().partition(":")
            if sep and key in HEADER_KEYS:
                if key in header:
                    raise LockError(f"line {number}: header {key} repeated")
                header[key] = value.strip()
            continue
        if match := HASH.match(line):
            if current is None:
                raise LockError(f"line {number}: hash without a package")
            pins[current][1].append(match[1])
            continue
        match = PIN.match(line)
        if not match:
            raise LockError(f"line {number}: expected name==version or --hash=sha256:, got {line!r}")
        current = canonical(match[1])
        if current in pins:
            raise LockError(f"line {number}: {current} is listed twice")
        pins[current] = (match[2], [])
    missing = [key for key in HEADER_KEYS if key not in header]
    if missing:
        raise LockError(f"header lacks {', '.join(missing)}")
    if not pins:
        raise LockError("lock lists no package")
    for name, (_, hashes) in pins.items():
        if not hashes:
            raise LockError(f"{name} has no hash")
        if len(set(hashes)) != len(hashes):
            raise LockError(f"{name} repeats a hash")
    if list(pins) != sorted(pins):
        raise LockError("packages are not sorted by name")
    for name in from_source(header):
        if name not in pins:
            raise LockError(f"from-source names {name}, which the lock does not pin")
        if len(pins[name][1]) != 1:
            raise LockError(f"{name} is built from source and needs exactly one sdist hash")
    if from_source(header) and BUILD_BACKEND not in pins:
        raise LockError(f"from-source packages need {BUILD_BACKEND} in the lock")
    return header, pins


def from_source(header: dict[str, str]) -> list[str]:
    """Canonical names the lock builds from their hashed sdist, sorted."""
    value = header.get("from-source", "none")
    names = [] if value == "none" else value.split()
    if names != sorted(set(names)) or any(canonical(name) != name for name in names):
        raise LockError("from-source must be 'none' or sorted, unique canonical names")
    return names


def render_lock(header: dict[str, str], pins: dict[str, tuple[str, list[str]]]) -> str:
    """The lock text, in the form parse_lock reads and pip accepts with -r."""
    lines = [
        "# ESP-IDF Python build tools, pinned and hashed (#176). Generated, do not edit.",
        "# Refresh with `pixi run idf-python-lock`, see docs/shared/development.md.",
        "# tools/idf-env.sh installs exactly this set with --require-hashes --no-deps.",
        "# A from-source package has no wheel for every platform on this Python. The",
        "# lock pins its sdist, and pip builds it with the locked setuptools.",
        "# `pixi run idf-python-check` fails when the venv differs from this list.",
    ]
    lines += [f"# {key}: {header[key]}" for key in HEADER_KEYS]
    for name in sorted(pins):
        version, hashes = pins[name]
        lines.append(f"{name}=={version} \\")
        ordered = sorted(hashes)
        for index, digest in enumerate(ordered):
            tail = " \\" if index < len(ordered) - 1 else ""
            lines.append(f"    --hash=sha256:{digest}{tail}")
    return "\n".join(lines) + "\n"


def drift(locked: dict[str, str], installed: dict[str, str]) -> list[str]:
    """Differences between the lock and the installed set, empty when they match."""
    problems = []
    for name in sorted(set(locked) | set(installed)):
        if name not in installed:
            problems.append(f"missing {name}=={locked[name]}")
        elif name not in locked:
            problems.append(f"not in the lock {name}=={installed[name]}")
        elif locked[name] != installed[name]:
            problems.append(f"{name} is {installed[name]}, the lock pins {locked[name]}")
    return problems


def installed_distributions() -> dict[str, str]:
    """{canonical name: version} of the running interpreter's environment."""
    from importlib import metadata

    found: dict[str, str] = {}
    for dist in metadata.distributions():
        name = dist.metadata["Name"]
        if name:
            # The first path entry wins, the same rule import uses.
            found.setdefault(canonical(name), dist.version)
    return found


def wheel_platforms(filename: str, python: tuple[int, int]) -> set[str]:
    """The Pixi platforms a wheel installs on for this CPython version."""
    parts = filename[:-len(".whl")].split("-")
    if not filename.endswith(".whl") or len(parts) < 5:
        return set()
    py_tags, abi_tag, plat_tags = parts[-3].split("."), parts[-2], parts[-1].split(".")
    major, minor = python
    exact = f"cp{major}{minor}"

    def python_ok(tag: str) -> bool:
        if tag in (f"py{major}", f"py{major}{minor}"):
            return True
        if match := re.fullmatch(rf"(?:py|cp){major}(\d+)", tag):
            older = int(match[1]) <= minor
            if abi_tag == "abi3":
                return tag.startswith("cp") and older
            if abi_tag == "none":
                return older if tag.startswith("py") else tag == exact
            return tag == exact and abi_tag == exact
        return False

    if not any(python_ok(tag) for tag in py_tags):
        return set()
    found = set()
    for platform, rules in PLATFORMS.items():
        for plat in plat_tags:
            if plat == "any" or any(plat.startswith(prefix) and plat.endswith(suffixes)
                                    for prefix, suffixes in rules):
                found.add(platform)
    return found


def release_hashes(name: str, version: str, files: list[dict],
                   python: tuple[int, int]) -> tuple[list[str], bool]:
    """SHA-256 of what pip installs for one release, and whether it builds it.

    Every wheel that installs on a Pixi platform when wheels cover all of them.
    Otherwise the one sdist, so every platform builds the same source.
    """
    hashes, covered, sdists = [], set(), []
    for item in files:
        if item.get("packagetype") == "sdist":
            sdists.append(item)
            continue
        if item.get("packagetype") != "bdist_wheel":
            continue
        platforms = wheel_platforms(item["filename"], python)
        if not platforms:
            continue
        if item.get("yanked"):
            raise LockError(f"{name}=={version} is yanked, {item['filename']}")
        hashes.append(digest(item))
        covered |= platforms
    if not set(PLATFORMS) - covered:
        return sorted(set(hashes)), False
    if len(sdists) != 1:
        raise LockError(f"{name}=={version} has no wheel for every platform on Python "
                        f"{python[0]}.{python[1]} and {len(sdists)} sdists")
    if sdists[0].get("yanked"):
        raise LockError(f"{name}=={version} is yanked, {sdists[0]['filename']}")
    return [digest(sdists[0])], True


def digest(item: dict) -> str:
    value = item["digests"]["sha256"]
    if not SHA256.fullmatch(value):
        raise LockError(f"{item['filename']} has a malformed sha256")
    return value


def fetch_release(name: str, version: str) -> list[dict]:
    request = urllib.request.Request(PYPI_JSON.format(name=name, version=version),
                                     headers={"User-Agent": "idf-python-lock"})
    with urllib.request.urlopen(request, timeout=60) as response:
        return json.load(response)["urls"]


def sha256_file(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def write(args: argparse.Namespace) -> int:
    idf_path = Path(args.idf_path)
    python = sys.version_info[:2]
    with tempfile.TemporaryDirectory(prefix="idf-python-lock.") as scratch:
        tools = Path(scratch) / "tools"
        venv = Path(scratch) / f"idf{args.idf_version}_py{python[0]}.{python[1]}_env"
        tools.mkdir()
        venv.mkdir()
        env = dict(os.environ, IDF_PATH=str(idf_path), IDF_TOOLS_PATH=str(tools),
                   IDF_PYTHON_ENV_PATH=str(venv), IDF_PYTHON_CHECK_CONSTRAINTS="yes")
        # The SDK installer resolves the set exactly as a plain ESP-IDF install
        # would, with the constraints file of today.
        subprocess.run([sys.executable, str(idf_path / "tools/idf_tools.py"),
                        "install-python-env", "--features=core"], env=env, check=True)
        constraints = tools / f"espidf.constraints.v{args.idf_version}.txt"
        listing = subprocess.run([str(venv / "bin/python"), "-m", "pip", "list", "--format=json",
                                  "--disable-pip-version-check"],
                                 env=env, check=True, capture_output=True, text=True).stdout
        resolved = {canonical(item["name"]): item["version"] for item in json.loads(listing)}
        constraints_sha = sha256_file(constraints)
    pins, sources, problems = {}, [], []
    for name, version in sorted(resolved.items()):
        try:
            hashes, source = release_hashes(name, version, fetch_release(name, version), python)
        except LockError as error:
            problems.append(str(error))
            continue
        pins[name] = (version, hashes)
        if source:
            sources.append(name)
    if problems:
        print("cannot lock the IDF Python tools:", *problems, sep="\n  ", file=sys.stderr)
        return 1
    header = {
        "esp-idf-version": args.idf_version,
        "esp-idf-commit": args.idf_commit,
        "python": f"{python[0]}.{python[1]}",
        "platforms": " ".join(PLATFORMS),
        "requirements": f"{REQUIREMENTS} sha256={sha256_file(idf_path / REQUIREMENTS)}",
        "constraints": f"{CONSTRAINTS_URL.format(idf=args.idf_version)} sha256={constraints_sha}",
        "from-source": " ".join(sources) or "none",
        "generated": datetime.datetime.now(datetime.timezone.utc).date().isoformat(),
    }
    text = render_lock(header, pins)
    parse_lock(text)
    lock = Path(args.lock)
    lock.parent.mkdir(parents=True, exist_ok=True)
    lock.write_text(text)
    print(f"wrote {lock}, {len(pins)} packages, from source {' '.join(sources) or 'none'}, "
          f"constraints sha256 {constraints_sha}")
    return 0


def split_lock(text: str) -> tuple[str, str, list[str]]:
    """The lock as two pip requirement files, wheels and from-source, and the source names."""
    header, pins = parse_lock(text)
    sources = from_source(header)
    parts: tuple[dict, dict] = ({}, {})
    for name, pin in pins.items():
        parts[name in sources][name] = pin
    wheels, built = (render_lock(header, part) if part else "" for part in parts)
    return wheels, built, sources


def install(args: argparse.Namespace) -> int:
    try:
        wheels, built, sources = split_lock(Path(args.lock).read_text())
    except (OSError, LockError) as error:
        print(f"IDF Python lock {args.lock}: {error}", file=sys.stderr)
        return 2
    pip = [sys.executable, "-m", "pip"]
    flags = ["install", "--no-input", "--disable-pip-version-check", "--require-hashes",
             "--no-deps", "--only-binary=:all:"]
    env = dict(os.environ, PIP_USER="no")
    with tempfile.TemporaryDirectory(prefix="idf-python-install.") as scratch:
        steps = [(wheels, [])]
        if built:
            # Without isolation pip builds with what is installed, the locked
            # setuptools, and does not fetch unhashed build requirements. With
            # no cache it never reuses a wheel another backend built.
            steps.append((built, ["--no-build-isolation", "--no-cache-dir", f"--no-binary={','.join(sources)}"]))
        for index, (text, extra) in enumerate(steps):
            requirements = Path(scratch) / f"step{index}.txt"
            requirements.write_text(text)
            subprocess.run(pip + flags + extra + ["-r", str(requirements)], env=env, check=True)
    # --no-deps trusts the lock to be complete. pip check proves it is.
    subprocess.run(pip + ["check", "--disable-pip-version-check"], env=env, check=True)
    return check(args)


def check(args: argparse.Namespace) -> int:
    try:
        _, pins = parse_lock(Path(args.lock).read_text())
    except (OSError, LockError) as error:
        print(f"IDF Python lock {args.lock}: {error}", file=sys.stderr)
        return 2
    problems = drift({name: version for name, (version, _) in pins.items()}, installed_distributions())
    if problems:
        print(f"IDF Python venv {sys.prefix} differs from {args.lock}:", *problems,
              sep="\n  ", file=sys.stderr)
        return 1
    if not args.quiet:
        print(f"IDF Python venv matches {args.lock}, {len(pins)} packages")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    commands = parser.add_subparsers(dest="command", required=True)
    check_parser = commands.add_parser("check", help="compare the running venv with the lock")
    check_parser.add_argument("--lock", required=True)
    check_parser.add_argument("--quiet", action="store_true", help="print nothing on a match")
    install_parser = commands.add_parser("install", help="install the lock into the running venv")
    install_parser.add_argument("--lock", required=True)
    install_parser.set_defaults(quiet=False)
    write_parser = commands.add_parser("write", help="resolve with the SDK installer and write the lock")
    write_parser.add_argument("--lock", required=True)
    write_parser.add_argument("--idf-path", required=True)
    write_parser.add_argument("--idf-version", required=True)
    write_parser.add_argument("--idf-commit", required=True)
    args = parser.parse_args()
    return {"check": check, "install": install, "write": write}[args.command](args)


if __name__ == "__main__":
    sys.exit(main())
