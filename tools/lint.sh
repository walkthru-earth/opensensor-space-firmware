#!/usr/bin/env bash
set -euo pipefail
repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${repo_root}"
# The C++ standard below must match the firmware helper and the host tests.
python tools/check_idf_target.py --cxx-dialect
# Shared code takes board capabilities from hooks, ServiceConfig and sdkconfig
# defaults, never from a board name (AGENTS.md, code boundaries). The check
# exits 0 when clean, 1 on a finding and 2 when it cannot run. Only 0 passes,
# so an error can never read as clean.
board_rc=0
python tools/check_board_conditionals.py firmware/common || board_rc=$?
case "${board_rc}" in
  0) ;;
  1) exit 1 ;;
  *) printf '!!! board conditional check failed to run (exit %s)\n' "${board_rc}" >&2; exit 1 ;;
esac
# The BLE name prefix is a board constant (#90). Shared code, host tools and
# their tests carry no product prefix literal. Same exit code contract.
prefix_rc=0
python tools/check_name_prefix.py || prefix_rc=$?
case "${prefix_rc}" in
  0) ;;
  1) exit 1 ;;
  *) printf '!!! name prefix check failed to run (exit %s)\n' "${prefix_rc}" >&2; exit 1 ;;
esac
# Retired AQ wire, NVS, Kconfig and serial names stay out of every tracked
# file but the dated history (#93). Same exit code contract.
retired_rc=0
python tools/check_retired_names.py || retired_rc=$?
case "${retired_rc}" in
  0) ;;
  1) exit 1 ;;
  *) printf '!!! retired name check failed to run (exit %s)\n' "${retired_rc}" >&2; exit 1 ;;
esac
# No path under secrets/ and no PEM private key may be tracked by git (#45).
# The app signing key stays on the owner machine. Same exit code contract.
secrets_rc=0
python tools/check_secrets.py || secrets_rc=$?
case "${secrets_rc}" in
  0) ;;
  1) exit 1 ;;
  *) printf '!!! tracked secrets check failed to run (exit %s)\n' "${secrets_rc}" >&2; exit 1 ;;
esac
# Board sources come from the board registry (#141), tracked .cpp files of
# each board.json project. The telemetry app holds board.json, every other
# file is a board helper or a bench diagnostic. A failing registry stops lint.
telemetry_list="$(python tools/boards.py sources telemetry)"
other_list="$(python tools/boards.py sources other)"
board_telemetry=()
while IFS= read -r file; do
  if [[ -n "${file}" ]]; then board_telemetry+=("${file}"); fi
done <<<"${telemetry_list}"
board_other=()
while IFS= read -r file; do
  if [[ -n "${file}" ]]; then board_other+=("${file}"); fi
