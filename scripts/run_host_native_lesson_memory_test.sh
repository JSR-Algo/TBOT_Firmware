#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="$(mktemp -d "${TMPDIR:-/tmp}/tbot-lesson-memory-host.XXXXXX")"
trap 'rm -rf "${BUILD_DIR}"' EXIT
CXX_BIN="${CXX:-clang++}"
CXX_FLAGS=(-std=c++17 -Wall -Wextra -Werror \
  -DTBOT_LESSON_MEMORY_TEST \
  -DTBOT_RENDERER_MEMORY_DIAGNOSTICS=1 \
  -DCONFIG_HEAP_USE_HOOKS=1 \
  -DHEAP_IRAM_ATTR=)
if [[ "${SANITIZE:-0}" == "1" ]]; then
  CXX_FLAGS+=(-fsanitize=address,undefined -fno-omit-frame-pointer)
fi

if ! rg -q '"lesson_renderer_memory_probe\.cc"' "${ROOT}/main/CMakeLists.txt"; then
  echo '{"schemaVersion":1,"passed":false,"error":"production-probe-source-not-linked"}'
  exit 1
fi

for production_hook in \
  'AdvanceLessonRendererAnimationFrame' \
  'LessonRendererMemoryDecodedLayerOpened' \
  'LessonRendererMemoryAnimationStarted' \
  'LessonRendererMemoryContextOpened' \
  'LessonRendererMemoryPhase::kStart' \
  'LessonRendererMemoryPhase::kPeak' \
  'LessonRendererMemoryPhase::kComplete' \
  'LessonRendererMemoryPhase::kCancel'; do
  if ! rg -q "${production_hook}" "${ROOT}/main/display/lcd_display.cc"; then
    echo "{\"schemaVersion\":1,\"passed\":false,\"error\":\"missing-production-hook:${production_hook}\"}"
    exit 1
  fi
done

"${CXX_BIN}" "${CXX_FLAGS[@]}" \
  -I"${ROOT}/tests/native_stubs_lesson" \
  -I"${ROOT}/main" \
  "${ROOT}/main/lesson_tvideo_template.cc" \
  "${ROOT}/main/lesson_renderer_memory_probe.cc" \
  "${ROOT}/tests/native/lesson_visual_animation_host_test.cc" \
  -o "${BUILD_DIR}/lesson_memory_host_test"

"${BUILD_DIR}/lesson_memory_host_test"
