#!/usr/bin/env bash
set -euo pipefail

repo_root="$(git rev-parse --show-toplevel)"
repro_dir="$repo_root/tests/lesson-production/repros"
export TBOT_REPRO_REPO_ROOT="$repo_root"

passed=0
for repro in "$repro_dir"/*.sh; do
  if [[ "$(basename "$repro")" == "t54-firmware.sh" ]]; then
    echo "lesson-production repro: t54-firmware.sh [SKIP_REGATE preserved]"
    continue
  fi
  echo "lesson-production repro: $(basename "$repro")"
  (cd "$repo_root" && bash "$repro")
  passed=$((passed + 1))
done

echo "lesson-production firmware repros passed: $passed; skip-regate=1"
