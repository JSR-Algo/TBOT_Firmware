#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="$(mktemp -d "${TMPDIR:-/tmp}/tbot-lesson-sync-path.XXXXXX")"
trap 'rm -rf "${BUILD_DIR}"' EXIT
"${CXX:-clang++}" -std=c++17 -Wall -Wextra -Werror \
  -I"${ROOT}/main" \
  "${ROOT}/main/lesson_asset_cache_evict.cc" \
  "${ROOT}/main/lesson_asset_sync_path_policy.cc" \
  "${ROOT}/tests/native/lesson_asset_sync_path_host_test.cc" \
  -o "${BUILD_DIR}/lesson_asset_sync_path_host_test"
"${BUILD_DIR}/lesson_asset_sync_path_host_test"
