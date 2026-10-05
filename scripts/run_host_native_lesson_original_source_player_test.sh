#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
: "${TMPDIR:?owned TMPDIR required}"
: "${CJSON_DIR:?qualified cJSON required}"
: "${ORIGINAL_SOURCE_CONTRACT_VECTORS:?canonical backend scene vectors required}"
BUILD_DIR="$(mktemp -d "${TMPDIR}/original-source-player.XXXXXX")"
trap 'rm -rf "${BUILD_DIR}"' EXIT
SAN=(-g -fsanitize=address,undefined -fno-omit-frame-pointer)
# Vendored cJSON uses sprintf, deprecated by the macOS SDK; our sources keep -Werror.
"${CC:-clang}" -std=c99 "${SAN[@]}" -Wall -Wextra -Wno-deprecated-declarations -I"${CJSON_DIR}" -c "${CJSON_DIR}/cJSON.c" -o "${BUILD_DIR}/cJSON.o"
# The player only uses the session's frame/status types; no decoder is linked.
"${CXX:-clang++}" -std=c++17 -ffp-contract=off "${SAN[@]}" -Wall -Wextra -Werror ${CXXFLAGS:-} \
  -I"${ROOT}/main" -I"${CJSON_DIR}" \
  "${ROOT}/main/lesson_original_source_player.cc" \
  "${ROOT}/main/lesson_original_source_scene_controller.cc" \
  "${ROOT}/main/lesson_original_source_contract.cc" \
  "${ROOT}/main/lesson_original_source_compositor.cc" \
  "${ROOT}/main/lesson_tvideo_frame_state.cc" \
  "${ROOT}/main/lesson_tvideo_painter.cc" \
  "${ROOT}/main/lesson_tvideo_raster_canvas.cc" \
  "${ROOT}/main/lesson_asset_cache_evict.cc" \
  "${ROOT}/main/lesson_asset_retained_selection.cc" \
  "${ROOT}/main/lesson_asset_storage_coordinator.cc" \
  "${ROOT}/main/sd_fat_session_guard.cc" \
  "${ROOT}/tests/native/lesson_original_source_player_host_test.cc" \
  "${BUILD_DIR}/cJSON.o" -o "${BUILD_DIR}/original-source-player-test"
UBSAN_OPTIONS=halt_on_error=1 "${BUILD_DIR}/original-source-player-test" "${ORIGINAL_SOURCE_CONTRACT_VECTORS}"
