#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="$(mktemp -d "${TMPDIR:-/tmp}/tbot-hil-status-contention.XXXXXX")"
IDF_CJSON="${IDF_PATH:?IDF_PATH must point to an ESP-IDF checkout}/components/json/cJSON"
trap 'rm -rf "${BUILD_DIR}"' EXIT
"${CC:-clang}" -std=c99 -Wall -Wextra -Werror \
  -I"${IDF_CJSON}" -c "${IDF_CJSON}/cJSON.c" -o "${BUILD_DIR}/cJSON.o"
"${CXX:-clang++}" -std=c++17 -pthread -Wall -Wextra -Werror \
  -ffunction-sections -fdata-sections \
  -DTBOT_LESSON_STORAGE_HIL_MCP_TOOLS_TESTING \
  -DTBOT_LESSON_ASSET_COORDINATOR_TESTING \
  -I"${ROOT}/tests/native_stubs_mcp" \
  -I"${ROOT}/tests/native_stubs" \
  -I"${ROOT}/main" \
  -I"${IDF_CJSON}" \
  "${ROOT}/tests/native/lesson_storage_hil_status_contention_host_test.cc" \
  "${ROOT}/main/lesson_storage_hil_mcp_tools.cc" \
  "${ROOT}/main/lesson_storage_hil_controller.cc" \
  "${ROOT}/main/lesson_asset_storage_coordinator.cc" \
  "${ROOT}/main/physical_sd_identity.cc" \
  "${ROOT}/main/sd_fat_session_guard.cc" \
  "${BUILD_DIR}/cJSON.o" \
  -Wl,-dead_strip \
  -o "${BUILD_DIR}/lesson_storage_hil_status_contention_host_test"
"${BUILD_DIR}/lesson_storage_hil_status_contention_host_test"
