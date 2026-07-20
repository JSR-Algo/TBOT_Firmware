#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="$(mktemp -d "${TMPDIR:-/tmp}/tbot-host-native-firmware-version.XXXXXX")"
trap 'rm -rf "${BUILD_DIR}"' EXIT

${CXX:-c++} -std=c++17 -Wall -Wextra -Werror \
  -I"${ROOT}/main" \
  "${ROOT}/tests/native/firmware_version_policy_host_test.cc" \
  -o "${BUILD_DIR}/firmware_version_policy_host_test"
"${BUILD_DIR}/firmware_version_policy_host_test"
