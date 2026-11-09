"""Host test that pins the safety rules of .github/workflows/release.yml (#45).

actionlint and zizmor catch unpinned actions and template injection, but not a
widened job permission, persisted checkout credentials, a cache, a secrets
reference, a lost tag gate, a lost --draft or a weaker tag pattern. check()
reads the workflow and returns one finding per broken rule. main() proves the
real file is clean and that each mutation in MUTATIONS gives a finding, so a
rule that stops matching anything fails here too.

PyYAML is pinned in pixi.toml for this test.
"""

from __future__ import annotations

import fnmatch
import os
from pathlib import Path
import re
import stat
import subprocess
import sys
import tempfile

import yaml

ROOT = Path(__file__).resolve().parents[1]
WORKFLOW = ROOT / ".github" / "workflows" / "release.yml"
DOCS = ROOT / "docs" / "shared" / "development.md"

TAG_PATTERN = "^v[0-9A-Za-z._-]+$"
RELEASE_IF = "github.event_name == 'push' && github.ref_type == 'tag'"
SHA_PIN = re.compile(r"^[^@\s]+@[0-9a-f]{40}$")
# Any secrets context inside an expression, such as secrets.X, secrets['X'] or
# toJSON(secrets).
SECRETS_EXPR = re.compile(r"\$\{\{[^}]*\bsecrets\b")
# The one REST call the workflow may make, the guard's read of every release.
# releases/latest skips drafts, so a narrower read would miss an earlier draft.
GUARD_API = 'tags="$(gh api --paginate "repos/${GITHUB_REPOSITORY}/releases" --jq \'.[].tag_name\')"'
TAG_CHECK = re.compile(r'^if \[\[ ! "\$\{[A-Z_]+\}" =~ (\S+) \]\]; then$')
PERMISSIONS = {
    "build": {"contents": "read"},
    "assemble": {},
    "release": {"contents": "write", "id-token": "write", "attestations": "write"},
}


def steps(job: dict) -> list[dict]:
    return job.get("steps") or []


def uses(step: dict) -> str:
    return str(step.get("uses", "")).split("@", 1)[0]


def if_blocks(run: str, opener: re.Pattern[str]) -> list[list[str]]:
    """The stripped `then` branch lines of every `if` whose first line matches
    opener. An `else` or `elif` branch is left out, so an `exit 1` there cannot
    pass for the failure exit. Lines before the opener are skipped, so the
    opener may sit inside an outer if, as the tag check in release.yml does.
    The block ends at the first `fi` after the opener, so an if nested inside
    the `then` branch would end it early."""
    blocks, body, in_then = [], None, False
    for line in (raw.strip() for raw in run.splitlines()):
        if body is None:
            if opener.match(line):
                body, in_then = [], True
        elif line == "fi":
            blocks.append(body)
            body = None
        elif line == "else" or line.startswith("elif "):
            in_then = False
        elif in_then:
            body.append(line)
    if body is not None:
        blocks.append(body)
    return blocks


