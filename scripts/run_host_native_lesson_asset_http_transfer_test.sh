#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="$(mktemp -d "${TMPDIR:-/tmp}/tbot-lesson-transfer.XXXXXX")"
trap 'rm -rf "${BUILD_DIR}"' EXIT
CXX_BIN="${CXX:-clang++}"

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
  "${ROOT}/main/lesson_storage_hil_controller.cc" \
  "${ROOT}/main/lesson_storage_hil_hooks.cc" \
  "${ROOT}/tests/native/lesson_asset_http_transfer_host_test.cc" \
  -o "${BUILD_DIR}/lesson_asset_http_transfer_host_test"
"${BUILD_DIR}/lesson_asset_http_transfer_host_test"
