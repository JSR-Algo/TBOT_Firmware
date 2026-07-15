#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="$(mktemp -d "${TMPDIR:-/tmp}/tbot-host-native-lesson-heap.XXXXXX")"
trap 'rm -rf "${BUILD_DIR}"' EXIT
CXX="${CXX:-clang++}"

cd "${ROOT}"
"${CXX}" -std=c++17 -Wall -Wextra -Werror \
    -Itests/native_stubs_lesson \
    -Imain \
    tests/native/lesson_heap_probe_host_test.cc \
    main/lesson_heap_probe.cc \
    -o "${BUILD_DIR}/lesson_heap_probe_host_test"

"${BUILD_DIR}/lesson_heap_probe_host_test"
