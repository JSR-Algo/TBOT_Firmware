#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="$(mktemp -d "${TMPDIR:-/tmp}/tbot-jpeg-abi.XXXXXX")"
trap 'rm -rf "${BUILD_DIR}"' EXIT
JPEG="${ROOT}/managed_components/espressif__esp_jpeg"
python3 "${ROOT}/scripts/prepare_host_jpeg_decoder.py" "${JPEG}/jpeg_decoder.c" "${BUILD_DIR}/jpeg_decoder.c"
"${CC:-clang}" -std=c11 -O0 -g -Werror=incompatible-function-pointer-types \
    -fsanitize=address,undefined -fno-omit-frame-pointer \
    -I"${BUILD_DIR}" -I"${ROOT}/tests/native_stubs_jpeg" -I"${JPEG}/include" -I"${JPEG}/tjpgd" \
    -include stdint.h -include stdbool.h -include assert.h -include stdlib.h -include esp_heap_caps.h \
    "${ROOT}/tests/native/jpeg_callback_abi_test.c" "${JPEG}/tjpgd/tjpgd.c" -o "${BUILD_DIR}/test"
"${BUILD_DIR}/test"
echo 'JPEG callback ABI and 12 behavior assertions passed'
