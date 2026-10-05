#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
: "${TMPDIR:?owned TMPDIR required}"
: "${CJSON_DIR:?qualified cJSON required}"
BUILD_DIR="$(mktemp -d "${TMPDIR}/tvideo-raster.XXXXXX")"
trap 'rm -rf "${BUILD_DIR}"' EXIT
SAN=(-g -fsanitize=address,undefined -fno-omit-frame-pointer)
"${CXX:-clang++}" -std=c++17 -ffp-contract=off "${SAN[@]}" -Wall -Wextra -Werror ${CXXFLAGS:-} \
  -I"${ROOT}/main" -I"${CJSON_DIR}" \
  "${ROOT}/main/lesson_tvideo_raster_canvas.cc" \
  "${ROOT}/main/lesson_original_source_compositor.cc" \
  "${ROOT}/tests/native/lesson_tvideo_raster_canvas_host_test.cc" \
  -o "${BUILD_DIR}/tvideo-raster-test"
UBSAN_OPTIONS=halt_on_error=1 "${BUILD_DIR}/tvideo-raster-test"
