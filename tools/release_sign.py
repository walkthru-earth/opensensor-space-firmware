"""Sign app images with the local app signing key (issue #45, owner machine only).

The key lives in secrets/ on the owner machine and is never a CI secret. See
docs/shared/development.md#local-app-signing for the one-time setup.
secrets/.env names the key

  OS_APP_SIGNING_KEY=<file name of the PEM, relative to secrets/>
  OS_APP_SIGNING_SCHEME=rsa3072   # optional, the only scheme ESP32-S3 has

Each unsigned app .bin is signed with `espsecure sign-data --version 2` into
the output directory as <stem>.signed.bin, then checked with
`espsecure verify-signature --version 2`. The public half of the key is
written as app_signing_public_key.pem, and every signed image is checked
against that file too, so anyone holding it can check the release. SHA256SUMS
in the output directory lists every signed image and the public key, in
sha256sum format.

It refuses to run when secrets/.env is missing, when anything under secrets/
is tracked by git, when the key or secrets/.env is open to group or other,
when the key is not an RSA-3072 PEM private key inside secrets/, or when the
output directory is inside secrets/. It never prints the key, the .env values
or any file content. The key is named only as secrets/<name>.

Firmware does not check app signatures yet. That waits on #40. Publishing
the public key is the owner's step, see the docs above.

Usage:
  pixi run release-sign --out <dir> <app.bin> [<app.bin> ...]
"""

from __future__ import annotations

import argparse
import hashlib
import os
from pathlib import Path
import re
import stat
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
KEY_VAR = "OS_APP_SIGNING_KEY"
SCHEME_VAR = "OS_APP_SIGNING_SCHEME"
# ESP32-S3 Secure Boot v2 verifies RSA-3072 only. ECDSA v2 is for other chips.
SCHEMES = {"rsa3072": 3072}
DEFAULT_SCHEME = "rsa3072"
ENV_NAME_RE = re.compile(r"[A-Za-z_][A-Za-z0-9_]*")
SUMS = "SHA256SUMS"
PUBLIC_KEY = "app_signing_public_key.pem"
ESPSECURE = [sys.executable, "-m", "espsecure"]


class Refused(Exception):
    """A precondition failed. The message never holds a secret value."""


def parse_env(text: str) -> dict[str, str]:
    """Minimal .env reader. KEY=VALUE lines, optional `export `, # comments,
    optional matching quotes. A bad line is named by number, never echoed."""
    values = {}
    for number, raw in enumerate(text.splitlines(), 1):
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        if line.startswith("export "):
            line = line[len("export "):].lstrip()
        name, sep, value = line.partition("=")
        name = name.strip()
        if not sep or not ENV_NAME_RE.fullmatch(name):
            raise Refused(f"secrets/.env line {number} is not NAME=VALUE")
        value = value.strip()
        if value[:1] in ("\"", "'"):
            # A quoted value ends at its closing quote. Only a comment may follow.
            end = value.find(value[0], 1)
            rest = value[end + 1:].strip()
            if end < 0 or (rest and not rest.startswith("#")):
                raise Refused(f"secrets/.env line {number} has an unmatched quote or text after it")
            value = value[1:end]
        elif " #" in value:
            value = value.split(" #", 1)[0].rstrip()
        values[name] = value
    return values


def git_tracked(repo: Path, rel: str) -> list[str]:
    """Tracked paths at or under rel in repo. Raises Refused when git fails."""
    try:
        result = subprocess.run(["git", "-C", str(repo), "ls-files", "-z", "--", rel],
                                capture_output=True, check=True)
    except (OSError, subprocess.CalledProcessError) as error:
        raise Refused(f"cannot ask git what is tracked in {repo}") from error
    return [name for name in result.stdout.decode("utf-8", "surrogateescape").split("\0") if name]


def private_mode(path: Path, label: str) -> None:
    mode = path.stat().st_mode
    if mode & (stat.S_IRWXG | stat.S_IRWXO):
        raise Refused(f"{label} is open to group or other (mode {stat.S_IMODE(mode):03o}), "
                      f"run chmod {'700' if stat.S_ISDIR(mode) else '600'} {label}")


def check_key(key: Path, label: str, bits: int) -> bytes:
    """The key must be an unencrypted RSA PEM private key of the scheme size.
    Returns its public key as a PEM SubjectPublicKeyInfo, which holds no secret."""
    from cryptography.hazmat.primitives import serialization
    from cryptography.hazmat.primitives.asymmetric import rsa

    try:
        data = key.read_bytes()
    except OSError as error:
        raise Refused(f"{label} cannot be read ({error.strerror}), run chmod 600 {label}") from None
    try:
        loaded = serialization.load_pem_private_key(data, password=None)
    except TypeError:
        raise Refused(f"{label} is password protected, espsecure needs an unencrypted PEM") from None
    except ValueError:
        raise Refused(f"{label} is not a PEM private key") from None
    if not isinstance(loaded, rsa.RSAPrivateKey) or loaded.key_size != bits:
        raise Refused(f"{label} is not an RSA-{bits} private key")
    return loaded.public_key().public_bytes(serialization.Encoding.PEM,
                                            serialization.PublicFormat.SubjectPublicKeyInfo)


