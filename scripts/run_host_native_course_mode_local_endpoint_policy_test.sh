#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="$(mktemp -d "${TMPDIR:-/tmp}/tbot-course-mode-endpoint.XXXXXX")"
trap 'rm -rf "${BUILD_DIR}"' EXIT
"${CXX:-clang++}" -std=c++17 -Wall -Wextra -Werror -pedantic ${CXXFLAGS:-} \
  -I"${ROOT}/main" \
  "${ROOT}/main/course_mode_local_endpoint_policy.cc" \
  "${ROOT}/tests/native/course_mode_local_endpoint_policy_test.cc" \
  -o "${BUILD_DIR}/course_mode_local_endpoint_policy_test"
"${BUILD_DIR}/course_mode_local_endpoint_policy_test"
