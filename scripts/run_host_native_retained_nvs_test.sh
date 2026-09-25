#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
: "${TMPDIR:?owned TMPDIR required}"
: "${CJSON_DIR:?qualified cJSON required}"
BUILD_DIR="$(mktemp -d "${TMPDIR}/retained-nvs.XXXXXX")"
"${CC:-clang}" -std=c99 -I"${CJSON_DIR}" -c "${CJSON_DIR}/cJSON.c" -o "${BUILD_DIR}/cJSON.o"
"${CXX:-clang++}" -std=c++17 -Wall -Wextra -Werror -fsanitize=address,undefined \
  -DESP_PLATFORM -I"${ROOT}/tests/native_stubs_retained" -I"${ROOT}/main" \
  -c "${ROOT}/main/lesson_asset_retained_selection.cc" -o "${BUILD_DIR}/selection.o"
"${CXX:-clang++}" -std=c++17 -pthread -Wall -Wextra -Werror -fsanitize=address,undefined \
  -I"${ROOT}/tests/native_stubs_retained" -I"${ROOT}/main" -I"${CJSON_DIR}" \
  "${ROOT}/tests/native/retained_nvs_host_test.cc" \
  "${ROOT}/main/lesson_asset_retained_parser.cc" \
  "${ROOT}/main/lesson_asset_cache_evict.cc" \
  "${ROOT}/main/lesson_asset_storage_coordinator.cc" \
  "${ROOT}/main/sd_fat_session_guard.cc" \
  "${BUILD_DIR}/selection.o" "${BUILD_DIR}/cJSON.o" -o "${BUILD_DIR}/retained-nvs-test"
for fault in open size read corrupt set commit; do
  "${BUILD_DIR}/retained-nvs-test" "${RETAINED_CONTRACT_VECTORS:?shared vectors required}" "${fault}"
done
