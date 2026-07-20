#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="$(mktemp -d "${TMPDIR:-/tmp}/tbot-lesson-storage-coordinator.XXXXXX")"
trap 'rm -rf "${BUILD_DIR}"' EXIT
"${CXX:-clang++}" -std=c++17 -pthread -Wall -Wextra -Werror \
  -DTBOT_LESSON_ASSET_COORDINATOR_TESTING \
  -I"${ROOT}/main" \
  "${ROOT}/main/lesson_asset_storage_coordinator.cc" \
  "${ROOT}/tests/native/lesson_asset_storage_coordinator_host_test.cc" \
  -o "${BUILD_DIR}/lesson_asset_storage_coordinator_host_test"
"${BUILD_DIR}/lesson_asset_storage_coordinator_host_test"
