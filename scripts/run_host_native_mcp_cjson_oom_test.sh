#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="$(mktemp -d "${TMPDIR:-/tmp}/tbot-mcp-cjson-oom.XXXXXX")"
if [[ -z "${IDF_PATH:-}" ]]; then
  echo "IDF_PATH must point to an ESP-IDF checkout" >&2
  exit 2
fi
IDF_CJSON="${IDF_PATH}/components/json/cJSON"
trap 'rm -rf "${BUILD_DIR}"' EXIT
"${CC:-clang}" -std=c99 -Wall -Wextra -Werror \
  -I"${IDF_CJSON}" -c "${IDF_CJSON}/cJSON.c" -o "${BUILD_DIR}/cJSON.o"
"${CXX:-clang++}" -std=c++17 -Wall -Wextra -Werror \
  -ffunction-sections -fdata-sections \
  -I"${ROOT}/tests/native_stubs_mcp" \
  -I"${ROOT}/main" \
  -I"${IDF_CJSON}" \
  "${ROOT}/tests/native/mcp_cjson_oom_host_test.cc" \
  "${ROOT}/main/lesson_storage_hil_mcp_tools.cc" \
  "${BUILD_DIR}/cJSON.o" \
  -Wl,-dead_strip \
  -o "${BUILD_DIR}/mcp_cjson_oom_host_test"
"${BUILD_DIR}/mcp_cjson_oom_host_test"

"${CXX:-clang++}" -std=c++17 -Wall -Wextra -Werror \
  -ffunction-sections -fdata-sections \
  -I"${ROOT}/tests/native_stubs_mcp" \
  -I"${ROOT}/main" \
  -I"${IDF_CJSON}" \
  "${ROOT}/tests/native/lesson_storage_hil_mcp_validation_host_test.cc" \
  "${ROOT}/main/lesson_storage_hil_mcp_tools.cc" \
  "${BUILD_DIR}/cJSON.o" \
  -Wl,-dead_strip \
  -o "${BUILD_DIR}/lesson_storage_hil_mcp_validation_host_test"
"${BUILD_DIR}/lesson_storage_hil_mcp_validation_host_test"
