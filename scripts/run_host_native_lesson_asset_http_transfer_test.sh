#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
: "${TMPDIR:?owned TMPDIR required}"
BUILD_DIR="$(mktemp -d "${TMPDIR}/tbot-lesson-transfer.XXXXXX")"
export TBOT_RETAINED_TEST_STATE_PATH="${BUILD_DIR}/selection.record"
CXX_BIN="${CXX:-clang++}"

"${CXX_BIN}" -std=c++17 -Wall -Wextra -Werror ${CXXFLAGS:-} \
  -I"${ROOT}/tests/native_stubs_transfer" \
  -I"${ROOT}/components/esp-ml307/include" \
  -I"${ROOT}/main" \
  -c "${ROOT}/main/lesson_asset_http_transfer.cc" \
  -o "${BUILD_DIR}/lesson_asset_http_transfer_config_off.o"

"${CXX_BIN}" -std=c++17 -pthread -Wall -Wextra -Werror ${CXXFLAGS:-} \
  -DTBOT_LESSON_ASSET_STAGING_TESTING=1 \
  -DTBOT_LESSON_STORAGE_HIL_HOOKS_TESTING=1 \
  -DTBOT_LESSON_STORAGE_HIL_CONTROLLER_TESTING=1 \
  -DTBOT_LESSON_ASSET_TRANSFER_TEST_ROOT=\"${BUILD_DIR}/root\" \
  -I"${ROOT}/tests/native_stubs_transfer" \
  -I"${ROOT}/tests/native_stubs_staging" \
  -I"${ROOT}/components/esp-ml307/include" \
  -I"${ROOT}/main" \
  "${ROOT}/main/lesson_asset_http_transfer.cc" \
  "${ROOT}/main/lesson_asset_download_staging.cc" \
  "${ROOT}/main/lesson_asset_cache_evict.cc" \
  "${ROOT}/main/lesson_asset_retained_selection.cc" \
  "${ROOT}/main/lesson_asset_storage_coordinator.cc" \
  "${ROOT}/main/sd_fat_session_guard.cc" \
  "${ROOT}/main/lesson_storage_hil_controller.cc" \
  "${ROOT}/main/lesson_storage_hil_hooks.cc" \
  "${ROOT}/tests/native/lesson_asset_http_transfer_host_test.cc" \
  -o "${BUILD_DIR}/lesson_asset_http_transfer_host_test"
"${BUILD_DIR}/lesson_asset_http_transfer_host_test"