done <<<"${other_list}"
if [[ ${#board_telemetry[@]} -eq 0 ]]; then
  printf '!!! board registry listed no telemetry app sources\n' >&2; exit 1
fi
options=(--language=c++ --enable=warning,style,performance,portability
  --inline-suppr --error-exitcode=1 --std=c++23 --max-configs=1
  '--suppress=*:*/vendor/lz4/*')
# Shared sources cppcheck reads. The list is by hand, and the check below
# fails when a tracked shared source is in neither list, so a new file cannot
# skip cppcheck unnoticed.
common_sources=(
  firmware/common/connectivity/src/ble_sync.cpp
  firmware/common/connectivity/src/ble_policy.cpp
  firmware/common/connectivity/src/wifi_link.cpp
  firmware/common/connectivity/src/network_time.cpp
  firmware/common/stac/src/os_stac_item.cpp
  firmware/common/gnss/src/os_gnss.cpp
  firmware/common/gnss_reader/src/os_gnss_reader.cpp
  firmware/common/connectivity/src/sync_service.cpp
  firmware/common/connectivity/src/archive_sync.cpp
  firmware/common/connectivity/src/control_sync.cpp
  firmware/common/runtime/src/device_config.cpp
  firmware/common/runtime/src/debug_log.cpp
  firmware/common/runtime/src/os_console.cpp
  firmware/common/runtime/src/coredump_report.cpp
  firmware/common/src/parquet_writer.cpp
  firmware/common/src/parquet_reader.cpp
  firmware/common/src/lz4_codec.cpp
  firmware/common/src/sha256_stream.cpp
  firmware/common/sensors/src/pms_frame.cpp
  firmware/common/power/src/os_battery_health.cpp
  firmware/common/upload/src/os_upload_sigv4.cpp
  firmware/common/upload/src/os_upload_sts.cpp
  firmware/common/upload/src/os_upload_policy.cpp
  firmware/common/upload/src/os_upload_ledger.cpp
  firmware/common/upload/src/os_upload_session.cpp
  firmware/common/logger/src/os_logger.cpp
  firmware/common/logger/src/os_logger_status.cpp
  firmware/common/logger/src/os_logger_provision.cpp
  firmware/common/logger/src/os_logger_live.cpp
  firmware/common/connectivity/src/disconnect.cpp
  firmware/common/location/src/os_h3_alloc.cpp
  firmware/common/runtime/src/log_persist.cpp
  firmware/common/runtime/src/power.cpp
  firmware/common/upload/src/os_upload_device.cpp
)
# Tracked shared sources cppcheck does not read yet. Each one has open
# findings that need a source change of its own. Remove a line once its file
# is clean and add the file to common_sources.
common_excluded=(
  # shadowFunction, argument capacity in FrameRing::attach
  firmware/common/connectivity/src/frame_ring.cpp
  # variableScope, static project buffer
  firmware/common/connectivity/src/ota_lan.cpp
  # useStlAlgorithm, two raw loops over the checks
  firmware/common/src/boot_self_test.cpp
  # shadowFunction, argument owner in drop_owner, begin and handle
  firmware/common/src/ota_transfer.cpp
)
# Every tracked firmware/common .cpp in a src folder, vendor and tests aside,
# is in exactly one of the two lists, and every listed file is tracked.
tracked_list="$(git ls-files -- 'firmware/common/*.cpp' |
  grep -E '(^|/)src/[^/]+\.cpp$' | grep -Ev '/(vendor|tests)/' || true)"
if [[ -z "${tracked_list}" ]]; then
  printf '!!! git listed no shared sources under firmware/common\n' >&2; exit 1
fi
known=" ${common_sources[*]} ${common_excluded[*]} "
unlisted=()
while IFS= read -r file; do
  if [[ "${known}" != *" ${file} "* ]]; then unlisted+=("${file}"); fi
done <<<"${tracked_list}"
if [[ ${#unlisted[@]} -gt 0 ]]; then
  printf '!!! shared sources missing from the cppcheck lists in tools/lint.sh, %s\n' "${unlisted[*]}" >&2
  exit 1
fi
stale=()
for file in "${common_sources[@]}" "${common_excluded[@]}"; do
  if [[ $'\n'"${tracked_list}"$'\n' != *$'\n'"${file}"$'\n'* ]]; then stale+=("${file}"); fi
done
if [[ ${#stale[@]} -gt 0 ]]; then
  printf '!!! tools/lint.sh lists shared sources git does not track, %s\n' "${stale[*]}" >&2
  exit 1
fi
for file in "${common_excluded[@]}"; do
  if [[ " ${common_sources[*]} " == *" ${file} "* ]]; then
    printf '!!! %s is in both cppcheck lists in tools/lint.sh\n' "${file}" >&2; exit 1
  fi
done
cppcheck "${options[@]}" \
  ${board_other[@]+"${board_other[@]}"} \
  "${common_sources[@]}"
# A diagnostic explicitly rejects PSRAM, every telemetry app needs it for the
# writer workspace. Check each file in the configuration it is compiled with.
cppcheck "${options[@]}" -DCONFIG_SPIRAM=1 "${board_telemetry[@]}"
# The bench only parquet bench-stall branch is off in every shipped image, so
# check os_logger.cpp once more with it on (CONFIG_OS_LOGGER_BENCH_STALL).
cppcheck "${options[@]}" -DCONFIG_OS_LOGGER_BENCH_STALL=1 \
  firmware/common/logger/src/os_logger.cpp
