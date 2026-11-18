"""Host test of the hashed ESP-IDF Python lock and its drift check (#176).

It proves the committed lock in tools/idf-python matches the pinned SDK, the
Pixi Python and the Pixi platforms, that the parser refuses a malformed lock,
that `check` reports each kind of drift, that the wheel tag rules pick the
right files, and that CI and the release run the check. No network and no SDK.
"""

from __future__ import annotations

from pathlib import Path
import re
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

import idf_python_lock as lock  # noqa: E402

LOCK_DIR = ROOT / "tools" / "idf-python"
TOOL = ROOT / "tools" / "idf_python_lock.py"
WORKFLOWS = [ROOT / ".github" / "workflows" / name for name in ("ci.yml", "release.yml")]
PY = sys.version_info[:2]


def pinned_idf() -> tuple[str, str]:
    text = (ROOT / "tools" / "idf-dependencies.lock").read_text()
    version = re.search(r'^ESP_IDF_VERSION="?([0-9.]+)"?', text, re.M)[1]
    commit = re.search(r'^ESP_IDF_COMMIT="?([0-9a-f]{40})"?', text, re.M)[1]
    return version, commit


def committed_lock() -> Path:
    version, _ = pinned_idf()
    major, minor = version.split(".")[:2]
    path = LOCK_DIR / f"idf{major}.{minor}_py{PY[0]}.{PY[1]}.txt"
    others = sorted(p.name for p in LOCK_DIR.iterdir() if p != path)
    assert path.is_file(), f"no lock for this SDK and Python, run pixi run idf-python-lock: {path}"
    assert not others, f"stale locks in tools/idf-python: {others}"
    return path


def check_committed() -> str:
    text = committed_lock().read_text()
    header, pins = lock.parse_lock(text)
    version, commit = pinned_idf()
    assert header["esp-idf-version"] == ".".join(version.split(".")[:2]), header
    assert header["esp-idf-commit"] == commit, "lock was made for another SDK commit"
    assert header["python"] == f"{PY[0]}.{PY[1]}", header
    pixi = (ROOT / "pixi.toml").read_text()
    platforms = re.search(r"^platforms = \[(.*)\]", pixi, re.M)[1]
    assert sorted(header["platforms"].split()) == sorted(re.findall(r'"([^"]+)"', platforms))
    assert sorted(header["platforms"].split()) == sorted(lock.PLATFORMS)
    assert re.fullmatch(rf"\S+ sha256={lock.SHA256.pattern}", header["constraints"]), header
    assert re.fullmatch(rf"{lock.REQUIREMENTS} sha256={lock.SHA256.pattern}", header["requirements"])
    for name in ("pip", "setuptools", "esptool", "idf-component-manager"):
        assert name in pins, f"{name} missing from the lock"
    assert lock.render_lock(header, pins) == text, "lock is not in generated form"
    return text


def check_malformed(text: str) -> int:
    first_pin = re.search(r"^[a-z].*==.*$", text, re.M)[0]
    first_hash = re.search(r"^    --hash=sha256:[0-9a-f]{64}.*$", text, re.M)[0]
    mutations = {
        "no header": re.sub(r"^# esp-idf-commit:.*\n", "", text, flags=re.M),
        "repeated header": text.replace("# python:", "# python: 3.0\n# python:", 1),
        "hash without package": text.replace(first_pin, "\n" + first_hash.strip(" \\") + "\n" + first_pin, 1),
        "package without hash": text + "zzz-extra==1.0\n",
        "duplicate package": text + "\n" + first_pin.rstrip(" \\") + " \\\n" + first_hash + "\n",
        "unsorted": text + "\naaa==1.0 \\\n    --hash=sha256:" + "0" * 64 + "\n",
        "bad line": text + "\nnot a pin\n",
        "short hash": text.replace(first_hash, "    --hash=sha256:abc \\", 1),
        "repeated hash": text.replace(first_hash, first_hash.rstrip(" \\") + " \\\n" + first_hash, 1),
        "unknown from-source": re.sub(r"^# from-source:.*$", "# from-source: zzz", text, flags=re.M),
        "unsorted from-source": re.sub(r"^# from-source:.*$", "# from-source: setuptools pip",
                                       text, flags=re.M),
        "from-source with two hashes": re.sub(r"^# from-source:.*$", "# from-source: pydantic-core",
                                              text, flags=re.M),
        "empty": "",
    }
    for name, mutated in mutations.items():
        assert mutated != text, f"mutation {name} no longer applies"
        try:
            lock.parse_lock(mutated)
        except lock.LockError:
            continue
        raise AssertionError(f"malformed lock accepted, {name}")
    return len(mutations)


def check_split(text: str) -> None:
    header, pins = lock.parse_lock(text)
    wheels, built, sources = lock.split_lock(text)
    assert sources == lock.from_source(header)
    for name, (version, _) in pins.items():
        part = built if name in sources else wheels
        assert f"\n{name}=={version} \\\n" in part, name
    names = {line.split("==")[0] for line in (wheels + built).splitlines() if "==" in line}
    assert names == set(pins), "split lost or added a package"
    assert lock.BUILD_BACKEND not in sources, "the build backend must come from a wheel"


def check_drift() -> None:
    locked = {"a": "1.0", "b": "2.0"}
    assert lock.drift(locked, dict(locked)) == []
    assert lock.drift(locked, {"a": "1.0"}) == ["missing b==2.0"]
    assert lock.drift(locked, {**locked, "c": "3"}) == ["not in the lock c==3"]
    assert lock.drift(locked, {"a": "1.1", "b": "2.0"}) == ["a is 1.1, the lock pins 1.0"]
    assert lock.canonical("Ruamel.Yaml") == "ruamel-yaml"
    assert lock.canonical("tree_sitter") == "tree-sitter"