def load_config(repo: Path) -> tuple[Path, str, str, bytes]:
    """Check secrets/ and return the key path, its printable label, the scheme
    and the public key PEM."""
    secrets = repo / "secrets"
    env_file = secrets / ".env"
    if not env_file.is_file():
        raise Refused("secrets/.env is missing, see docs/shared/development.md#local-app-signing")
    tracked = git_tracked(repo, "secrets")
    if tracked:
        raise Refused(f"git tracks {len(tracked)} path(s) under secrets/, untrack them and rotate the key")
    private_mode(secrets, "secrets")
    private_mode(env_file, "secrets/.env")
    try:
        text = env_file.read_text(encoding="utf-8")
    except OSError as error:
        raise Refused(f"secrets/.env cannot be read ({error.strerror}), run chmod 600 secrets/.env") from None
    except UnicodeDecodeError:
        raise Refused("secrets/.env is not UTF-8 text") from None
    values = parse_env(text)
    name = values.get(KEY_VAR, "")
    if not name:
        raise Refused(f"secrets/.env does not set {KEY_VAR}")
    scheme = values.get(SCHEME_VAR, "") or DEFAULT_SCHEME
    if scheme not in SCHEMES:
        raise Refused(f"{SCHEME_VAR} must be one of {', '.join(sorted(SCHEMES))} for ESP32-S3")
    secrets_real = secrets.resolve()
    key = (secrets / name).resolve()
    if not key.is_relative_to(secrets_real) or key == secrets_real:
        raise Refused(f"{KEY_VAR} must name a file inside secrets/")
    label = f"secrets/{key.relative_to(secrets_real).as_posix()}"
    if not key.is_file():
        raise Refused(f"{label} does not exist")
    private_mode(key, label)
    public = check_key(key, label, SCHEMES[scheme])
    return key, label, scheme, public


def espsecure(args: list[str], key: Path, label: str) -> None:
    """Run espsecure quietly. On failure show its output with the key path
    replaced by its label. espsecure never prints key material."""
    result = subprocess.run(ESPSECURE + args, capture_output=True, text=True, check=False)
    if result.returncode != 0:
        output = (result.stdout + result.stderr).replace(str(key), label).strip()
        raise Refused(f"espsecure {args[0]} failed (exit {result.returncode})\n{output}")


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for block in iter(lambda: handle.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def write_replace(out_real: Path, name: str, data: bytes) -> Path:
    """Write a fresh file and rename it over out_real/name. The rename replaces
    a link that appeared since the check, it never writes through one. A failed
    write or rename removes the temp file, so no .<name>.* is left."""
    target = out_real / name
    handle = tempfile.NamedTemporaryFile("wb", dir=out_real, prefix=f".{name}.", delete=False)
    try:
        with handle:
            handle.write(data)
        os.chmod(handle.name, 0o644)
        os.replace(handle.name, target)
    except BaseException:
        Path(handle.name).unlink(missing_ok=True)
        raise
    return target


def sign(repo: Path, out: Path, images: list[Path]) -> list[str]:
    secrets_real = (repo / "secrets").resolve()
    out_real = out.resolve()
    if out_real.is_relative_to(secrets_real):
        raise Refused("the output directory must not be inside secrets/")
    names = {}
    for image in images:
        if not image.is_file():
            raise Refused(f"{image} is not a file")
        if image.resolve().is_relative_to(secrets_real):
            raise Refused(f"input {image.name} is inside secrets/, sign a copy from the build directory")
        signed = f"{image.stem}.signed.bin"
        if signed in names:
            raise Refused(f"{image} and {names[signed]} would both write {signed}")
        names[signed] = image
    # No output may replace an input, including one signed later in the run.
    inputs = {image.resolve() for image in images}
    for written in [*names, PUBLIC_KEY, SUMS]:
        if (out_real / written).resolve() in inputs:
            raise Refused(f"{out_real / written} is also an input, choose another --out")
        # A write through a symlink would land on whatever it points at.
        if (out_real / written).is_symlink():
            raise Refused(f"{out_real / written} is a symlink, remove it or choose another --out")
    key, label, scheme, public = load_config(repo)
    out_real.mkdir(parents=True, exist_ok=True)
    print(f"signing {len(images)} image(s) with {label} ({scheme}, Secure Boot v2)")
    public_path = write_replace(out_real, PUBLIC_KEY, public)
    lines = [f"{sha256(public_path)}  {PUBLIC_KEY}"]
    for signed, image in names.items():
        target = out_real / signed
        target.unlink(missing_ok=True)
        try:
            espsecure(["sign-data", "--version", "2", "--keyfile", str(key), "--output", str(target),
                       str(image)], key, label)
            espsecure(["verify-signature", "--version", "2", "--keyfile", str(key), str(target)], key, label)
            # The published public key alone must verify it too.
            espsecure(["verify-signature", "--version", "2", "--keyfile", str(public_path), str(target)], key,
                      label)
        except Refused:
            # A refused run leaves no public key next to images it did not sign.
            target.unlink(missing_ok=True)
            public_path.unlink(missing_ok=True)
            raise
        lines.append(f"{sha256(target)}  {signed}")
        print(f"signed and verified {target}")
    text = "".join(f"{line}\n" for line in sorted(lines, key=lambda line: line.split("  ", 1)[1]))
    sums = write_replace(out_real, SUMS, text.encode("ascii"))
    print(f"wrote {public_path}, sha256 {lines[0].split('  ', 1)[0]}")
    print(f"wrote {sums}")
    return lines


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--out", type=Path, required=True, help="directory for the signed images and SHA256SUMS")
    parser.add_argument("--repo", type=Path, default=ROOT, help=argparse.SUPPRESS)
    parser.add_argument("images", type=Path, nargs="+", help="unsigned app .bin files")
    args = parser.parse_args(argv)
    try:
        sign(args.repo, args.out, args.images)
    except Refused as error:
        print(f"release-sign: refused, {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
