#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="$(mktemp -d "${TMPDIR:-/tmp}/tbot-host-native-passive-ws.XXXXXX")"
trap 'rm -rf "${BUILD_DIR}"' EXIT

cd "${ROOT_DIR}"

${CXX:-c++} -std=c++17 -Wall -Wextra -Werror -pthread \
    -Imain/protocols \
    -Imain \
    -Icomponents/esp-ml307/src/esp \
    tests/native/passive_websocket_liveness_host_test.cc \
    -o "${BUILD_DIR}/passive_websocket_liveness_host_test"

"${BUILD_DIR}/passive_websocket_liveness_host_test"