def check(text: str) -> list[str]:
    """Every broken rule in the workflow text, empty when all hold."""
    found = []
    if "secrets." in text or "secrets[" in text or SECRETS_EXPR.search(text):
        found.append("the workflow references secrets, it must only use github.token")
    try:
        doc = yaml.safe_load(text)
    except yaml.YAMLError as error:
        return [*found, f"not valid YAML, {error}"]
    # PyYAML reads the bare key `on` as True.
    triggers = doc.get("on", doc.get(True))
    if not isinstance(triggers, dict) or set(triggers) != {"push", "workflow_dispatch"}:
        found.append("triggers must be exactly push and workflow_dispatch")
    elif triggers["push"] != {"tags": ["v*"]}:
        found.append("push must trigger on v* tags only")
    if doc.get("permissions") != {}:
        found.append("top level permissions must be {}")
    jobs = doc.get("jobs") or {}
    if set(jobs) != set(PERMISSIONS):
        found.append(f"jobs must be exactly {sorted(PERMISSIONS)}")
    for name, want in PERMISSIONS.items():
        job = jobs.get(name) or {}
        if job.get("permissions") != want:
            found.append(f"job {name} permissions must be {want}")
    for name, job in jobs.items():
        for step in steps(job):
            ref = str(step.get("uses", ""))
            if ref and not SHA_PIN.match(ref):
                found.append(f"job {name} uses {ref} without a full commit SHA")
            if uses(step) == "actions/checkout" and (step.get("with") or {}).get("persist-credentials") is not False:
                found.append(f"job {name} checkout must set persist-credentials false")
            if uses(step) == "prefix-dev/setup-pixi":
                options = step.get("with") or {}
                if options.get("cache") is not False or "cache-write" in options:
                    found.append(f"job {name} setup-pixi must set cache false")
            if uses(step).startswith("actions/cache"):
                found.append(f"job {name} uses {uses(step)}, release builds take no cache")
    release = jobs.get("release") or {}
    if release.get("if") != RELEASE_IF:
        found.append(f"job release must run only if {RELEASE_IF}")
    for name, job in jobs.items():
        if name != "release" and "if" in job:
            found.append(f"job {name} must not be skipped")

    run = {name: "\n".join(str(step.get("run", "")) for step in steps(job)) for name, job in jobs.items()}
    for name in ("build", "release"):
        patterns = re.findall(r'=~ (\S+) \]\]', run.get(name, ""))
        if patterns != [TAG_PATTERN]:
            found.append(f"job {name} must check the tag once against {TAG_PATTERN}, found {patterns}")
        # The check fails the job only while its own block exits 1.
        blocks = if_blocks(run.get(name, ""), TAG_CHECK)
        if len(blocks) != 1 or "exit 1" not in blocks[0]:
            found.append(f"job {name} tag check must exit 1 when the tag does not match")
    create = [line for line in run.get("release", "").splitlines() if "gh release" in line]
    if len(create) != 1 or "gh release create" not in create[0]:
        found.append("job release must call gh release only to create")
    release_run = run.get("release", "")
    for flag in ("--draft", "--verify-tag"):
        if not re.search(rf"^\s*{flag} \\$", release_run, re.M):
            found.append(f"gh release create must pass {flag}")
    if "--draft=false" in text or "--latest" in text or re.search(r"\bdraft\W{0,3}false", text):
        found.append("the workflow must never publish")
    # gh api and curl could patch or publish a release behind gh release.
    api = [line.strip() for line in text.splitlines() if "gh api" in line or "curl" in line]
    if api != [GUARD_API]:
        found.append(f"the only REST call must be the guard read {GUARD_API}, found {api}")
    order = [step.get("name", uses(step)) for step in steps(release)]
    wanted = ["Refuse an existing release", "Attest build provenance", "Create draft release"]
    if [step for step in order if step in wanted] != wanted:
        found.append(f"job release must run {wanted} in that order")
    guard = next((step for step in steps(release) if step.get("name") == wanted[0]), {})
    # Its own block must exit 1. The tag check in the same step exits 1 too,
    # so a search of the whole step would hide a lost guard exit.
    blocks = if_blocks(str(guard.get("run", "")), re.compile(r'^if grep -Fxq -- "\$\{TAG\}" <<<"\$\{tags\}"; then$'))
    if len(blocks) != 1 or "exit 1" not in blocks[0]:
        found.append("the existing release guard must fail when the tag has a release")

    # The board artifacts and the joined artifact must not share a pattern.
    uploaded = {name: [str((step.get("with") or {}).get("name", "")) for step in steps(job)
                       if uses(step) == "actions/upload-artifact"] for name, job in jobs.items()}
    pattern = next((str(step["with"].get("pattern")) for step in steps(jobs.get("assemble") or {})
                    if uses(step) == "actions/download-artifact" and "pattern" in (step.get("with") or {})), "")
    boards = [n.replace("${{ matrix.name }}", "cores3") for n in uploaded.get("build", [])]
    if not pattern or not boards or not all(fnmatch.fnmatchcase(n, pattern) for n in boards):
        found.append("the assemble download pattern must match every board artifact")
    if any(fnmatch.fnmatchcase(n, pattern) for n in uploaded.get("assemble", [])):
        found.append(f"the assemble download pattern {pattern} also matches the joined artifact")
    return found


