#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
FIXTURE="${1:-}"
if [[ -z "${FIXTURE}" || ! -f "${FIXTURE}" ]]; then
  echo "usage: $0 /absolute/path/to/renderer-v2-manifest.json" >&2
  exit 2
fi

BUILD_DIR="$(mktemp -d "${TMPDIR:-/tmp}/tbot-renderer-trace.XXXXXX")"
trap 'rm -rf "${BUILD_DIR}"' EXIT
CXX="${CXX:-clang++}"
CC="${CC:-clang}"
CJSON_DIR="${CJSON_DIR:-${HOME}/esp/esp-idf-v5.5.2/components/json/cJSON}"
if [[ ! -f "${CJSON_DIR}/cJSON.c" ]]; then
  echo "missing cJSON.c at ${CJSON_DIR}; set CJSON_DIR" >&2
  exit 127
fi

mkdir -p "${BUILD_DIR}/src"
cp "${ROOT}/main/lesson_handler.cc" "${BUILD_DIR}/src/lesson_handler.cc"
cp "${ROOT}/main/lesson_motion_presets.cc" "${BUILD_DIR}/src/lesson_motion_presets.cc"
cp "${ROOT}/tests/native/lesson_renderer_trace_host_test.cc" "${BUILD_DIR}/src/lesson_renderer_trace_host_test.cc"

"${CC}" -std=c11 -O0 -Wall -Wextra -Werror \
  -I"${CJSON_DIR}" -c "${CJSON_DIR}/cJSON.c" -o "${BUILD_DIR}/cJSON.o"

"${CXX}" -std=c++17 -O0 -pthread -Wall -Wextra -Werror \
  -Wno-unused-variable -Wno-unused-parameter -Wno-unused-lambda-capture \
  -DTBOT_HOST_NATIVE_COVERAGE \
  -DTBOT_LESSON_ASSET_COORDINATOR_TESTING \
  -Dfopen=HostLessonFopen \
  -Dfread=HostLessonFread \
  -I"${ROOT}/tests/native_stubs_lesson" \
  -I"${CJSON_DIR}" \
  -I"${ROOT}/main" \
  -I"${ROOT}/main/protocols" \
  "${BUILD_DIR}/src/lesson_renderer_trace_host_test.cc" \
  "${BUILD_DIR}/src/lesson_motion_presets.cc" \
  "${ROOT}/main/lesson_layer_state.cc" \
  "${ROOT}/main/lesson_asset_storage_coordinator.cc" \
  "${ROOT}/main/lesson_tvideo_template.cc" \
  "${ROOT}/main/json_payload_safety.cc" \
  "${ROOT}/main/sd_fat_session_guard.cc" \
  "${BUILD_DIR}/cJSON.o" \
  -o "${BUILD_DIR}/lesson_renderer_trace_host_test"

"${BUILD_DIR}/lesson_renderer_trace_host_test" "${FIXTURE}"
