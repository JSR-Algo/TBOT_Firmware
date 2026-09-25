#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
: "${TMPDIR:?owned TMPDIR required}"
: "${CJSON_DIR:?qualified cJSON required}"
: "${RETAINED_CONTRACT_VECTORS:?canonical shared vectors required}"
BUILD_DIR="$(mktemp -d "${TMPDIR}/retained-parser.XXXXXX")"
"${CC:-clang}" -std=c99 -Wall -Wextra -Werror -I"${CJSON_DIR}" -c "${CJSON_DIR}/cJSON.c" -o "${BUILD_DIR}/cJSON.o"
"${CXX:-clang++}" -std=c++17 -pthread -Wall -Wextra -Werror ${CXXFLAGS:-} \
  -I"${ROOT}/main" -I"${CJSON_DIR}" \
  "${ROOT}/main/lesson_asset_retained_parser.cc" \
  "${ROOT}/main/lesson_asset_retained_selection.cc" \
  "${ROOT}/main/lesson_asset_cache_evict.cc" \
  "${ROOT}/main/lesson_asset_storage_coordinator.cc" \
  "${ROOT}/main/sd_fat_session_guard.cc" \
  "${ROOT}/tests/native/lesson_asset_retained_parser_host_test.cc" \
  "${BUILD_DIR}/cJSON.o" -o "${BUILD_DIR}/retained-parser-test"
"${BUILD_DIR}/retained-parser-test" "${RETAINED_CONTRACT_VECTORS}" "${ROOT}/tests/fixtures/retained-assignment-device.v1.vectors.json"
