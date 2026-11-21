"""Host tests for release_sign.py, the local app signing task (#45).

Every run uses a throwaway git checkout in a temp dir with its own secrets/
and a throwaway RSA-3072 key from `espsecure generate-signing-key`. The real
secrets/ directory is never created, read or listed.
"""

from __future__ import annotations

import contextlib
import hashlib
import io
import os
from pathlib import Path
import subprocess
import tempfile

from cryptography.hazmat.primitives import serialization
from cryptography.hazmat.primitives.asymmetric import rsa

import release_sign as rs

KEY_NAME = "app_signing_key.pem"


def git(repo: Path, *args: str) -> None:
    subprocess.run(["git", "-C", str(repo), *args], check=True, capture_output=True)


def espsecure(*args: str) -> subprocess.CompletedProcess:
    return subprocess.run(rs.ESPSECURE + list(args), capture_output=True, text=True, check=False)


def run(args: list[str]) -> tuple[int, str, str]:
    out, err = io.StringIO(), io.StringIO()
    with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
        code = rs.main(args)
    return code, out.getvalue(), err.getvalue()


def make_key(path: Path, scheme: str = "rsa3072", version: str = "2") -> None:
    result = espsecure("generate-signing-key", "--version", version, "--scheme", scheme, str(path))
    assert result.returncode == 0, result.stdout + result.stderr
    path.chmod(0o600)


def make_repo(root: Path, name: str, env: str | None = None) -> Path:
    repo = root / name
    secrets = repo / "secrets"
    secrets.mkdir(parents=True)
    git(repo, "init", "-q")
    (repo / ".gitignore").write_text("/secrets\n")
    git(repo, "add", ".gitignore")
    secrets.chmod(0o700)
    make_key(secrets / KEY_NAME)
    (secrets / ".env").write_text(env if env is not None else
                                  f"# owner signing\nexport {rs.KEY_VAR}=\"{KEY_NAME}\"\n{rs.SCHEME_VAR}=rsa3072\n")
    (secrets / ".env").chmod(0o600)
    return repo


def images(root: Path) -> list[Path]:
    paths = []
    for index, size in enumerate((70_000, 4096 * 3 + 17)):
        path = root / f"img{index}" / f"aq_app{index}.bin"
        path.parent.mkdir(parents=True)
        # A real app image starts with the 0xE9 magic. sign-data does not care.
        path.write_bytes(b"\xe9" + bytes((index * 31 + n) % 251 for n in range(size - 1)))
        paths.append(path)
    return paths


def assert_no_leak(repo: Path, text: str) -> None:
    key = repo / "secrets" / KEY_NAME
    mode = key.stat().st_mode
    key.chmod(0o600)
    key_text = key.read_text(errors="replace")
    key.chmod(mode)
    body = "".join(key_text.splitlines()[1:-1])
    assert "PRIVATE" not in text and (len(body) < 40 or body[:40] not in text), text
    # The key path shows only as secrets/<name>, never absolute.
    assert str((repo / "secrets").resolve()) not in text and str(repo / "secrets") not in text, text


def check_parse_env() -> None:
    values = rs.parse_env("\n# c\nexport A=1\nB = 'two words'\nC=\"q\"\nD=plain # note\nE=\n")
    assert values == {"A": "1", "B": "two words", "C": "q", "D": "plain", "E": ""}, values
    for bad in ("JUSTTEXT", "1A=x", "A B=c"):
        try:
            rs.parse_env(f"OK=1\n{bad}\n")
        except rs.Refused as error:
            assert str(error) == "secrets/.env line 2 is not NAME=VALUE", error
            assert bad not in str(error)
        else:
            raise AssertionError(bad)
    # A comment after a quoted value is dropped with its quotes (#45).
    values = rs.parse_env("A=\"x\" # note\nB='y'# note\nC=\"a # b\"\nD=\"\"\n")
    assert values == {"A": "x", "B": "y", "C": "a # b", "D": ""}, values
    for bad in ('A="x', "A='x\"", 'A="x" y', 'A="x"y'):
        try:
            rs.parse_env(f"OK=1\n{bad}\n")
        except rs.Refused as error:
            assert str(error) == "secrets/.env line 2 has an unmatched quote or text after it", error
        else:
            raise AssertionError(bad)