def check_tag_pattern() -> None:
    tag = re.compile(TAG_PATTERN)
    for good in ("v1.2.0", "v0.1.0-rc.1", "v2_beta"):
        assert tag.match(good), good
    for bad in ("v", "1.2.0", "v1/2", "v1 2", "v1;rm", "v1$(x)", "../v1", "v1\n"):
        assert not tag.fullmatch(bad), bad


# Each break must change the file and give at least one finding.
MUTATIONS = [
    ("build gets contents write", "    permissions:\n      contents: read\n",
     "    permissions:\n      contents: write\n"),
    ("checkout keeps credentials", "persist-credentials: false", "persist-credentials: true"),
    ("setup-pixi cache on", "cache: false", "cache: true"),
    ("secrets token", "GH_TOKEN: ${{ github.token }}\n          TAG",
     "GH_TOKEN: ${{ secrets.RELEASE_PAT }}\n          TAG"),
    ("release gate removed", f"    if: {RELEASE_IF}\n", ""),
    ("release gate widened", f"    if: {RELEASE_IF}\n", "    if: github.ref_type == 'tag'\n"),
    ("draft removed", "            --draft \\\n", ""),
    ("verify-tag removed", "            --verify-tag \\\n", ""),
    ("tag regex weakened", '=~ ^v[0-9A-Za-z._-]+$ ]]', '=~ ^v ]]'),
    ("publish added", '--notes "${notes}"', '--notes "${notes}"\n          gh release edit "${TAG}" --draft=false'),
    ("guard removed", 'if grep -Fxq -- "${TAG}" <<<"${tags}"; then', 'if false; then'),
    ("guard exit lost", 'then re-run."\n            exit 1', 'then re-run."\n            true'),
    ("guard reads only the latest release", '"repos/${GITHUB_REPOSITORY}/releases" --jq',
     '"repos/${GITHUB_REPOSITORY}/releases/latest" --jq'),
    ("build tag check exit lost", 'must match v[0-9A-Za-z._-]+"\n              exit 1',
     'must match v[0-9A-Za-z._-]+"\n              true'),
    ("release tag check exit lost", 'must match v[0-9A-Za-z._-]+"\n            exit 1',
     'must match v[0-9A-Za-z._-]+"\n            true'),
    ("guard exit moved to else", 'then re-run."\n            exit 1', 'then re-run."\n            else\n            exit 1'),
    ("release tag check exit moved to else", 'must match v[0-9A-Za-z._-]+"\n            exit 1',
     'must match v[0-9A-Za-z._-]+"\n            else\n            exit 1'),
    ("all secrets as JSON", "GH_TOKEN: ${{ github.token }}\n          TAG",
     "GH_TOKEN: ${{ github.token }}\n          ALL: ${{ toJSON(secrets) }}\n          TAG"),
    ("publish through the REST API", '--notes "${notes}"',
     '--notes "${notes}"\n          gh api -X PATCH "repos/${GITHUB_REPOSITORY}/releases/1" -F draft=false'),
    ("publish through curl", '--notes "${notes}"',
     '--notes "${notes}"\n          curl -X PATCH -d \'{"draft":false}\' "https://api.github.com/x"'),
    ("artifact pattern overlaps", "pattern: board-*", "pattern: '*'"),
    ("action unpinned", "actions/attest-build-provenance@4d101475d8b20a2381f78447822ac1eab6504dd8",
     "actions/attest-build-provenance@v4"),
    ("pull_request_target", "  workflow_dispatch:\n", "  workflow_dispatch:\n  pull_request_target:\n"),
    ("top level write-all", "\npermissions: {}\n", "\npermissions: write-all\n"),
    ("cache action", "      - name: Install pinned ESP-IDF\n",
     "      - uses: actions/cache/restore@55cc8345863c7cc4c66a329aec7e433d2d1c52a9\n"
     "        with:\n          path: ~/.espressif\n          key: k\n"
     "      - name: Install pinned ESP-IDF\n"),
]


