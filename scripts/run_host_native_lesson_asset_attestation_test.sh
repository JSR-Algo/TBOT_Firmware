#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="$(mktemp -d "${TMPDIR:-/tmp}/tbot-host-native-lesson-attestation.XXXXXX")"
trap 'rm -rf "${BUILD_DIR}"' EXIT
CXX="${CXX:-clang++}"
CJSON_DIR="${CJSON_DIR:-${HOME}/esp/esp-idf-v5.5.2/components/json/cJSON}"

if [[ ! -f "${CJSON_DIR}/cJSON.c" ]]; then
    echo "missing cJSON.c at ${CJSON_DIR}; set CJSON_DIR" >&2
    exit 127
fi

cd "${ROOT}"
"${CXX}" -std=c++17 -O0 -g \
    -Imain \
    -I"${CJSON_DIR}" \
    tests/native/lesson_asset_sync_attestation_host_test.cc \
    main/lesson_asset_sync_attestation.cc \
    "${CJSON_DIR}/cJSON.c" \
    -o "${BUILD_DIR}/lesson_asset_sync_attestation_host_test"

"${BUILD_DIR}/lesson_asset_sync_attestation_host_test"
