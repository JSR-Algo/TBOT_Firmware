#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
if [[ -n "${TBOT_HOST_NATIVE_COVERAGE_BUILD_DIR:-}" ]]; then
    BUILD_DIR="${TBOT_HOST_NATIVE_COVERAGE_BUILD_DIR}"
else
    BUILD_DIR="$(mktemp -d "${TMPDIR:-/tmp}/tbot-host-native-afsk-coverage.XXXXXX")"
    trap 'rm -rf "${BUILD_DIR}"' EXIT
fi
CXX="${CXX:-clang++}"
LLVM_COV_BIN="${LLVM_COV_BIN:-$(command -v llvm-cov || true)}"
if [[ -z "${LLVM_COV_BIN}" && -x "/Applications/Xcode.app/Contents/Developer/Toolchains/XcodeDefault.xctoolchain/usr/bin/llvm-cov" ]]; then
    LLVM_COV_BIN="/Applications/Xcode.app/Contents/Developer/Toolchains/XcodeDefault.xctoolchain/usr/bin/llvm-cov"
fi
if [[ -z "${LLVM_COV_BIN}" ]]; then
    echo "missing llvm-cov; install Xcode command line tools or set LLVM_COV_BIN" >&2
    exit 127
fi
LLVM_COV="${LLVM_COV:-${LLVM_COV_BIN} gcov}"
GCOVR_BIN="${GCOVR_BIN:-$(command -v gcovr || true)}"
if [[ -z "${GCOVR_BIN}" && -x "${HOME}/.espressif/python_env/idf5.5_py3.9_env/bin/gcovr" ]]; then
    GCOVR_BIN="${HOME}/.espressif/python_env/idf5.5_py3.9_env/bin/gcovr"
fi
if [[ -z "${GCOVR_BIN}" ]]; then
    echo "missing gcovr; install it or set GCOVR_BIN" >&2
    exit 127
fi

cd "${ROOT}"
rm -rf "${BUILD_DIR}"
mkdir -p "${BUILD_DIR}"

"${CXX}" -std=c++17 -O0 -g --coverage \
    -DTBOT_HOST_NATIVE_COVERAGE \
    -Itests/native_stubs \
    -Imain/boards/common \
    tests/native/afsk_demod_host_test.cc \
    main/boards/common/afsk_demod.cc \
    -o "${BUILD_DIR}/afsk_demod_host_test"

"${BUILD_DIR}/afsk_demod_host_test"

"${GCOVR_BIN}" -r "${ROOT}" \
    --gcov-executable "${LLVM_COV}" \
    --object-directory "${BUILD_DIR}" \
    --filter main/boards/common/afsk_demod.cc \
    --fail-under-line 100 \
    --fail-under-function 100 \
    --print-summary
