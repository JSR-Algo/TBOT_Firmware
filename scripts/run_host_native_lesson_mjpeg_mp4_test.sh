#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="$(mktemp -d "${TMPDIR:-/tmp}/tbot-lesson-mjpeg-mp4.XXXXXX")"
trap 'rm -rf "${BUILD_DIR}"' EXIT
"${CXX:-clang++}" -std=c++17 -O0 -g -Wall -Wextra -Werror \
  -fsanitize=address,undefined -fno-omit-frame-pointer \
  -I"${ROOT}/main" \
  "${ROOT}/main/lesson_mjpeg_mp4.cc" \
  "${ROOT}/main/lesson_asset_storage_coordinator.cc" \
  "${ROOT}/main/sd_fat_session_guard.cc" \
  "${ROOT}/tests/native/lesson_mjpeg_mp4_test.cc" \
  -o "${BUILD_DIR}/lesson_mjpeg_mp4_test"
"${BUILD_DIR}/lesson_mjpeg_mp4_test"
