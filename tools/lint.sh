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
cppcheck "${options[@]}" \
  ${board_other[@]+"${board_other[@]}"} \
  firmware/common/connectivity/src/ble_sync.cpp \
  firmware/common/connectivity/src/ble_policy.cpp \
  firmware/common/connectivity/src/wifi_link.cpp \
  firmware/common/connectivity/src/network_time.cpp \
  firmware/common/location/src/os_location.cpp \
  firmware/common/gnss/src/os_gnss.cpp \
  firmware/common/gnss_reader/src/os_gnss_reader.cpp \
  firmware/common/connectivity/src/sync_service.cpp \
  firmware/common/connectivity/src/archive_sync.cpp \
  firmware/common/connectivity/src/control_sync.cpp \
  firmware/common/runtime/src/device_config.cpp \
  firmware/common/runtime/src/debug_log.cpp \
  firmware/common/runtime/src/os_console.cpp \
  firmware/common/runtime/src/coredump_report.cpp \
  firmware/common/src/parquet_writer.cpp \
  firmware/common/src/parquet_reader.cpp \
  firmware/common/src/lz4_codec.cpp \
  firmware/common/sensors/src/pms_frame.cpp \
  firmware/common/power/src/os_battery_health.cpp \
  firmware/common/upload/src/os_upload_sigv4.cpp \
  firmware/common/upload/src/os_upload_sts.cpp \
  firmware/common/upload/src/os_upload_policy.cpp \
  firmware/common/upload/src/os_upload_ledger.cpp \
  firmware/common/upload/src/os_upload_session.cpp \
  firmware/common/logger/src/os_logger.cpp \
  firmware/common/logger/src/os_logger_status.cpp \
  firmware/common/logger/src/os_logger_provision.cpp \
  firmware/common/logger/src/os_logger_live.cpp
# A diagnostic explicitly rejects PSRAM, every telemetry app needs it for the
# writer workspace. Check each file in the configuration it is compiled with.
cppcheck "${options[@]}" -DCONFIG_SPIRAM=1 "${board_telemetry[@]}"
# The bench only parquet bench-stall branch is off in every shipped image, so
# check os_logger.cpp once more with it on (CONFIG_OS_LOGGER_BENCH_STALL).
cppcheck "${options[@]}" -DCONFIG_OS_LOGGER_BENCH_STALL=1 \
  firmware/common/logger/src/os_logger.cpp
