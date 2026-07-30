#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="$(mktemp -d "${TMPDIR:-/tmp}/tbot-lesson-jpeg-reuse.XXXXXX")"
trap 'rm -rf "${BUILD_DIR}"' EXIT
CC="${CC:-clang}"
CXX="${CXX:-clang++}"
SANITIZERS=(-fsanitize=address,undefined -fno-omit-frame-pointer)
JPEG_COMPONENT_ROOT="${ROOT}/managed_components/espressif__esp_jpeg"
if [[ ! -d "${JPEG_COMPONENT_ROOT}" ]]; then
  JPEG_COMPONENT_ROOT="${ROOT}/../../managed_components/espressif__esp_jpeg"
fi
touch "${BUILD_DIR}/sdkconfig.h"
INCLUDES=(
  -I"${BUILD_DIR}"
  -I"${ROOT}/tests/native_stubs_jpeg"
  -I"${ROOT}/main/display/lvgl_display/jpg"
  -I"${JPEG_COMPONENT_ROOT}/include"
  -I"${JPEG_COMPONENT_ROOT}/tjpgd"
)
COMPAT=(-include stdint.h -include stdbool.h -include assert.h -include stdlib.h -include esp_heap_caps.h)
JPEG_CONFIG=(
  -DCONFIG_JD_SZBUF=512
  -DCONFIG_JD_FORMAT=1
  -DCONFIG_JD_FASTDECODE=1
  -DCONFIG_JD_USE_SCALE=1
  -DCONFIG_JD_TBLCLIP=1
  -DCONFIG_JD_DEFAULT_HUFFMAN=0
)
ALLOC_INTERCEPT=(
  -Dmalloc=tbot_test_malloc
  -Dcalloc=tbot_test_calloc
  -Drealloc=tbot_test_realloc
  -Dfree=tbot_test_free
)
"${CC}" -std=c11 -O0 -g "${SANITIZERS[@]}" "${INCLUDES[@]}" \
  "${ALLOC_INTERCEPT[@]}" -c "${ROOT}/main/display/lvgl_display/jpg/jpeg_to_image.c" \
  -o "${BUILD_DIR}/jpeg_to_image.o"
"${CC}" -std=c11 -O0 -g "${SANITIZERS[@]}" -Wno-incompatible-function-pointer-types \
  "${INCLUDES[@]}" "${COMPAT[@]}" "${JPEG_CONFIG[@]}" "${ALLOC_INTERCEPT[@]}" \
  -c "${JPEG_COMPONENT_ROOT}/jpeg_decoder.c" \
  -o "${BUILD_DIR}/jpeg_decoder.o"
"${CC}" -std=c11 -O0 -g "${SANITIZERS[@]}" "${INCLUDES[@]}" "${COMPAT[@]}" "${JPEG_CONFIG[@]}" \
  "${ALLOC_INTERCEPT[@]}" -c "${JPEG_COMPONENT_ROOT}/tjpgd/tjpgd.c" -o "${BUILD_DIR}/tjpgd.o"
"${CC}" -std=c11 -O0 -g "${SANITIZERS[@]}" "${ALLOC_INTERCEPT[@]}" \
  -c "${ROOT}/tests/native/lesson_jpeg_allocation_probe.c" -o "${BUILD_DIR}/allocation_probe.o"
"${CXX}" -std=c++17 -O0 -g "${SANITIZERS[@]}" "${INCLUDES[@]}" \
  "${ROOT}/tests/native/lesson_jpeg_reuse_test.cc" "${BUILD_DIR}/jpeg_to_image.o" \
  "${BUILD_DIR}/jpeg_decoder.o" "${BUILD_DIR}/tjpgd.o" "${BUILD_DIR}/allocation_probe.o" \
  -o "${BUILD_DIR}/lesson_jpeg_reuse_test"
"${BUILD_DIR}/lesson_jpeg_reuse_test" "${JPEG_COMPONENT_ROOT}/test_apps/main/logo.jpg"
