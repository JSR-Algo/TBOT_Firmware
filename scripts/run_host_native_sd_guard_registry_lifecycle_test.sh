#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="$(mktemp -d "${TMPDIR:-/tmp}/tbot-sd-guard-registry.XXXXXX")"
trap 'rm -rf "${BUILD_DIR}"' EXIT
"${CXX:-clang++}" -std=c++17 -pthread -Wall -Wextra -Werror -pedantic ${CXXFLAGS:-} \
  -I"${ROOT}/tests/native_stubs" -I"${ROOT}/main" \
  "${ROOT}/main/physical_sd_identity.cc" \
  "${ROOT}/main/sd_fat_session_guard.cc" \
  "${ROOT}/tests/native/sd_guard_registry_lifecycle_host_test.cc" \
  -o "${BUILD_DIR}/sd_guard_registry_lifecycle_host_test"
"${BUILD_DIR}/sd_guard_registry_lifecycle_host_test"
