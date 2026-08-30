#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="$(mktemp -d "${TMPDIR:-/tmp}/tbot-course-mode-hil-diagnostic.XXXXXX")"
trap 'rm -rf "${BUILD_DIR}"' EXIT
CJSON_DIR="${CJSON_DIR:-${HOME}/esp/esp-idf-v5.5.2/components/json/cJSON}"

"${CC:-clang}" -std=c11 -I"${CJSON_DIR}" -c "${CJSON_DIR}/cJSON.c" -o "${BUILD_DIR}/cJSON.o"
"${CC:-clang}" -std=c11 -c \
  "${IDF_PATH:-${HOME}/esp/esp-idf-v5.5.2}/components/console/split_argv.c" \
  -o "${BUILD_DIR}/split_argv.o"
"${CXX:-clang++}" -std=c++17 -I"${ROOT}/main" -I"${CJSON_DIR}" \
  "${ROOT}/tests/native/course_mode_hil_diagnostic_test.cc" \
  "${ROOT}/main/course_mode_hil_diagnostic.cc" "${BUILD_DIR}/cJSON.o" \
  "${BUILD_DIR}/split_argv.o" \
  -o "${BUILD_DIR}/course_mode_hil_diagnostic_test"
"${BUILD_DIR}/course_mode_hil_diagnostic_test"
