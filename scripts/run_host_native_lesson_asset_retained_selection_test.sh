#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
: "${TMPDIR:?owned TMPDIR required}"
BUILD_DIR="$(mktemp -d "${TMPDIR}/retained-selection.XXXXXX")"
export TBOT_RETAINED_TEST_STATE_PATH="${BUILD_DIR}/selection.record"
export TBOT_RETAINED_TEST_ASSET_ROOT="${BUILD_DIR}/assets"
"${CXX:-clang++}" -std=c++17 -pthread -Wall -Wextra -Werror ${CXXFLAGS:-} \
  -DTBOT_LESSON_ASSET_CACHE_EVICT_TESTING \
  -DTBOT_LESSON_ASSET_ROOT="\"${TBOT_RETAINED_TEST_ASSET_ROOT}\"" \
  -I"${ROOT}/main" \
  "${ROOT}/main/lesson_asset_retained_selection.cc" \
  "${ROOT}/main/lesson_asset_cache_evict.cc" \
  "${ROOT}/main/lesson_asset_pack_activation.cc" \
  "${ROOT}/main/lesson_asset_storage_coordinator.cc" \
  "${ROOT}/main/sd_fat_session_guard.cc" \
  "${ROOT}/tests/native/lesson_asset_retained_selection_host_test.cc" \
  -o "${BUILD_DIR}/retained-selection-test"
"${BUILD_DIR}/retained-selection-test" exercise
"${BUILD_DIR}/retained-selection-test" restart
