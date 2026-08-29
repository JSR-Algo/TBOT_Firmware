#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD="$(mktemp -d "${TMPDIR:-/tmp}/tbot-course-mode-hil-sd-reader.XXXXXX")"
trap 'rm -rf "${BUILD}"' EXIT

"${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror -I"${ROOT}/main" \
  "${ROOT}/main/course_mode_hil_sd_reader.cc" \
  "${ROOT}/tests/native/course_mode_hil_sd_reader_test.cc" \
  -o "${BUILD}/course_mode_hil_sd_reader_test"
"${BUILD}/course_mode_hil_sd_reader_test"
