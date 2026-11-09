#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
board=""
port=""
image=""
while [[ $# -gt 0 ]]; do
  case "$1" in
    --board) board="${2:-}"; shift 2 ;;
    --port) port="${2:-}"; shift 2 ;;
    --*) printf 'unknown option: %s\n' "$1" >&2; exit 2 ;;
    *)
      if [[ -n "${image}" ]]; then
        printf 'only one backup image may be restored\n' >&2
        exit 2
      fi
      image="$1"; shift ;;
  esac
done
if [[ -z "${board}" || -z "${port}" || -z "${image}" ]]; then
  printf 'usage: %s --board <board> --port <checked-port> backup/<matching-file>.bin\n' "$0" >&2
  python "${repo_root}/tools/boards.py" list >&2 || true
  exit 2
fi
# Any name of a board.json board (#141). The image name starts with one of
# the board's backup prefixes, cores3-flash-*.bin included for CoreS3.
assignments="$(python "${repo_root}/tools/boards.py" resolve "${board}")"
eval "${assignments}"
matched=0
for prefix in ${backup_prefixes}; do
  case "$(basename "${image}")" in
    "${prefix}"-flash-*.bin) matched=1 ;;
  esac
done
if [[ "${matched}" != 1 ]]; then
  printf 'backup name does not match board %s, expected one of %s followed by -flash-*.bin: %s\n' \
    "${board}" "${backup_prefixes}" "${image}" >&2
  exit 2
fi
if [[ ! -f "${image}" || "$(wc -c <"${image}" | tr -d " ")" != 16777216 ]]; then
  printf 'expected a complete 16777216-byte image: %s\n' "${image}" >&2
  exit 2
fi
# Keep the transport at esptool's default speed. The checked port and image
# remain explicit because this operation overwrites the entire flash.
exec python -m esptool --chip esp32s3 --port "${port}" write-flash 0 "${image}"
