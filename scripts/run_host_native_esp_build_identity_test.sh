#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="$(mktemp -d "${TMPDIR:-/tmp}/tbot-esp-build-identity.XXXXXX")"
trap 'rm -rf "${BUILD_DIR}"' EXIT
"${CXX:-clang++}" -std=c++17 -pthread -Wall -Wextra -Werror -pedantic ${CXXFLAGS:-} \
  -DTBOT_BUILD_IDENTITY_HOST_TEST \
  -I"${ROOT}/main" \
  "${ROOT}/main/esp_build_identity.cc" \
  "${ROOT}/tests/native/esp_build_identity_host_test.cc" \
  -o "${BUILD_DIR}/esp_build_identity_host_test"
"${BUILD_DIR}/esp_build_identity_host_test"
