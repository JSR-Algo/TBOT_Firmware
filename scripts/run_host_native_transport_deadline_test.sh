#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="$(mktemp -d "${TMPDIR:-/tmp}/tbot-host-native-transport-deadline.XXXXXX")"
trap 'rm -rf "${BUILD_DIR}"' EXIT

${CXX:-c++} -std=c++17 -Wall -Wextra -Werror \
  -I"${ROOT}/components/esp-ml307/include" \
  "${ROOT}/tests/native/transport_deadline_host_test.cc" \
  -o "${BUILD_DIR}/transport_deadline_host_test"
"${BUILD_DIR}/transport_deadline_host_test"
