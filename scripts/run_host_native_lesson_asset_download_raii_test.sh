#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="$(mktemp -d "${TMPDIR:-/tmp}/tbot-download-raii.XXXXXX")"
trap 'rm -rf "${BUILD_DIR}"' EXIT
"${CXX:-clang++}" -std=c++17 -Wall -Wextra -Werror \
  -fsanitize=address,undefined -fno-omit-frame-pointer \
  -I"${ROOT}/main" \
  "${ROOT}/tests/native/lesson_asset_download_raii_host_test.cc" \
  -o "${BUILD_DIR}/lesson_asset_download_raii_host_test"
"${BUILD_DIR}/lesson_asset_download_raii_host_test"