def check_happy(root: Path) -> None:
    repo = make_repo(root, "happy")
    bins = images(root)
    out = root / "signed"
    code, stdout, stderr = run(["--repo", str(repo), "--out", str(out), *map(str, bins)])
    assert code == 0, stderr
    assert f"secrets/{KEY_NAME}" in stdout, stdout
    assert_no_leak(repo, stdout + stderr)
    sums = (out / rs.SUMS).read_text().splitlines()
    assert len(sums) == 3, sums
    # The public key is listed, holds no private part and is the key's own.
    public = out / rs.PUBLIC_KEY
    public_line = f"{hashlib.sha256(public.read_bytes()).hexdigest()}  {rs.PUBLIC_KEY}"
    assert public_line in sums, sums
    assert f"sha256 {public_line.split('  ')[0]}" in stdout, stdout
    sums.remove(public_line)
    assert "PRIVATE" not in public.read_text() and (public.stat().st_mode & 0o777) == 0o644
    private = serialization.load_pem_private_key((repo / "secrets" / KEY_NAME).read_bytes(), password=None)
    assert public.read_bytes() == private.public_key().public_bytes(
        serialization.Encoding.PEM, serialization.PublicFormat.SubjectPublicKeyInfo)
    for line, image in zip(sums, bins):
        digest, name = line.split("  ")
        signed = out / f"{image.stem}.signed.bin"
        assert name == signed.name and digest == hashlib.sha256(signed.read_bytes()).hexdigest(), line
        # Padded to a 4 KiB sector plus one 4 KiB signature sector.
        data = signed.read_bytes()
        assert len(data) % 4096 == 0 and len(data) >= len(image.read_bytes()) + 4096, len(data)
        assert data.startswith(image.read_bytes())
        result = espsecure("verify-signature", "--version", "2", "--keyfile", str(repo / "secrets" / KEY_NAME),
                           str(signed))
        assert result.returncode == 0, result.stdout + result.stderr
        # Anyone holding only the published public key can check it too.
        result = espsecure("verify-signature", "--version", "2", "--keyfile", str(public), str(signed))
        assert result.returncode == 0, result.stdout + result.stderr

    # A flipped byte fails verification, and the bad output is removed.
    tampered = out / "aq_app0.signed.bin"
    data = bytearray(tampered.read_bytes())
    data[100] ^= 0xFF
    tampered.write_bytes(bytes(data))
    key = (repo / "secrets" / KEY_NAME).resolve()
    try:
        rs.espsecure(["verify-signature", "--version", "2", "--keyfile", str(key), str(tampered)], key,
                     f"secrets/{KEY_NAME}")
    except rs.Refused as error:
        assert "verify-signature failed" in str(error) and str(key) not in str(error), error
    else:
        raise AssertionError("tampered image verified")

    # A key that differs from the one that signed fails as well.
    other = root / "other.pem"
    make_key(other)
    result = espsecure("verify-signature", "--version", "2", "--keyfile", str(other), str(out / "aq_app1.signed.bin"))
    assert result.returncode != 0
    # A tampered image fails against the public key as well.
    result = espsecure("verify-signature", "--version", "2", "--keyfile", str(public), str(tampered))
    assert result.returncode != 0


FAKE_ESPSECURE = """
import shutil, sys
args = sys.argv[1:]
key = args[args.index("--keyfile") + 1]
mode = sys.argv[0].rsplit("-", 1)[1].removesuffix(".py")
if args[0] == "sign-data":
    if mode == "sign":
        print(f"cannot sign with {key}")
        sys.exit(3)
    shutil.copyfile(args[-1], args[args.index("--output") + 1])
    sys.exit(0)
if args[0] == "verify-signature":
    print(f"signature does not match {key}", file=sys.stderr)
    sys.exit(0 if mode == "pass" else 4)
sys.exit(9)
"""


def check_fake_espsecure(root: Path) -> None:
    """A fake espsecure echoes the absolute key path and fails on cue, so the
    redaction, the verify step and the cleanup inside sign() are each pinned."""
    repo = make_repo(root, "fake")
    bins = images(root / "fake-in")
    real = rs.ESPSECURE
    try:
        for mode, failing in (("verify", "verify-signature"), ("sign", "sign-data")):
            fake = root / f"fake-espsecure-{mode}.py"
            fake.write_text(FAKE_ESPSECURE)
            rs.ESPSECURE = [real[0], str(fake)]
            out = root / f"fake-out-{mode}"
            code, stdout, stderr = run(["--repo", str(repo), "--out", str(out), *map(str, bins)])
            assert code == 1 and f"espsecure {failing} failed" in stderr, stderr
            # The fake printed the absolute key path. Only the label remains.
            assert f"secrets/{KEY_NAME}" in stderr, stderr
            assert_no_leak(repo, stdout + stderr)
            # The failed output is removed and no SHA256SUMS is written.
            assert sorted(path.name for path in out.iterdir()) == [], list(out.iterdir())
        # With verify passing the same fake run succeeds, so the refusal above
        # came from the verify step.
        fake = root / "fake-espsecure-pass.py"
        fake.write_text(FAKE_ESPSECURE)
        rs.ESPSECURE = [real[0], str(fake)]
        out = root / "fake-out-pass"
        code, _, stderr = run(["--repo", str(repo), "--out", str(out), *map(str, bins)])
        assert code == 0, stderr
        assert (out / rs.SUMS).is_file()
        # A failed rename of SHA256SUMS leaves no .SHA256SUMS.* temp file (#45).
        out = root / "fake-out-rename"
        replace = rs.os.replace

        def failing_replace(*_args: object) -> None:
            raise OSError("rename failed")

        rs.os.replace = failing_replace
        try:
            try:
                rs.sign(repo, out, bins)
            except OSError as error:
                assert str(error) == "rename failed", error
            else:
                raise AssertionError("a failed rename must raise")
        finally:
            rs.os.replace = replace
        left = sorted(path.name for path in out.iterdir() if path.name.startswith("."))
        assert left == [], left
        assert not (out / rs.SUMS).exists()
    finally:
        rs.ESPSECURE = real


