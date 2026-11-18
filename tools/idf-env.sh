# shellcheck shell=bash
# ESP-IDF locations shared by tools/setup-idf.sh and tools/idf.sh. Source it
# after setting repo_root and sourcing tools/idf-dependencies.lock.
#
# Shared by every checkout and worktree (large downloads)
#   SDK clone   $OS_TOOLCHAIN_ROOT, default
#               ${XDG_CACHE_HOME:-~/.cache}/<pixi workspace name>/toolchains,
#               a sibling of pixi's own cache, never inside it
#   SDK tools   $OS_IDF_TOOLS_PATH, default ~/.espressif (compilers, OpenOCD)
# Owned by one checkout (bound to that checkout's Pixi Python)
#   IDF venv    $OS_IDF_PYTHON_ENV_PATH, default
#               <workspace>/.pixi/idf-python-env/idf<X.Y>_py<X.Y>_env
#
# A venv links to the interpreter that created it. One shared venv under
# ~/.espressif broke every checkout once the checkout owning its Python
# moved. A venv per checkout breaks only with its own checkout, and
# ensure_idf_python_env rebuilds it then.
toolchain_root="${OS_TOOLCHAIN_ROOT:-${XDG_CACHE_HOME:-${HOME}/.cache}/${PIXI_WORKSPACE_NAME:-opensensor-space-firmware}/toolchains}"
idf_path="${OS_IDF_PATH:-${toolchain_root}/esp-idf-v${ESP_IDF_VERSION}}"
export IDF_TOOLS_PATH="${OS_IDF_TOOLS_PATH:-${HOME}/.espressif}"
# PIXI_WORKSPACE_ROOT owns the python on PATH, also when a script of another
# worktree runs under it (repro-paths). Outside pixi the script's own checkout.
# The leaf follows idf_tools.py's own idf<X.Y>_py<X.Y>_env name, so a new SDK
# or Python gets a fresh venv, and idf_tools.py accepts the path as a venv.
IFS=. read -r idf_major idf_minor _ <<<"${ESP_IDF_VERSION}"
idf_py_version="$(python -c 'import sys; print("%d.%d" % sys.version_info[:2])')"
export IDF_PYTHON_ENV_PATH="${OS_IDF_PYTHON_ENV_PATH:-${PIXI_WORKSPACE_ROOT:-${repo_root}}/.pixi/idf-python-env/idf${idf_major}.${idf_minor}_py${idf_py_version}_env}"

# The committed hashed lock of the venv's packages (#176), one per ESP-IDF and
# Python minor version. tools/idf_python_lock.py writes and checks it.
export IDF_PYTHON_LOCK="${repo_root}/tools/idf-python/idf${idf_major}.${idf_minor}_py${idf_py_version}.txt"
# The SDK checks the venv against its downloaded constraints file on every
# idf.py run and CMake configure. That file moves and is no longer fetched,
# the lock above replaces it. The SDK still checks that every package its
# requirements name is installed with its dependencies.
export IDF_PYTHON_CHECK_CONSTRAINTS=no

# Exit 0 when the venv holds exactly the locked set, 1 on drift, 2 when the
# check cannot run. Runs the venv's own python, so it sees the venv packages.
idf_python_env_check() {
  "${IDF_PYTHON_ENV_PATH}/bin/python" -I "${repo_root}/tools/idf_python_lock.py" \
    check --lock "${IDF_PYTHON_LOCK}" "$@"
}

# Create the IDF venv from the committed lock when it is missing, when its
# python is a dangling link (the checkout or its Pixi env moved), when the
# pyvenv.cfg home is not the bin folder of the python on PATH (a copied
# checkout, or a venv still bound to another checkout's live Pixi env), or when
# its packages differ from the lock (the lock changed, or a package moved).
# tools/idf.sh never runs the installer, so the repair lives here. pip installs
# only the locked files, each checked against its SHA-256, and resolves
# nothing. The SDK installer and its constraints download are not used.
ensure_idf_python_env() {
  local venv_python="${IDF_PYTHON_ENV_PATH}/bin/python"
  local cfg="${IDF_PYTHON_ENV_PATH}/pyvenv.cfg"
  local want_home venv_home="" reason="is broken"
  if [[ ! -f "${IDF_PYTHON_LOCK}" ]]; then
    printf 'No IDF Python lock for ESP-IDF %s.%s on Python %s, %s is missing.\n' \
      "${idf_major}" "${idf_minor}" "${idf_py_version}" "${IDF_PYTHON_LOCK}" >&2
    printf 'Run pixi run idf-python-lock, see docs/shared/development.md.\n' >&2
    return 2
  fi
  want_home="$(dirname "$(command -v python)")"
  if [[ -f "${cfg}" ]]; then
    venv_home="$(sed -n 's/^home[[:space:]]*=[[:space:]]*//p' "${cfg}" | head -n 1)"
  fi
  if [[ -x "${venv_python}" ]] && [[ "${venv_home}" == "${want_home}" ]] \
    && "${venv_python}" -c '' >/dev/null 2>&1; then
    if idf_python_env_check --quiet; then
      return 0
    fi
    reason="differs from the lock"
  fi
  if [[ -L "${IDF_PYTHON_ENV_PATH}" ]] || [[ -n "$(ls -A "${IDF_PYTHON_ENV_PATH}" 2>/dev/null)" ]]; then
    # Remove only what is recognisably a venv, never an arbitrary override.
    if [[ ! -f "${IDF_PYTHON_ENV_PATH}/pyvenv.cfg" ]]; then
      printf 'Refusing to replace %s, it is not a Python venv\n' "${IDF_PYTHON_ENV_PATH}" >&2
      return 2
    fi
    printf 'IDF Python venv %s, rebuilding it: %s\n' "${reason}" "${IDF_PYTHON_ENV_PATH}" >&2
    rm -rf "${IDF_PYTHON_ENV_PATH}"
  else
    printf 'Creating the IDF Python venv: %s\n' "${IDF_PYTHON_ENV_PATH}" >&2
  fi
  python -m venv "${IDF_PYTHON_ENV_PATH}"
  # idf_tools.py reads this file to match the venv to the SDK version.
  printf '%s.%s' "${idf_major}" "${idf_minor}" >"${IDF_PYTHON_ENV_PATH}/idf_version.txt"
  "${venv_python}" -I "${repo_root}/tools/idf_python_lock.py" install --lock "${IDF_PYTHON_LOCK}"
}
