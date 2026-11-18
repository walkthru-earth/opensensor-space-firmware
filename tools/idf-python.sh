#!/usr/bin/env bash
# The hashed lock of the ESP-IDF Python build tools (#176). The lock, its paths
# and the venv install live in tools/idf-env.sh and tools/idf_python_lock.py.
#
# Usage: tools/idf-python.sh check|lock
#   check  Fail when the IDF venv differs from the committed lock. It never
#          repairs the venv. CI and the release build run it after the build.
#   lock   Resolve the set with the pinned SDK installer in a throwaway venv
#          and IDF_TOOLS_PATH, look up the file hashes on PyPI and rewrite
#          the lock. Needs the SDK (pixi run idf-setup) and network. The next
#          build rebuilds the venv from the new lock.
set -euo pipefail
repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
source "${repo_root}/tools/idf-dependencies.lock"
# Sets idf_path, IDF_PYTHON_ENV_PATH and IDF_PYTHON_LOCK.
source "${repo_root}/tools/idf-env.sh"
case "${1:-}" in
  check)
    if [[ ! -x "${IDF_PYTHON_ENV_PATH}/bin/python" ]]; then
      printf 'No IDF Python venv at %s. Run pixi run idf-setup.\n' "${IDF_PYTHON_ENV_PATH}" >&2
      exit 2
    fi
    idf_python_env_check
    ;;
  lock)
    if [[ ! -f "${idf_path}/tools/idf_tools.py" ]] || [[ "$(git -C "${idf_path}" rev-parse HEAD)" != "${ESP_IDF_COMMIT}" ]]; then
      printf 'Pinned ESP-IDF v%s missing. Run pixi run idf-setup.\n' "${ESP_IDF_VERSION}" >&2
      exit 2
    fi
    python "${repo_root}/tools/idf_python_lock.py" write --lock "${IDF_PYTHON_LOCK}" \
      --idf-path "${idf_path}" --idf-version "${idf_major}.${idf_minor}" --idf-commit "${ESP_IDF_COMMIT}"
    ;;
  *)
    printf 'usage: %s check|lock\n' "$0" >&2
    exit 2
    ;;
esac
