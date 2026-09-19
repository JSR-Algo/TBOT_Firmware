#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
: "${TMPDIR:?owned TMPDIR required}"
BUILD_DIR="$(mktemp -d "${TMPDIR}/tbot-host-native-lesson-handler.XXXXXX")"
export TBOT_RETAINED_TEST_STATE_PATH="${BUILD_DIR}/selection.record"

CXX="${CXX:-clang++}"
CC="${CC:-clang}"
CJSON_DIR="${CJSON_DIR:-${HOME}/esp/esp-idf-v5.5.2/components/json/cJSON}"
if [[ ! -f "${CJSON_DIR}/cJSON.c" ]]; then
    echo "missing cJSON.c at ${CJSON_DIR}; set CJSON_DIR" >&2
    exit 127
fi

cd "${ROOT}"
mkdir -p "${BUILD_DIR}/src"
python3 - "${BUILD_DIR}" <<'PY'
from pathlib import Path
import sys
root = Path(sys.argv[1])
assert all(c.isalnum() or c in '/-._' for c in str(root)), 'fixture shell paths require a simple owned root'
fixture = root / 'fixtures'
fixture.mkdir()
source = Path('tests/native/lesson_handler_host_test.cc').read_text()
(root / 'src/lesson_handler_host_test.cc').write_text(source.replace('/tmp', str(fixture)))
PY
cp main/lesson_handler.cc "${BUILD_DIR}/src/lesson_handler.cc"
cp main/lesson_embodied_action.cc "${BUILD_DIR}/src/lesson_embodied_action.cc"
cp main/lesson_motion_presets.cc "${BUILD_DIR}/src/lesson_motion_presets.cc"
cp main/lesson_layer_state.cc "${BUILD_DIR}/src/lesson_layer_state.cc"
cp main/lesson_asset_storage_coordinator.cc \
    "${BUILD_DIR}/src/lesson_asset_storage_coordinator.cc"
python3 scripts/extract_lesson_host_application.py main/application.cc \
    "${BUILD_DIR}/src/lesson_application_context.cc"

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
    "${BUILD_DIR}/src/lesson_handler_host_test.cc" \
    "${BUILD_DIR}/src/lesson_application_context.cc" \
    "${BUILD_DIR}/src/lesson_handler.cc" \
    "${BUILD_DIR}/src/lesson_embodied_action.cc" \
    "${BUILD_DIR}/src/lesson_motion_presets.cc" \
    "${BUILD_DIR}/src/lesson_layer_state.cc" \
    "${BUILD_DIR}/src/lesson_asset_storage_coordinator.cc" \
    main/lesson_tvideo_template.cc \
    main/lesson_cinematic_renderer.cc \
    main/lesson_flattened_cinematic_renderer.cc \
    main/lesson_layered_cinematic_renderer.cc \
    main/lesson_cinematic_evidence.cc \
    main/lesson_chroma_compositor.cc \
    main/json_payload_safety.cc \
    main/sd_fat_session_guard.cc \
    main/lesson_asset_retained_selection.cc \
    main/lesson_asset_retained_parser.cc \
    main/lesson_asset_cache_evict.cc \
    "${BUILD_DIR}/cJSON.o" \
    -o "${BUILD_DIR}/lesson_handler_host_test"

TBOT_TVIDEO_FARM_COMMAND_FIXTURE="${ROOT}/tests/fixtures/tvideo_farm_command_v2.json" \
    "${BUILD_DIR}/lesson_handler_host_test"
