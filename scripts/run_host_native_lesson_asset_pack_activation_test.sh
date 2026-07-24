#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="$(mktemp -d "${TMPDIR:-/tmp}/tbot-lesson-pack-activation.XXXXXX")"
trap 'rm -rf "${BUILD_DIR}" /tmp/tbot-lesson-asset-pack-activation-host' EXIT
"${CXX:-clang++}" -std=c++17 -pthread -Wall -Wextra -Werror ${CXXFLAGS:-} \
  -DTBOT_LESSON_ASSET_ROOT='"/tmp/tbot-lesson-asset-pack-activation-host"' \
  -DTBOT_LESSON_ASSET_CACHE_EVICT_TESTING \
  -I"${ROOT}/main" \
  "${ROOT}/main/lesson_asset_pack_activation.cc" \
  "${ROOT}/main/lesson_asset_cache_evict.cc" \
  "${ROOT}/main/lesson_asset_storage_coordinator.cc" \
  "${ROOT}/main/sd_fat_session_guard.cc" \
  "${ROOT}/tests/native/lesson_asset_pack_activation_host_test.cc" \
  -o "${BUILD_DIR}/lesson_asset_pack_activation_host_test"
"${BUILD_DIR}/lesson_asset_pack_activation_host_test"
