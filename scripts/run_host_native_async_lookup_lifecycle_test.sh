#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="$(mktemp -d "${TMPDIR:-/tmp}/tbot-host-native-async-lookup.XXXXXX")"
trap 'rm -rf "${BUILD_DIR}"' EXIT

${CXX:-c++} -std=c++17 -Wall -Wextra -Werror -pthread \
  -I"${ROOT}/components/esp-ml307/include" \
  "${ROOT}/tests/native/async_lookup_lifecycle_host_test.cc" \
  -o "${BUILD_DIR}/async_lookup_lifecycle_host_test"
"${BUILD_DIR}/async_lookup_lifecycle_host_test"
