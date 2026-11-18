#!/usr/bin/env bash
# Serial only core dump readback, erase only with --erase (#40).
# See tools/coredump_read.py.
set -euo pipefail
repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
source "${repo_root}/tools/idf-dependencies.lock"
# Sets toolchain_root, idf_path, IDF_TOOLS_PATH and IDF_PYTHON_ENV_PATH.
source "${repo_root}/tools/idf-env.sh"
export IDF_PATH="${idf_path}"
if [[ ! -f "${IDF_PATH}/tools/idf.py" ]] || [[ "$(git -C "${IDF_PATH}" rev-parse HEAD)" != "${ESP_IDF_COMMIT}" ]]; then
  printf 'Pinned ESP-IDF v%s missing. Run pixi run idf-setup.\n' "${ESP_IDF_VERSION}" >&2
  exit 2
fi
ensure_idf_python_env
# esp_coredump comes from the hashed lock in tools/idf-python (#176), its GDB
# and the ROM ELFs from the pinned SDK installer.
source "${IDF_PATH}/export.sh" >/dev/null
exec python "${repo_root}/tools/coredump_read.py" "$@"