def refused(repo: Path, out: Path, bins: list[Path], expect: str) -> None:
    code, stdout, stderr = run(["--repo", str(repo), "--out", str(out), *map(str, bins)])
    assert code == 1 and "release-sign: refused" in stderr and expect in stderr, (expect, stderr)
    assert_no_leak(repo, stdout + stderr)
    assert not (out / rs.SUMS).exists()


def check_refusals(root: Path) -> None:
    bins = images(root / "r")
    out = root / "r-out"
    secrets_key = f"secrets/{KEY_NAME}"

    repo = make_repo(root, "no-env")
    (repo / "secrets" / ".env").unlink()
    refused(repo, out, bins, "secrets/.env is missing")

    repo = make_repo(root, "tracked-key")
    git(repo, "add", "-f", secrets_key)
    refused(repo, out, bins, "git tracks 1 path(s) under secrets/")

    repo = make_repo(root, "tracked-env")
    git(repo, "add", "-f", "secrets/.env")
    refused(repo, out, bins, "under secrets/")

    repo = make_repo(root, "env-mode")
    (repo / "secrets" / ".env").chmod(0o644)
    refused(repo, out, bins, "secrets/.env is open to group or other (mode 644)")

    repo = make_repo(root, "key-mode")
    (repo / "secrets" / KEY_NAME).chmod(0o640)
    refused(repo, out, bins, f"{secrets_key} is open to group or other (mode 640)")

    # Write or execute bits for group or other count too, not just read bits.
    repo = make_repo(root, "key-mode-write")
    (repo / "secrets" / KEY_NAME).chmod(0o620)
    refused(repo, out, bins, f"{secrets_key} is open to group or other (mode 620)")
    repo = make_repo(root, "env-mode-write")
    (repo / "secrets" / ".env").chmod(0o602)
    refused(repo, out, bins, "secrets/.env is open to group or other (mode 602)")

    # An owner who cannot read the key or .env gets a refusal, not a
    # traceback with the absolute path. Root reads any mode, so skip there.
    if os.geteuid() != 0:
        repo = make_repo(root, "key-unreadable")
        (repo / "secrets" / KEY_NAME).chmod(0o200)
        refused(repo, out, bins, f"{secrets_key} cannot be read (Permission denied), run chmod 600 {secrets_key}")
        repo = make_repo(root, "env-unreadable")
        (repo / "secrets" / ".env").chmod(0o200)
        refused(repo, out, bins, "secrets/.env cannot be read (Permission denied)")
        (repo / "secrets" / ".env").chmod(0o600)
    repo = make_repo(root, "env-not-utf8")
    (repo / "secrets" / ".env").write_bytes(f"{rs.KEY_VAR}={KEY_NAME}\n# \xff\xfe\n".encode("latin-1"))
    refused(repo, out, bins, "secrets/.env is not UTF-8 text")

    # The key size branch, an RSA key of the wrong size.
    repo = make_repo(root, "rsa2048")
    small = rsa.generate_private_key(public_exponent=65537, key_size=2048)
    (repo / "secrets" / KEY_NAME).write_bytes(small.private_bytes(
        serialization.Encoding.PEM, serialization.PrivateFormat.TraditionalOpenSSL, serialization.NoEncryption()))
    refused(repo, out, bins, f"{secrets_key} is not an RSA-3072 private key")

    # A symlinked SHA256SUMS or signed output in --out is refused, and the
    # file it points at is left alone.
    repo = make_repo(root, "out-links")
    for name in (rs.SUMS, rs.PUBLIC_KEY, f"{bins[0].stem}.signed.bin"):
        link_out = root / f"links-{len(name)}"
        link_out.mkdir()
        victim = root / f"victim-{len(name)}.txt"
        victim.write_text("keep me\n")
        (link_out / name).symlink_to(victim)
        code, stdout, stderr = run(["--repo", str(repo), "--out", str(link_out), *map(str, bins)])
        assert code == 1 and f"{name} is a symlink" in stderr, stderr
        assert victim.read_text() == "keep me\n" and (link_out / name).is_symlink()
        assert_no_leak(repo, stdout + stderr)

    repo = make_repo(root, "dir-mode")
    (repo / "secrets").chmod(0o755)
    refused(repo, out, bins, "secrets is open to group or other")

    repo = make_repo(root, "out-in-secrets")
    refused(repo, repo / "secrets" / "signed", bins, "output directory must not be inside secrets/")
    assert not (repo / "secrets" / "signed").exists()
    refused(repo, repo / "secrets", bins, "output directory must not be inside secrets/")

    repo = make_repo(root, "no-var", env="OTHER=1\n")
    refused(repo, out, bins, f"does not set {rs.KEY_VAR}")

    outside = root / "outside.pem"
    make_key(outside)
    for name in ("../../outside.pem", str(outside), "."):
        repo = make_repo(root, f"escape{len(name)}", env=f"{rs.KEY_VAR}={name}\n")
        refused(repo, out, bins, "must name a file inside secrets/")

    repo = make_repo(root, "link-escape")
    (repo / "secrets" / "link.pem").symlink_to(outside)
    (repo / "secrets" / ".env").write_text(f"{rs.KEY_VAR}=link.pem\n")
    refused(repo, out, bins, "must name a file inside secrets/")

    repo = make_repo(root, "missing-key", env=f"{rs.KEY_VAR}=nope.pem\n")
    refused(repo, out, bins, "secrets/nope.pem does not exist")

    repo = make_repo(root, "scheme", env=f"{rs.KEY_VAR}={KEY_NAME}\n{rs.SCHEME_VAR}=ecdsa256\n")
    refused(repo, out, bins, f"{rs.SCHEME_VAR} must be one of rsa3072")

    repo = make_repo(root, "ecdsa-key")
    (repo / "secrets" / KEY_NAME).unlink()
    make_key(repo / "secrets" / KEY_NAME, scheme="ecdsa256")
    refused(repo, out, bins, f"{secrets_key} is not an RSA-3072 private key")

    repo = make_repo(root, "not-pem")
    (repo / "secrets" / KEY_NAME).write_text("not a key\n")
    refused(repo, out, bins, f"{secrets_key} is not a PEM private key")

    repo = make_repo(root, "bad-line", env=f"{rs.KEY_VAR}={KEY_NAME}\nnot an assignment\n")
    refused(repo, out, bins, "secrets/.env line 2 is not NAME=VALUE")

    repo = make_repo(root, "inputs")
    refused(repo, out, [root / "missing.bin"], "is not a file")
    twin = root / "twin" / bins[0].name
    twin.parent.mkdir()
    twin.write_bytes(bins[0].read_bytes())
    refused(repo, out, [bins[0], twin], "would both write aq_app0.signed.bin")
    # An output that would replace an input, even one listed later, is refused
    # before anything is written.
    clash_dir = root / "clash"
    clash_dir.mkdir()
    first, second = clash_dir / "app.bin", clash_dir / "app.signed.bin"
    first.write_bytes(b"\xe9" * 64)
    second.write_bytes(b"\xe9" * 96)
    refused(repo, clash_dir, [first, second], "app.signed.bin is also an input")
    assert first.read_bytes() == b"\xe9" * 64 and second.read_bytes() == b"\xe9" * 96
    sums_input = clash_dir / rs.SUMS
    sums_input.write_bytes(b"\xe9" * 32)
    code, _, stderr = run(["--repo", str(repo), "--out", str(clash_dir), str(sums_input)])
    assert code == 1 and f"{rs.SUMS} is also an input" in stderr, stderr
    sums_input.unlink()
    inside = repo / "secrets" / "app.bin"
    inside.write_bytes(b"\xe9" * 64)
    refused(repo, out, [inside], "is inside secrets/")

    plain = root / "not-a-repo"
    (plain / "secrets").mkdir(parents=True)
    (plain / "secrets" / ".env").write_text(f"{rs.KEY_VAR}={KEY_NAME}\n")
    code, _, stderr = run(["--repo", str(plain), "--out", str(out), str(bins[0])])
    assert code == 1 and "cannot ask git" in stderr, stderr

    # Nothing was signed by any refused run.
    assert not out.exists() or not any(out.iterdir()), list(out.iterdir())


def main() -> None:
    with tempfile.TemporaryDirectory() as raw:
        root = Path(raw).resolve()
        check_parse_env()
        check_happy(root)
        check_refusals(root)
        check_fake_espsecure(root)
    print("release_sign tests passed")


if __name__ == "__main__":
    main()
