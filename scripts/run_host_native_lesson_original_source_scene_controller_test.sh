#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
: "${TMPDIR:?owned TMPDIR required}"
: "${CJSON_DIR:?qualified cJSON required}"
: "${ORIGINAL_SOURCE_CONTRACT_VECTORS:?canonical backend scene vectors required}"
: "${TVIDEO_FRAME_STATE_VECTORS:?canonical backend frame-state vectors required}"
BUILD_DIR="$(mktemp -d "${TMPDIR}/original-source-controller.XXXXXX")"
trap 'rm -rf "${BUILD_DIR}"' EXIT
SAN=(-g -fsanitize=address,undefined -fno-omit-frame-pointer)
# Vendored cJSON uses sprintf, deprecated by the macOS SDK; our sources keep -Werror.
"${CC:-clang}" -std=c99 "${SAN[@]}" -Wall -Wextra -Wno-deprecated-declarations -I"${CJSON_DIR}" -c "${CJSON_DIR}/cJSON.c" -o "${BUILD_DIR}/cJSON.o"
"${CXX:-clang++}" -std=c++17 -ffp-contract=off "${SAN[@]}" -Wall -Wextra -Werror ${CXXFLAGS:-} \
  -I"${ROOT}/main" -I"${CJSON_DIR}" \
  "${ROOT}/main/lesson_original_source_scene_controller.cc" \
  "${ROOT}/main/lesson_original_source_contract.cc" \
  "${ROOT}/main/lesson_tvideo_frame_state.cc" \
  "${ROOT}/main/lesson_asset_cache_evict.cc" \
  "${ROOT}/main/lesson_asset_retained_selection.cc" \
  "${ROOT}/main/lesson_asset_storage_coordinator.cc" \
  "${ROOT}/main/sd_fat_session_guard.cc" \
  "${ROOT}/tests/native/lesson_original_source_scene_controller_host_test.cc" \
  "${BUILD_DIR}/cJSON.o" -o "${BUILD_DIR}/original-source-controller-test"
UBSAN_OPTIONS=halt_on_error=1 "${BUILD_DIR}/original-source-controller-test" \
  "${ORIGINAL_SOURCE_CONTRACT_VECTORS}" "${TVIDEO_FRAME_STATE_VECTORS}"
