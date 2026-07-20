#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="$(mktemp -d "${TMPDIR:-/tmp}/tbot-lesson-transport-epoch.XXXXXX")"
trap 'rm -rf "${BUILD_DIR}"' EXIT
"${CXX:-clang++}" -std=c++17 -pthread -Wall -Wextra -Werror ${CXXFLAGS:-} \
  -I"${ROOT}/main" \
  "${ROOT}/tests/native/lesson_transport_epoch_gate_host_test.cc" \
  -o "${BUILD_DIR}/lesson_transport_epoch_gate_host_test"
"${BUILD_DIR}/lesson_transport_epoch_gate_host_test"
