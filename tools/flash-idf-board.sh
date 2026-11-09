#!/usr/bin/env bash
set -euo pipefail
repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
if [[ $# -lt 3 || $# -gt 4 ]]; then
  printf 'usage: %s <board> <checked-port> <full-backup> [variant]\n' "$0" >&2
  python "${repo_root}/tools/boards.py" list >&2 || true
  exit 2
fi
port="$2"
backup="$3"
# Board names and variants come from board.json (#141), see build-idf-board.sh.
assignments="$(python "${repo_root}/tools/boards.py" resolve "$1" "${4:-}")"
eval "${assignments}"
trial_dir="${repo_root}/${project}"
board="${alias}"
if [[ -z "${port}" || ! -c "${port}" || ! -f "${backup}" ]]; then
  printf 'checked serial device and full backup required\n' >&2; exit 2
fi
case "$(basename "${backup}")" in
  "${backup_prefix}"-flash-*.bin) ;;
  *) printf 'backup does not match selected board, expected %s-flash-*.bin\n' "${backup_prefix}" >&2; exit 2 ;;
esac
if [[ "$(wc -c <"${backup}" | tr -d " ")" != 16777216 ]]; then
  printf 'backup must be exactly 16777216 bytes\n' >&2; exit 2
fi
build_dir="${trial_dir}/build/${variant}"
target="${board}/${variant}"
case "${OS_BENCH:-}" in
  "") ;;
  stall)
    build_dir="${build_dir}-bench-stall"
    target="${target} bench=stall"
    printf 'flashing a BENCH image with parquet bench-stall, restore a shipped build afterwards\n' >&2 ;;
  *) printf 'unknown OS_BENCH: %s (expected stall)\n' "${OS_BENCH}" >&2; exit 2 ;;
esac
if [[ ! -f "${build_dir}/os-build-target.txt" ]] || [[ "$(cat "${build_dir}/os-build-target.txt")" != "${target}" ]]; then
  printf 'build and verify the selected native firmware first\n' >&2; exit 2
fi
python "${repo_root}/tools/check_idf_target.py" "${build_dir}" --board "${board}" --variant "${variant}" --verify-manifest --backup "${backup}"
# flash the reviewed build without silently rebuilding it; never erase NVS.
# ESP-IDF 6.1 has no --no-deps option. Its normal flash action depends on a
# build, so consume the checked generated response file with pinned Pixi
# esptool directly. Only the manifest-verified four regions are written.
cd "${build_dir}"
exec python -m esptool --chip esp32s3 --port "${port}" --baud 115200 \
  --before default-reset --after hard-reset write-flash "@flash_args"