def check_wheel_tags() -> None:
    py = (3, 15)
    both = set(lock.PLATFORMS)
    cases = {
        "x-1-py3-none-any.whl": both,
        "x-1-py2.py3-none-any.whl": both,
        "x-1-cp315-cp315-macosx_11_0_arm64.whl": {"osx-arm64"},
        "x-1-cp315-cp315-macosx_10_9_universal2.whl": {"osx-arm64"},
        "x-1-cp315-cp315-macosx_10_9_x86_64.whl": set(),
        "x-1-cp315-cp315-manylinux_2_17_x86_64.manylinux2014_x86_64.whl": {"linux-64"},
        "x-1-cp315-cp315-manylinux_2_17_aarch64.whl": set(),
        "x-1-cp315-cp315-musllinux_1_2_x86_64.whl": set(),
        "x-1-cp314-cp314-manylinux_2_17_x86_64.whl": set(),
        "x-1-cp315-cp315t-manylinux_2_17_x86_64.whl": set(),
        "x-1-cp39-abi3-manylinux_2_17_x86_64.whl": {"linux-64"},
        "x-1-cp316-abi3-manylinux_2_17_x86_64.whl": set(),
        "x-1-cp315-cp315-win_amd64.whl": set(),
        "x-1.tar.gz": set(),
    }
    for filename, expected in cases.items():
        got = lock.wheel_platforms(filename, py)
        assert got == expected, f"{filename}: {got} != {expected}"

    def item(filename, kind="bdist_wheel", yanked=False, digest="a" * 64):
        return {"filename": filename, "packagetype": kind, "yanked": yanked,
                "digests": {"sha256": digest}}

    mac = item("x-1-cp315-cp315-macosx_11_0_arm64.whl", digest="1" * 64)
    linux = item("x-1-cp315-cp315-manylinux_2_17_x86_64.whl", digest="2" * 64)
    other = item("x-1-cp315-cp315-win_amd64.whl", digest="3" * 64)
    sdist = item("x-1.tar.gz", kind="sdist", digest="4" * 64)
    assert lock.release_hashes("x", "1", [mac, linux, other, sdist], py) == (["1" * 64, "2" * 64], False)
    assert lock.release_hashes("x", "1", [mac, sdist], py) == (["4" * 64], True)
    assert lock.release_hashes("x", "1", [sdist], py) == (["4" * 64], True)
    for name, files in {
        "no wheel and no sdist": [mac],
        "two sdists": [mac, sdist, item("x-1.zip", kind="sdist")],
        "yanked wheel": [mac, item(linux["filename"], yanked=True)],
        "yanked sdist": [item("x-1.tar.gz", kind="sdist", yanked=True)],
        "malformed digest": [mac, item(linux["filename"], digest="XYZ")],
    }.items():
        try:
            lock.release_hashes("x", "1", files, py)
        except lock.LockError:
            continue
        raise AssertionError(f"release_hashes accepted {name}")


def check_end_to_end(text: str) -> None:
    """`check` under a real venv's python, with fake installed distributions."""
    _, pins = lock.parse_lock(text)
    with tempfile.TemporaryDirectory(prefix="idf-python-lock-test.") as scratch:
        venv = Path(scratch) / "venv"
        subprocess.run([sys.executable, "-m", "venv", "--without-pip", str(venv)], check=True)
        python = venv / "bin" / "python"
        site = Path(subprocess.run([str(python), "-I", "-c", "import sysconfig; print(sysconfig.get_path('purelib'))"],
                                   check=True, capture_output=True, text=True).stdout.strip())

        def install(name: str, version: str) -> Path:
            info = site / f"{name.replace('-', '_')}-{version}.dist-info"
            info.mkdir(parents=True)
            (info / "METADATA").write_text(f"Metadata-Version: 2.1\nName: {name}\nVersion: {version}\n")
            return info

        def run(lock_path: Path) -> subprocess.CompletedProcess:
            return subprocess.run([str(python), "-I", str(TOOL), "check", "--lock", str(lock_path)],
                                  capture_output=True, text=True)

        lock_path = Path(scratch) / "lock.txt"
        lock_path.write_text(text)
        infos = {name: install(name, version) for name, (version, _) in pins.items()}
        result = run(lock_path)
        assert result.returncode == 0, result.stderr
        assert f"{len(pins)} packages" in result.stdout

        name = next(iter(pins))
        version = pins[name][0]
        (infos[name] / "METADATA").write_text(f"Metadata-Version: 2.1\nName: {name}\nVersion: 0.0\n")
        result = run(lock_path)
        assert result.returncode == 1 and f"{name} is 0.0, the lock pins {version}" in result.stderr, result

        install("zzz-extra", "1.0")
        result = run(lock_path)
        assert result.returncode == 1 and "not in the lock zzz-extra==1.0" in result.stderr, result

        assert run(Path(scratch) / "missing.txt").returncode == 2
        lock_path.write_text("garbage\n")
        assert run(lock_path).returncode == 2


def check_workflows() -> None:
    for workflow in WORKFLOWS:
        text = workflow.read_text()
        assert "run: pixi run idf-python-check" in text, f"{workflow.name} does not run idf-python-check"
        for key in re.findall(r"hashFiles\(([^)]*)\)", text):
            if "tools/idf-env.sh" in key:
                assert "'tools/idf-python/*'" in key, f"{workflow.name} IDF cache key ignores the lock"


def main() -> int:
    text = check_committed()
    mutations = check_malformed(text)
    check_split(text)
    check_drift()
    check_wheel_tags()
    check_end_to_end(text)
    check_workflows()
    print(f"idf python lock tests passed, {mutations} malformed locks refused")
    return 0


if __name__ == "__main__":
    sys.exit(main())
