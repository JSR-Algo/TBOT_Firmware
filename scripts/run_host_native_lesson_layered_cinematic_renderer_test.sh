#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="$(mktemp -d "${TMPDIR:-/tmp}/tbot-lesson-layered-cinematic.XXXXXX")"
trap 'rm -rf "${BUILD_DIR}"' EXIT
for mode in host esp-log; do
  CXX_FLAGS=(-std=c++17 -O0 -g -Wall -Wextra -Werror)
  if [[ "${mode}" == esp-log ]]; then
    CXX_FLAGS+=(-DESP_PLATFORM -I"${ROOT}/tests/native_stubs_layered")
  fi
  "${CXX:-clang++}" "${CXX_FLAGS[@]}" \
    -fsanitize=address,undefined -fno-omit-frame-pointer -I"${ROOT}/main" \
    "${ROOT}/tests/native/lesson_layered_cinematic_renderer_test.cc" \
    "${ROOT}/main/lesson_layered_cinematic_renderer.cc" \
    "${ROOT}/main/lesson_chroma_compositor.cc" \
    -o "${BUILD_DIR}/lesson_layered_cinematic_renderer_test"
  echo "layered renderer configuration: ${mode}"
  "${BUILD_DIR}/lesson_layered_cinematic_renderer_test" "$@"
done
