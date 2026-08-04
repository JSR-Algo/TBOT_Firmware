#!/usr/bin/env bash
# Host-native statement coverage for main/lesson_handler.cc (US-006 lesson_* renderer).
# Mirrors scripts/run_host_native_afsk_coverage.sh: clang++ --coverage on the REAL .cc
# compiled against tests/native_stubs_lesson/* fakes + vendored cJSON.c, then gcovr.
# No IDF, no JTAG, no hardware, no network — pure host logic coverage.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
if [[ -n "${TBOT_HOST_NATIVE_COVERAGE_BUILD_DIR:-}" ]]; then
    BUILD_DIR="${TBOT_HOST_NATIVE_COVERAGE_BUILD_DIR}"
else
    BUILD_DIR="$(mktemp -d "${TMPDIR:-/tmp}/tbot-host-native-lesson-coverage.XXXXXX")"
    trap 'rm -rf "${BUILD_DIR}"' EXIT
fi
CXX="${CXX:-clang++}"
CC="${CC:-clang}"
LLVM_COV_BIN="${LLVM_COV_BIN:-$(command -v llvm-cov || true)}"
if [[ -z "${LLVM_COV_BIN}" && -x "/Applications/Xcode.app/Contents/Developer/Toolchains/XcodeDefault.xctoolchain/usr/bin/llvm-cov" ]]; then
    LLVM_COV_BIN="/Applications/Xcode.app/Contents/Developer/Toolchains/XcodeDefault.xctoolchain/usr/bin/llvm-cov"
fi
if [[ -z "${LLVM_COV_BIN}" ]]; then
    echo "missing llvm-cov; install Xcode command line tools or set LLVM_COV_BIN" >&2
    exit 127
fi
LLVM_COV="${LLVM_COV:-${LLVM_COV_BIN} gcov}"
GCOVR_BIN="${GCOVR_BIN:-$(command -v gcovr || true)}"
if [[ -z "${GCOVR_BIN}" && -x "${HOME}/.espressif/python_env/idf5.5_py3.9_env/bin/gcovr" ]]; then
    GCOVR_BIN="${HOME}/.espressif/python_env/idf5.5_py3.9_env/bin/gcovr"
fi
if [[ -z "${GCOVR_BIN}" ]]; then
    echo "missing gcovr; install it or set GCOVR_BIN" >&2
    exit 127
fi

# Vendored cJSON (header + source), standalone (libc only). Same source the afsk note cites.
CJSON_DIR="${CJSON_DIR:-${HOME}/esp/esp-idf-v5.5.2/components/json/cJSON}"
if [[ ! -f "${CJSON_DIR}/cJSON.c" ]]; then
    echo "missing cJSON.c at ${CJSON_DIR}; set CJSON_DIR" >&2
    exit 127
fi

cd "${ROOT}"
rm -rf "${BUILD_DIR}"
mkdir -p "${BUILD_DIR}/src"

# Copy the unit-under-test into a neutral build dir so its quoted #includes
# ("application.h", "assets.h", ...) resolve via our -I stub dir instead of the real
# headers that sit next to it in main/ (clang searches the including file's own dir
# first for quoted includes). The .cc is copied byte-for-byte, never modified, so the
# coverage we measure is of the REAL source. gcovr maps it back via the original path.
cp main/lesson_handler.cc "${BUILD_DIR}/src/lesson_handler.cc"
cp main/lesson_motion_presets.cc "${BUILD_DIR}/src/lesson_motion_presets.cc"
cp main/lesson_layer_state.cc "${BUILD_DIR}/src/lesson_layer_state.cc"
cp main/lesson_asset_storage_coordinator.cc \
    "${BUILD_DIR}/src/lesson_asset_storage_coordinator.cc"

"${CC}" -std=c11 -O0 -g --coverage -Wno-deprecated-declarations \
    -I"${CJSON_DIR}" -c "${CJSON_DIR}/cJSON.c" -o "${BUILD_DIR}/cJSON.o"

"${CXX}" -std=c++17 -O0 -g --coverage \
    -Itests/native_stubs_lesson \
    -Imain \
    tests/native/lesson_layer_state_test.cc \
    "${BUILD_DIR}/src/lesson_layer_state.cc" \
    -o "${BUILD_DIR}/lesson_layer_state_test"

"${BUILD_DIR}/lesson_layer_state_test"

"${CXX}" -std=c++17 -O0 -g --coverage -pthread -Wno-unused-parameter \
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
    main/lesson_tvideo_template.cc \
    main/lesson_cinematic_renderer.cc \
    main/lesson_flattened_cinematic_renderer.cc \
    main/lesson_cinematic_evidence.cc \
    main/lesson_chroma_compositor.cc \
    "${BUILD_DIR}/src/lesson_motion_presets.cc" \
    "${BUILD_DIR}/src/lesson_layer_state.cc" \
    "${BUILD_DIR}/src/lesson_asset_storage_coordinator.cc" \
    main/json_payload_safety.cc \
    main/sd_fat_session_guard.cc \
    "${BUILD_DIR}/cJSON.o" \
    -o "${BUILD_DIR}/lesson_handler_host_test"

TBOT_TVIDEO_FARM_COMMAND_FIXTURE="${ROOT}/tests/fixtures/tvideo_farm_command_v2.json" \
    "${BUILD_DIR}/lesson_handler_host_test"

"${GCOVR_BIN}" -r "${ROOT}" \
    --gcov-executable "${LLVM_COV}" \
    --object-directory "${BUILD_DIR}" \
    --filter '.*lesson_handler\.cc' \
    --filter '.*lesson_motion_presets\.cc' \
    --filter '.*lesson_layer_state\.cc' \
    --fail-under-line 100 \
    --print-summary \
    "$@"
