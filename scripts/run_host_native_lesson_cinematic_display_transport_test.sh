#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="$(mktemp -d "${TMPDIR:-/tmp}/tbot-lesson-cinematic-display-transport.XXXXXX")"
trap 'rm -rf "${BUILD_DIR}"' EXIT
"${CXX:-clang++}" -std=c++17 -O0 -g -Wall -Wextra -Werror -pthread \
  -fsanitize=address,undefined -fno-omit-frame-pointer -I"${ROOT}/main" \
  "${ROOT}/tests/native/lesson_cinematic_display_transport_test.cc" \
  -o "${BUILD_DIR}/lesson_cinematic_display_transport_test"
"${BUILD_DIR}/lesson_cinematic_display_transport_test"