# A fake gh that fails `run list` FAKE_FAILS times, then succeeds with empty
# output FAKE_EMPTY times, as `--jq '.[0].databaseId // empty'` does before the
# run shows up, then prints FAKE_ID. It passes `run watch`. Each call appends
# one line to FAKE_LOG.
FAKE_GH = """#!/usr/bin/env bash
echo "$*" >> "${FAKE_LOG}"
if [[ "$1 $2" == "run watch" ]]; then exit 0; fi
calls="$(grep -c '^run list' "${FAKE_LOG}")"
if (( calls <= FAKE_FAILS )); then exit 1; fi
if (( calls <= FAKE_FAILS + FAKE_EMPTY )); then exit 0; fi
echo "${FAKE_ID}"
"""


def wait_snippet() -> str:
    """The shell block of step 2 in the docs Cutting a release steps."""
    docs = DOCS.read_text(encoding="utf-8")
    step = docs.split("2. Wait for the Release workflow", 1)[1]
    return step.split("```sh\n", 1)[1].split("```", 1)[0]


def check_wait_loop() -> None:
    """The docs wait loop gives up after its cap even when gh keeps failing,
    and watches the run once gh finds it (#45)."""
    snippet = wait_snippet()
    assert "until " not in snippet and "while true" not in snippet, snippet
    with tempfile.TemporaryDirectory() as raw:
        bin_dir = Path(raw)
        fake_sleep = '#!/usr/bin/env bash\necho "sleep $*" >> "${FAKE_LOG}"\n'
        for name, body in (("gh", FAKE_GH), ("sleep", fake_sleep)):
            (bin_dir / name).write_text(body)
            (bin_dir / name).chmod(0o755 | stat.S_IXUSR)
        # (gh failures, empty successes, polls, exit code). Both failure
        # modes give up after 30 polls, and a hit after either one is watched.
        cases = ((1000, 0, 30, 1), (0, 1000, 30, 1), (3, 0, 4, 0), (0, 3, 4, 0), (2, 2, 5, 0))
        for fails, empty, polls, want_rc in cases:
            log = bin_dir / f"log-{fails}-{empty}"
            env = dict(os.environ, PATH=f"{bin_dir}{os.pathsep}{os.environ['PATH']}", FAKE_LOG=str(log),
                       FAKE_FAILS=str(fails), FAKE_EMPTY=str(empty), FAKE_ID="4242")
            result = subprocess.run(["bash", "-c", snippet], env=env, capture_output=True, text=True,
                                    timeout=30, check=False)
            assert result.returncode == want_rc, (fails, empty, result.returncode, result.stderr)
            calls = ["run list" if call.startswith("run list ") else call for call in log.read_text().splitlines()]
            # Each poll but the last is followed by one sleep. A hit is watched
            # once, and a miss is reported and never watched.
            want = []
            for poll in range(polls):
                want.append("run list")
                if poll < polls - 1:
                    want.append("sleep 10")
            if not want_rc:
                want.append("run watch 4242 --exit-status")
            assert calls == want, (fails, empty, calls)
            assert ("after 5 minutes" in result.stderr) == bool(want_rc), result.stderr

def main() -> int:
    text = WORKFLOW.read_text(encoding="utf-8")
    clean = check(text)
    assert not clean, clean
    check_tag_pattern()
    check_wait_loop()
    for name, old, new in MUTATIONS:
        assert old in text, f"mutation {name} no longer applies, update it"
        found = check(text.replace(old, new))
        assert found, f"mutation {name} was not caught"
    print(f"release workflow tests passed, {len(MUTATIONS)} mutations caught")
    return 0


if __name__ == "__main__":
    sys.exit(main())
