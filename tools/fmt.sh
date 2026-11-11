#!/usr/bin/env bash
# clang-format over every C and C++ source we own, for `pixi run fmt` and
# `pixi run fmt-check`. With --check nothing is rewritten and any difference
# fails. An empty or failed file listing fails too, so a broken glob or a
# missing rg can never read as a clean tree.
set -euo pipefail
repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${repo_root}"
case "${1:-}" in
  "") format_args=(-i) ;;
  --check) format_args=(--dry-run --Werror) ;;
  *) printf 'usage: %s [--check]\n' "$0" >&2; exit 2 ;;
esac
if [[ $# -gt 1 ]]; then
  printf 'usage: %s [--check]\n' "$0" >&2; exit 2
fi
list="$(mktemp)"
trap 'rm -f "${list}"' EXIT
# rg exits 1 when it lists no file and 2 on an error.
list_rc=0
rg --files -0 firmware tools -g '*.c' -g '*.h' -g '*.cpp' -g '*.hpp' -g '*.ino' \
  -g '!**/vendor/**' -g '!**/build/**' -g '!**/managed_components/**' >"${list}" || list_rc=$?
count="$(tr -cd '\0' <"${list}" | wc -c | tr -d ' ')"
if [[ "${list_rc}" -ne 0 || "${count}" -eq 0 ]]; then
  printf '!!! fmt found no sources to format (rg exit %s, %s files)\n' "${list_rc}" "${count}" >&2
  exit 1
fi
xargs -0 clang-format "${format_args[@]}" <"${list}"
