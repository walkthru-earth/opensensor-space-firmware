#!/usr/bin/env bash
set -euo pipefail
repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
if [[ $# -lt 1 || $# -gt 2 ]]; then
  printf 'usage: %s <board> [variant]\n' "$0" >&2
  python "${repo_root}/tools/boards.py" list >&2 || true
  exit 2
fi
# The board list is every board.json (#141). A board is its alias, its
# footer slug or its backup prefix. No variant means the
# telemetry app, which holds board.json. Unknown names fail, never default.
assignments="$(python "${repo_root}/tools/boards.py" resolve "$1" "${2:-}")"
eval "${assignments}"
trial_dir="${repo_root}/${project}"
board="${alias}"
source "${trial_dir}/dependencies.env"
printf '%s  %s\n' \
  "${LZ4_C_SHA256}" "${repo_root}/firmware/common/vendor/lz4/lz4.c" \
  "${LZ4_H_SHA256}" "${repo_root}/firmware/common/vendor/lz4/lz4.h" \
  "${LZ4_LICENSE_SHA256}" "${repo_root}/firmware/common/vendor/lz4/LICENSE" | shasum -a 256 -c -
if [[ "${telemetry}" == 1 ]]; then
  digest="$(sed -n 's/.*"\([0-9a-f]\{64\}\)".*/\1/p' "${trial_dir}/${variant}/telemetry_dictionary_digest.h")"
  test "${#digest}" -eq 64
  printf '%s  %s\n' "${digest}" "${trial_dir}/${variant}/telemetry_fields.inc" | shasum -a 256 -c -
  # desc, the SHA-256 of the embedded board template (#85).
  python "${repo_root}/tools/check_board_template.py" "${trial_dir}/${variant}"
fi
# OS_BENCH=stall builds a bench image with tools/idf-bench-stall-sdkconfig.defaults
# in its own directory, so its sdkconfig never leaks into the shipped build.
build_dir="${trial_dir}/build/${variant}"
target="${board}/${variant}"
idf_args=(-D "OS_VARIANT=${variant}")
case "${OS_BENCH:-}" in
  "") ;;
  stall)
    if [[ "${telemetry}" != 1 ]]; then
      printf 'OS_BENCH=stall needs the logger runtime of the telemetry app, not %s\n' "${variant}" >&2; exit 2
    fi
    build_dir="${build_dir}-bench-stall"
    target="${target} bench=stall"
    idf_args+=(-D "OS_SDKCONFIG_OVERLAY=${repo_root}/tools/idf-bench-stall-sdkconfig.defaults") ;;
  *) printf 'unknown OS_BENCH: %s (expected stall)\n' "${OS_BENCH}" >&2; exit 2 ;;
esac
rm -f "${build_dir}/os-build-target.txt" "${build_dir}/os-build-sha256.json"
"${repo_root}/tools/idf.sh" -C "${trial_dir}" -B "${build_dir}" "${idf_args[@]}" build
if [[ "${telemetry}" == 1 ]]; then
  python "${repo_root}/tools/check_board_template.py" "${trial_dir}/${variant}" --build "${build_dir}"
fi
python "${repo_root}/tools/check_idf_target.py" "${build_dir}" --board "${board}" --variant "${variant}" --write-manifest
printf '%s\n' "${target}" > "${build_dir}/os-build-target.txt"
