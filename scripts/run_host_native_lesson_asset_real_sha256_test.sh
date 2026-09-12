#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="$(mktemp -d "${TMPDIR:-/tmp}/tbot-real-sha256.XXXXXX")"
trap 'rm -rf "${BUILD_DIR}"' EXIT
IDF_ROOT="${IDF_PATH:-${HOME}/esp/esp-idf}"
MBEDTLS_ROOT="${IDF_ROOT}/components/mbedtls/mbedtls"
if [[ ! -f "${MBEDTLS_ROOT}/library/sha256.c" ]]; then
  echo "missing ESP-IDF mbedTLS source; set IDF_PATH" >&2
  exit 127
fi
printf '#define MBEDTLS_SHA256_C\n' > "${BUILD_DIR}/sha256_config.h"
FLAGS=(-fsanitize=address,undefined -fno-omit-frame-pointer
  -DMBEDTLS_CONFIG_FILE='"sha256_config.h"'
  -I"${BUILD_DIR}" -I"${MBEDTLS_ROOT}/include" -I"${MBEDTLS_ROOT}/library")
for source in sha256 platform_util; do
  "${CC:-clang}" -std=c99 "${FLAGS[@]}" \
    -c "${MBEDTLS_ROOT}/library/${source}.c" -o "${BUILD_DIR}/${source}.o"
done
"${CXX:-clang++}" -std=c++17 -Wall -Wextra -Werror "${FLAGS[@]}" \
  -DTBOT_REAL_SHA256_TEST_ROOT=\""${BUILD_DIR}/root"\" -I"${ROOT}/main" \
  "${ROOT}/main/lesson_asset_download_staging.cc" \
  "${ROOT}/tests/native/lesson_asset_real_sha256_host_test.cc" \
  "${BUILD_DIR}/sha256.o" "${BUILD_DIR}/platform_util.o" \
  -o "${BUILD_DIR}/real_sha256"
"${BUILD_DIR}/real_sha256"
