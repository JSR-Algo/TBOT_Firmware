#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="$(mktemp -d "${TMPDIR:-/tmp}/tbot-esp-build-identity.XXXXXX")"
trap 'rm -rf "${BUILD_DIR}"' EXIT
build_and_run() {
  local output="$1"
  shift
  "${CXX:-clang++}" -std=c++17 -pthread -Wall -Wextra -Werror -pedantic ${CXXFLAGS:-} \
    -DTBOT_BUILD_IDENTITY_HOST_TEST "$@" \
    -I"${ROOT}/main" \
    "${ROOT}/main/esp_build_identity.cc" \
    "${ROOT}/tests/native/esp_build_identity_host_test.cc" \
    -o "${BUILD_DIR}/${output}"
  "${BUILD_DIR}/${output}"
}

build_and_run esp_build_identity_host_test
build_and_run esp_build_identity_course_mode_host_test \
  -DCONFIG_TBOT_COURSE_MODE_LOCAL_ENDPOINT=1 \
  -DTBOT_EXPECT_LOCAL_ENDPOINT_PROFILE
