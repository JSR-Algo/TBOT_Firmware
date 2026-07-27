#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${TMPDIR:-/tmp}/tbot-lesson-visual-animation-host"
mkdir -p "${BUILD_DIR}"

${CXX:-c++} -std=c++17 -Wall -Wextra -Werror \
  -I"${ROOT}/main" \
  "${ROOT}/main/lesson_tvideo_template.cc" \
  "${ROOT}/tests/native/lesson_visual_animation_host_test.cc" \
  -o "${BUILD_DIR}/lesson_visual_animation_host_test"

"${BUILD_DIR}/lesson_visual_animation_host_test"
