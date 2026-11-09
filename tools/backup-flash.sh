#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
board=""
port=""
while [[ $# -gt 0 ]]; do
  case "$1" in
    --board) board="${2:-}"; shift 2 ;;
    --port) port="${2:-}"; shift 2 ;;
    *) printf 'unknown argument: %s\n' "$1" >&2; exit 2 ;;
  esac
done
if [[ -z "${board}" ]]; then
  printf 'usage: %s --board <board> --port <checked-port>\n' "$0" >&2
  python "${repo_root}/tools/boards.py" list >&2 || true
  exit 2
fi
# Any name of a board.json board (#141). The image is named by the board's
# first backup prefix, tooling.backup_prefixes[0].
assignments="$(python "${repo_root}/tools/boards.py" resolve -- "${board}")"
eval "${assignments}"
board="${backup_prefix}"
if [[ -z "${port}" ]]; then
  printf 'a checked --port is required\n' >&2
  exit 2
fi
# Only use this after flash-id has independently confirmed 16 MB on this port.
backup_dir="${repo_root}/backup"
image="${backup_dir}/${board}-flash-$(date -u +%Y%m%dT%H%M%SZ).bin"
umask 077
mkdir -p "${backup_dir}"
# Read into a unique private path. A failed read must never leave a partial
# image under the restoreable backup name, and a same-second run must not
# replace an earlier verified backup.
temporary="$(mktemp "${backup_dir}/.${board}-flash.XXXXXXXX")"
trap 'rm -f "${temporary}"' EXIT
# Keep esptool at its default serial speed.
python -m esptool --chip esp32s3 --port "${port}" read-flash 0 0x1000000 "${temporary}"
bytes="$(wc -c <"${temporary}" | tr -d " ")"
if [[ "${bytes}" != 16777216 ]]; then
  printf 'backup size mismatch: %s bytes in %s\n' "${bytes}" "${temporary}" >&2
  exit 1
fi
# Complete the hash before publication; shasum failure leaves no final image.
digest="$(/usr/bin/shasum -a 256 "${temporary}" | awk '{print $1}')"
if [[ ! "${digest}" =~ ^[0-9a-f]{64}$ ]]; then
  printf 'backup SHA-256 could not be verified: %s\n' "${temporary}" >&2
  exit 1
fi
if ! ln "${temporary}" "${image}"; then
  printf 'backup already exists or could not be published: %s\n' "${image}" >&2
  exit 1
fi
printf '%s  %s\n' "${digest}" "${image}"
printf 'backup verified: %s (%s bytes)\n' "${image}" "${bytes}"
