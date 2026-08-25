#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="$(mktemp -d "${TMPDIR:-/tmp}/tbot-wake-word-lifecycle.XXXXXX")"
trap 'rm -rf "${BUILD_DIR}"' EXIT

"${CXX:-c++}" -std=c++17 -pthread \
    -I"${ROOT_DIR}/tests/native_stubs" \
    -I"${ROOT_DIR}/main" \
    "${ROOT_DIR}/tests/native/wake_word_lifecycle_gate_test.cc" \
    "${ROOT_DIR}/main/device_state_machine.cc" \
    -o "${BUILD_DIR}/wake_word_lifecycle_controller_test"

"${BUILD_DIR}/wake_word_lifecycle_controller_test"

"${CXX:-c++}" -std=c++17 -pthread \
    -I"${ROOT_DIR}/main" \
    "${ROOT_DIR}/tests/native/afe_run_synchronization_test.cc" \
    -o "${BUILD_DIR}/afe_run_synchronization_test"

"${BUILD_DIR}/afe_run_synchronization_test"

if rg -q 'atomic_flag|test_and_set' \
    "${ROOT_DIR}/main/audio/wake_words/wake_word_telemetry.h"; then
    echo "wake word telemetry test failed: task-level spin guards are forbidden" >&2
    exit 1
fi

"${CXX:-c++}" -std=c++17 -pthread \
    -I"${ROOT_DIR}/main" \
    "${ROOT_DIR}/tests/native/wake_word_telemetry_test.cc" \
    -o "${BUILD_DIR}/wake_word_telemetry_test"

"${BUILD_DIR}/wake_word_telemetry_test"

"${CXX:-c++}" -std=c++17 \
    -I"${ROOT_DIR}/main" \
    "${ROOT_DIR}/tests/native/wake_word_model_map_test.cc" \
    -o "${BUILD_DIR}/wake_word_model_map_test"

"${BUILD_DIR}/wake_word_model_map_test"

"${CXX:-c++}" -std=c++17 -pthread -Wall -Wextra -Werror \
    -I"${ROOT_DIR}/main" \
    "${ROOT_DIR}/tests/native/opus_encoder_serialization_test.cc" \
    "${ROOT_DIR}/main/audio/opus_encoder_serialization.cc" \
    -o "${BUILD_DIR}/opus_encoder_serialization_test"

"${BUILD_DIR}/opus_encoder_serialization_test"
