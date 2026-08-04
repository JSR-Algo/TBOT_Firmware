#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="$(mktemp -d "${TMPDIR:-/tmp}/tbot-lesson-cinematic-evidence.XXXXXX")"
trap 'rm -rf "${BUILD_DIR}"' EXIT
CONTRACT_FILES=(
  "${ROOT}/main/lesson_cinematic_evidence.h"
  "${ROOT}/main/lesson_cinematic_evidence.cc"
  "${ROOT}/tests/native_stubs_hil_telemetry/esp_heap_caps.h"
  "${ROOT}/tests/native_stubs_hil_telemetry/esp_random.h"
  "${ROOT}/tests/native_stubs_hil_telemetry/esp_rom_sys.h"
)
FORBIDDEN_CONTRACT='heap_caps_monitor|heap_monitor|(^|[^[:alnum:]_])(malloc|calloc|realloc|free|new|delete|socket|connect|http|https|websocket|network|mcp)([^[:alnum:]_]|$)'
if rg -n -i "${FORBIDDEN_CONTRACT}" "${CONTRACT_FILES[@]}"; then
  echo "cinematic evidence collector violates its bounded serial-only contract" >&2
  exit 1
fi
"${CXX:-clang++}" -std=c++17 -O0 -g -Wall -Wextra -Werror \
  -fsanitize=address,undefined -fno-omit-frame-pointer -I"${ROOT}/main" \
  "${ROOT}/tests/native/lesson_cinematic_evidence_host_test.cc" \
  "${ROOT}/main/lesson_cinematic_evidence.cc" \
  -o "${BUILD_DIR}/lesson_cinematic_evidence_test"
"${BUILD_DIR}/lesson_cinematic_evidence_test"
"${CXX:-clang++}" -std=c++17 -O0 -g -Wall -Wextra -Werror \
  -fsanitize=address,undefined -fno-omit-frame-pointer -I"${ROOT}/main" \
  -DESP_PLATFORM -DCONFIG_TBOT_RELEASE_CINEMATIC_EVIDENCE=0 \
  -DCONFIG_TBOT_HIL_CINEMATIC_TELEMETRY=0 \
  "${ROOT}/tests/native/lesson_cinematic_evidence_host_test.cc" \
  "${ROOT}/main/lesson_cinematic_evidence.cc" \
  -o "${BUILD_DIR}/lesson_cinematic_evidence_esp_disabled_test"
"${BUILD_DIR}/lesson_cinematic_evidence_esp_disabled_test"
"${CXX:-clang++}" -std=c++17 -O0 -g -Wall -Wextra -Werror \
  -fsanitize=address,undefined -fno-omit-frame-pointer \
  -I"${ROOT}/tests/native_stubs_hil_telemetry" -I"${ROOT}/main" \
  -DESP_PLATFORM -DCONFIG_TBOT_RELEASE_CINEMATIC_EVIDENCE=1 \
  -DCONFIG_TBOT_HIL_CINEMATIC_TELEMETRY=0 \
  "${ROOT}/tests/native/lesson_cinematic_evidence_host_test.cc" \
  "${ROOT}/main/lesson_cinematic_evidence.cc" \
  -o "${BUILD_DIR}/lesson_cinematic_evidence_esp_enabled_test"
"${BUILD_DIR}/lesson_cinematic_evidence_esp_enabled_test"
