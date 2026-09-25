#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
: "${TMPDIR:?owned TMPDIR required}"
BUILD_DIR="$(mktemp -d "${TMPDIR}/tbot-sample-url.XXXXXX")"
export TBOT_RETAINED_TEST_STATE_PATH="${BUILD_DIR}/selection.record"
"${CXX:-clang++}" -std=c++17 -Wall -Wextra -Werror \
  -fsanitize=address,undefined -fno-omit-frame-pointer \
  -I"${ROOT}/main" \
  "${ROOT}/main/lesson_asset_cache_evict.cc" \
  "${ROOT}/main/lesson_asset_retained_selection.cc" \
  "${ROOT}/main/lesson_asset_storage_coordinator.cc" \
  "${ROOT}/main/sd_fat_session_guard.cc" \
  "${ROOT}/main/lesson_asset_sync_path_policy.cc" \
  "${ROOT}/main/lesson_asset_sample_url_policy.cc" \
  "${ROOT}/tests/native/lesson_asset_sample_url_policy_host_test.cc" \
  -o "${BUILD_DIR}/lesson_asset_sample_url_policy_host_test"
"${BUILD_DIR}/lesson_asset_sample_url_policy_host_test"
