#!/usr/bin/env bash
# Host gate shared by local runs and CI. Run it as `pixi run gate`.
#
# Every task below is an existing pixi task, run with its sanitizer flag when
# the script supports one. Tasks that need a board, a serial port, Bluetooth,
# a LAN peer, the network or the ESP-IDF SDK are deliberately absent. The SDK
# builds run in the CI board job instead.
#
# Each task runs under GNU timeout from the pixi coreutils pin. A task that
# outlives OS_GATE_TASK_TIMEOUT seconds (default 300) is stopped, reported as
# TIMEOUT by name, and killed 10 s later if it ignores SIGTERM. Anything left
# in the task's process group is then killed, so no stray process outlives it.
#
# Usage: tools/gate.sh [--keep-going]
#   --keep-going  run every task and report all failures at the end
set -uo pipefail

keep_going=0
case "${1:-}" in
  "") ;;
  --keep-going) keep_going=1 ;;
  *) printf 'usage: %s [--keep-going]\n' "$0" >&2; exit 2 ;;
esac

tasks=(
  # Formatting and static diagnostics
  "fmt-check"
  "lint"
  # Writer, reader, schema and dictionary contracts
  "common-test"
  "json-test"
  "parquet-test --sanitize"
  "geoparquet-test --sanitize"
  "reader-test"
  "rebuild-partial-test --sanitize"
  "bench-power-cut-test"
  "analyze-discharge-test"
  "size-survey-test"
  "size-report-test"
  "ota-slot-check-test"
  # LAN OTA rules, boot self test and core dump readback (#40)
  "ota-transfer-test"
  "boot-self-test-test"
  "coredump-read-test"
  "compile-dialect-test"
  "repro-paths-test"
  "stack-budget-test"
  "board-conditionals-test"
  "retired-names-test"
  # Local signing key guard and sign task, throwaway keys only
  "secrets-check-test"
  "release-sign-test"
  "release-workflow-test"
  "telemetry-contract-test --sanitize"
  "waveshare-contract-test --sanitize"
  "column-semantics-test"
  "board-template-check-test"
  "published-templates-test"
  "plugin-contract-test"
  "sensor-module-test"
  "pinned-uris-test"
  # Sensor drivers on fake buses
  "pms-frame-test"
  "ltr553-test"
  "max17048-test"
  # Battery health states and thresholds
  "battery-health-test"
  # Uploader signer, STS, policy, ledger and session on fakes (#81)
  "upload-test"
  # Archive, transport and radio policy fixtures
  "archive-sync-test"
  "control-sync-test"
  "disconnect-test"
  "frame-ring-test"
  "ble-policy-test"
  "ble-advert-test"
  "ble-sync-test"
  "wifi-link-test"
  # Settings, time and location
  "config-test"
  "network-time-test"
  "location-test"
  "gnss-test"
  # Logger lifecycle, storage and persistence
  "debug-log-test"
  "coredump-report-test"
  "logger-work-queue-test"
  "logger-watchdog-test"
  "logger-bench-stall-test"
  "logger-codec-store-test"
  "logger-status-test"
  "logger-rotation-test"
  "logger-footer-test"
  "logger-directory-test"
  "logger-list-since-test"
  "logger-archive-reset-test"
  "logger-provision-test"
  "logger-live-test"
  "logger-power-test"
  "storage-governor-test"
  "crc-cache-test"
  "sd-transfer-buffer-test"
  # Offline backup publication with a fake esptool
  "backup-test"
  # Board registry, board.json failure modes and board list drift
  "boards-test"
  # Serial capture send-on-cue with a fake port
  "capture-serial-test"
)

# Every *-test task in pixi.toml runs here unless it needs the ESP-IDF SDK.
# Fail before running anything if a new test task was added to pixi.toml but
# not to this list, so CI cannot silently skip it.
sdk_tasks=" connectivity-build-test logger-build-test "
listed=" ${tasks[*]%% *} "
unlisted=()
while read -r name; do
  if [[ "${sdk_tasks}" != *" ${name} "* && "${listed}" != *" ${name} "* ]]; then
    unlisted+=("${name}")
  fi
done < <(sed -n 's/^\([a-z0-9-]*-test\) = .*/\1/p' "$(dirname "$0")/../pixi.toml")
if [[ ${#unlisted[@]} -gt 0 ]]; then
  printf '!!! pixi test tasks missing from tools/gate.sh, %s\n' "${unlisted[*]}" >&2
  exit 1
fi

task_timeout=${OS_GATE_TASK_TIMEOUT:-300}
group=""
# The task group is not the terminal's foreground group, so Ctrl-C and a CI
# cancel reach only this script. Pass them on to the running task.
stop() {
  if [[ -n "${group}" ]]; then
    kill -TERM -- "-${group}" 2>/dev/null || true
  fi
  printf '\n!!! gate interrupted\n' >&2
  exit 130
}
trap stop INT TERM
passed=()
failed=()
started=${SECONDS}
for entry in "${tasks[@]}"; do
  read -r -a task <<<"${entry}"
  printf '\n==> pixi run %s\n' "${entry}"
  task_start=${SECONDS}
  # GNU timeout leads its own process group, so its PID is the group ID.
  timeout --kill-after=10 "${task_timeout}" pixi run "${task[@]}" &
  group=$!
  wait "${group}"
  status=$?
  # timeout exits as soon as pixi does. A grandchild forked while SIGTERM was
  # in flight would outlive it, so sweep the whole group before moving on.
  kill -KILL -- "-${group}" 2>/dev/null || true
  if [[ ${status} -eq 0 ]]; then
    passed+=("${entry} ($((SECONDS - task_start))s)")
  else
    # GNU timeout exits 124 after SIGTERM and 137 after the follow-up SIGKILL.
    if [[ ${status} -eq 124 || ${status} -eq 137 ]]; then
      failed+=("${entry} (TIMEOUT after ${task_timeout}s)")
    else
      failed+=("${entry} (exit ${status})")
    fi
    printf '\n!!! FAILED: pixi run %s, %s\n' "${entry}" "${failed[${#failed[@]}-1]}" >&2
    if [[ ${keep_going} -eq 0 ]]; then
      break
    fi
  fi
done

printf '\n==> gate summary, %d passed, %d failed, %ds\n' \
  "${#passed[@]}" "${#failed[@]}" "$((SECONDS - started))"
for entry in ${passed[@]+"${passed[@]}"}; do printf '  ok    %s\n' "${entry}"; done
for entry in ${failed[@]+"${failed[@]}"}; do printf '  FAIL  %s\n' "${entry}"; done
if [[ ${#failed[@]} -gt 0 ]]; then
  exit 1
fi
