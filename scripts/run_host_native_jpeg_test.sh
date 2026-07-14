#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="$(mktemp -d "${TMPDIR:-/tmp}/tbot-host-native-jpeg.XXXXXX")"
trap 'rm -rf "${BUILD_DIR}"' EXIT
CC="${CC:-clang}"
CXX="${CXX:-clang++}"

cd "${ROOT}"

COMMON_INCLUDES=(
    -Itests/native_stubs_jpeg
    -Imain/display/lvgl_display/jpg
    -Imanaged_components/espressif__esp_jpeg/include
    -Imanaged_components/espressif__esp_jpeg/tjpgd
)
HOST_IDF_COMPAT=(
    -include stdint.h
    -include stdbool.h
    -include assert.h
    -include stdlib.h
    -include esp_heap_caps.h
)

"${CC}" -std=c11 -O0 -g "${COMMON_INCLUDES[@]}" \
    -c main/display/lvgl_display/jpg/jpeg_to_image.c -o "${BUILD_DIR}/jpeg_to_image.o"
"${CC}" -std=c11 -O0 -g -Wno-incompatible-function-pointer-types "${COMMON_INCLUDES[@]}" "${HOST_IDF_COMPAT[@]}" \
    -c managed_components/espressif__esp_jpeg/jpeg_decoder.c -o "${BUILD_DIR}/jpeg_decoder.o"
"${CC}" -std=c11 -O0 -g "${COMMON_INCLUDES[@]}" "${HOST_IDF_COMPAT[@]}" \
    -c managed_components/espressif__esp_jpeg/tjpgd/tjpgd.c -o "${BUILD_DIR}/tjpgd.o"
"${CXX}" -std=c++17 -O0 -g "${COMMON_INCLUDES[@]}" \
    tests/native/jpeg_to_image_host_test.cc \
    "${BUILD_DIR}/jpeg_to_image.o" "${BUILD_DIR}/jpeg_decoder.o" "${BUILD_DIR}/tjpgd.o" \
    -o "${BUILD_DIR}/jpeg_to_image_host_test"

"${BUILD_DIR}/jpeg_to_image_host_test" \
    managed_components/espressif__esp_jpeg/test_apps/main/logo.jpg
