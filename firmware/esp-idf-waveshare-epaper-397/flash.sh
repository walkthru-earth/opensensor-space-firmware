#!/usr/bin/env bash
set -euo pipefail
trial_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
exec "${trial_dir}/../../tools/flash-idf-board.sh" epaper397 "$@"
