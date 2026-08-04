#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="$(mktemp -d "${TMPDIR:-/tmp}/tbot-host-native-lesson-handler.XXXXXX")"
trap 'rm -rf "${BUILD_DIR}"' EXIT

CXX="${CXX:-clang++}"
CC="${CC:-clang}"
CJSON_DIR="${CJSON_DIR:-${HOME}/esp/esp-idf-v5.5.2/components/json/cJSON}"
if [[ ! -f "${CJSON_DIR}/cJSON.c" ]]; then
    echo "missing cJSON.c at ${CJSON_DIR}; set CJSON_DIR" >&2
    exit 127
fi

cd "${ROOT}"
mkdir -p "${BUILD_DIR}/src"
cp main/lesson_handler.cc "${BUILD_DIR}/src/lesson_handler.cc"
cp main/lesson_motion_presets.cc "${BUILD_DIR}/src/lesson_motion_presets.cc"
cp main/lesson_layer_state.cc "${BUILD_DIR}/src/lesson_layer_state.cc"
cp main/lesson_asset_storage_coordinator.cc \
    "${BUILD_DIR}/src/lesson_asset_storage_coordinator.cc"

"${CC}" -std=c11 -O0 -g -Wall -Wextra -Werror \
    -I"${CJSON_DIR}" -c "${CJSON_DIR}/cJSON.c" -o "${BUILD_DIR}/cJSON.o"

"${CXX}" -std=c++17 -O0 -g -pthread -Wall -Wextra -Werror \
    -fsanitize=address,undefined -fno-omit-frame-pointer \
    -Wno-unused-variable -Wno-unused-parameter -Wno-unused-lambda-capture \
    -DTBOT_HOST_NATIVE_COVERAGE \
    -DTBOT_LESSON_ASSET_COORDINATOR_TESTING \
    -Dfopen=HostLessonFopen \
    -Dfread=HostLessonFread \
    -Itests/native_stubs_lesson \
    -I"${CJSON_DIR}" \
    -Imain \
    -Imain/protocols \
    tests/native/lesson_handler_host_test.cc \
    "${BUILD_DIR}/src/lesson_handler.cc" \
    "${BUILD_DIR}/src/lesson_motion_presets.cc" \
    "${BUILD_DIR}/src/lesson_layer_state.cc" \
    "${BUILD_DIR}/src/lesson_asset_storage_coordinator.cc" \
    main/lesson_tvideo_template.cc \
    main/lesson_cinematic_renderer.cc \
    main/lesson_flattened_cinematic_renderer.cc \
    main/lesson_cinematic_evidence.cc \
    main/lesson_chroma_compositor.cc \
    main/json_payload_safety.cc \
    main/sd_fat_session_guard.cc \
    "${BUILD_DIR}/cJSON.o" \
    -o "${BUILD_DIR}/lesson_handler_host_test"

TBOT_TVIDEO_FARM_COMMAND_FIXTURE="${ROOT}/tests/fixtures/tvideo_farm_command_v2.json" \
    "${BUILD_DIR}/lesson_handler_host_test"
