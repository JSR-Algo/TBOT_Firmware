#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="$(mktemp -d "${TMPDIR:-/tmp}/tbot-blufi-transition.XXXXXX")"
trap 'rm -rf "${BUILD_DIR}"' EXIT

"${CXX:-c++}" -std=c++17 -pthread \
    -I"${ROOT_DIR}/main" \
    "${ROOT_DIR}/tests/native/blufi_transition_gate_test.cc" \
    -o "${BUILD_DIR}/blufi_transition_gate_test"

"${BUILD_DIR}/blufi_transition_gate_test"
