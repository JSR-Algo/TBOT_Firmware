#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="$(mktemp -d "${TMPDIR:-/tmp}/tbot-lesson-cinematic-evidence.XXXXXX")"
trap 'rm -rf "${BUILD_DIR}"' EXIT
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
