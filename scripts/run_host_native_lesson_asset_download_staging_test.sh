#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="$(mktemp -d "${TMPDIR:-/tmp}/tbot-lesson-staging.XXXXXX")"
trap 'rm -rf "${BUILD_DIR}" /tmp/tbot-lesson-asset-staging-host' EXIT
CXX_BIN="${CXX:-clang++}"
IDF_ROOT="${IDF_PATH:-${HOME:+${HOME}/esp/esp-idf}}"
SANITIZER_FLAGS=()

if printf 'int main() { return 0; }\n' | "${CXX_BIN}" -x c++ - \
    -fsanitize=address,undefined -fno-omit-frame-pointer \
    -o "${BUILD_DIR}/sanitizer_probe" >/dev/null 2>&1 && \
    "${BUILD_DIR}/sanitizer_probe" >/dev/null 2>&1; then
  SANITIZER_FLAGS=(-fsanitize=address,undefined -fno-omit-frame-pointer)
  echo "host sanitizers: address,undefined"
else
  echo "host sanitizers: unavailable for ${CXX_BIN}"
fi

# IDF_ROOT is intentionally optional: host SHA stubs keep this test runnable
# when ESP-IDF is not installed, while IDF_PATH/HOME remain portable inputs.
: "${IDF_ROOT}"

"${CXX_BIN}" -std=c++17 -Wall -Wextra -Werror \
  "${SANITIZER_FLAGS[@]}" \
  -DTBOT_LESSON_ASSET_STAGING_TESTING=1 \
  -I"${ROOT}/tests/native_stubs_staging" \
  -I"${ROOT}/main" \
  "${ROOT}/main/lesson_asset_download_staging.cc" \
  "${ROOT}/tests/native/lesson_asset_download_staging_host_test.cc" \
  -o "${BUILD_DIR}/lesson_asset_download_staging_host_test"
"${BUILD_DIR}/lesson_asset_download_staging_host_test"
