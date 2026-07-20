#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="$(mktemp -d "${TMPDIR:-/tmp}/tbot-sd-fat-session-guard.XXXXXX")"
trap 'rm -rf "${BUILD_DIR}"' EXIT
"${CXX:-clang++}" -std=c++17 -pthread -Wall -Wextra -Werror -pedantic ${CXXFLAGS:-} \
  -I"${ROOT}/main" \
  "${ROOT}/main/sd_fat_session_guard.cc" \
  "${ROOT}/tests/native/sd_fat_session_guard_host_test.cc" \
  -o "${BUILD_DIR}/sd_fat_session_guard_host_test"
"${BUILD_DIR}/sd_fat_session_guard_host_test"
