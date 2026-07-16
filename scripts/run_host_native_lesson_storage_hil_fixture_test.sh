#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="$(mktemp -d "${TMPDIR:-/tmp}/tbot-lesson-storage-hil-fixture.XXXXXX")"
STORAGE_ROOT="${BUILD_DIR}/lesson-assets"
trap 'rm -rf "${BUILD_DIR}"' EXIT
# Intentional word splitting lets callers append normal compiler flags.
# shellcheck disable=SC2086
"${CXX:-clang++}" -std=c++17 -pthread -Wall -Wextra -Werror ${CXXFLAGS:-} \
  -DTBOT_LESSON_STORAGE_HIL_ROOT='"'"${STORAGE_ROOT}"'"' \
  -DTBOT_LESSON_STORAGE_HIL_FIXTURE_TESTING \
  -I"${ROOT}/tests/native_stubs_staging" \
  -I"${ROOT}/main" \
  "${ROOT}/main/lesson_asset_cache_evict.cc" \
  "${ROOT}/main/lesson_asset_storage_coordinator.cc" \
  "${ROOT}/main/lesson_storage_hil_fixture.cc" \
  "${ROOT}/tests/native/lesson_storage_hil_fixture_host_test.cc" \
  -o "${BUILD_DIR}/lesson_storage_hil_fixture_host_test"
"${BUILD_DIR}/lesson_storage_hil_fixture_host_test"
