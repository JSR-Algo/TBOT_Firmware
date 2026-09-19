#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
: "${TMPDIR:?owned TMPDIR required}"
BUILD_DIR="$(mktemp -d "${TMPDIR}/tbot-lesson-storage-hil-controller.XXXXXX")"
# Intentional word splitting lets callers append normal compiler flags.
# shellcheck disable=SC2086
export TBOT_RETAINED_TEST_STATE_PATH="${BUILD_DIR}/selection.record"
"${CXX:-clang++}" -std=c++17 -pthread -Wall -Wextra -Werror ${CXXFLAGS:-} \
  -DTBOT_LESSON_STORAGE_HIL_CONTROLLER_TESTING \
  -I"${ROOT}/main" \
  "${ROOT}/main/lesson_asset_cache_evict.cc" \
  "${ROOT}/main/lesson_asset_retained_selection.cc" \
  "${ROOT}/main/lesson_asset_storage_coordinator.cc" \
  "${ROOT}/main/sd_fat_session_guard.cc" \
  "${ROOT}/main/lesson_storage_hil_controller.cc" \
  "${ROOT}/tests/native/lesson_storage_hil_controller_host_test.cc" \
  -o "${BUILD_DIR}/lesson_storage_hil_controller_host_test"
"${BUILD_DIR}/lesson_storage_hil_controller_host_test"
