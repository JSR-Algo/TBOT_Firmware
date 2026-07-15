#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="$(mktemp -d "${TMPDIR:-/tmp}/tbot-lesson-cache-evict.XXXXXX")"
trap 'rm -rf "${BUILD_DIR}" /tmp/tbot-lesson-asset-cache-evict-host' EXIT
"${CXX:-clang++}" -std=c++17 -Wall -Wextra -Werror \
  -DTBOT_LESSON_ASSET_ROOT='"/tmp/tbot-lesson-asset-cache-evict-host"' \
  -I"${ROOT}/main" \
  "${ROOT}/main/lesson_asset_cache_evict.cc" \
  "${ROOT}/tests/native/lesson_asset_cache_evict_host_test.cc" \
  -o "${BUILD_DIR}/lesson_asset_cache_evict_host_test"
"${BUILD_DIR}/lesson_asset_cache_evict_host_test"
