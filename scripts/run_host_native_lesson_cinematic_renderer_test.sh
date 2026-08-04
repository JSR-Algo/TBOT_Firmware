#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="$(mktemp -d "${TMPDIR:-/tmp}/tbot-lesson-cinematic.XXXXXX")"
trap 'rm -rf "${BUILD_DIR}"' EXIT
"${CXX:-clang++}" -std=c++17 -O0 -g -Wall -Wextra -Werror \
  -fsanitize=address,undefined -fno-omit-frame-pointer -I"${ROOT}/main" \
  "${ROOT}/tests/native/lesson_cinematic_renderer_test.cc" \
  "${ROOT}/main/lesson_cinematic_renderer.cc" \
  "${ROOT}/main/lesson_flattened_cinematic_renderer.cc" \
  "${ROOT}/main/lesson_cinematic_evidence.cc" \
  "${ROOT}/main/lesson_chroma_compositor.cc" \
  -o "${BUILD_DIR}/lesson_cinematic_renderer_test"
"${BUILD_DIR}/lesson_cinematic_renderer_test"
