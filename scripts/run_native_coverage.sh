#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${TBOT_NATIVE_COVERAGE_BUILD_DIR:-build-native-coverage}"
IDF_TARGET="${IDF_TARGET:-esp32s3}"
SDKCONFIG_DEFAULTS="${SDKCONFIG_DEFAULTS:-sdkconfig.defaults;sdkconfig.defaults.esp32s3;sdkconfig.defaults.local;sdkconfig.defaults.coverage}"
TBOT_NATIVE_COVERAGE_SOURCES="${TBOT_NATIVE_COVERAGE_SOURCES:-boards/common/afsk_demod.cc}"
TBOT_NATIVE_COVERAGE_MIN="${TBOT_NATIVE_COVERAGE_MIN:-100}"
TBOT_NATIVE_COVERAGE_FLASH="${TBOT_NATIVE_COVERAGE_FLASH:-0}"
OPENOCD_PORT="${TBOT_NATIVE_COVERAGE_OPENOCD_PORT:-4444}"
OPENOCD_ARGS="${TBOT_NATIVE_COVERAGE_OPENOCD_ARGS:--f board/esp32s3-builtin.cfg}"
OPENOCD_WAIT_SEC="${TBOT_NATIVE_COVERAGE_OPENOCD_WAIT_SEC:-20}"
DUMP_COMMAND="${TBOT_NATIVE_COVERAGE_DUMP_COMMAND:-esp gcov dump}"

OPENOCD_LOG="${ROOT}/${BUILD_DIR}/openocd-gcov.log"
DUMP_LOG="${ROOT}/${BUILD_DIR}/openocd-gcov-dump.log"
openocd_pid=""

cleanup() {
    if [[ -n "${openocd_pid}" ]] && kill -0 "${openocd_pid}" 2>/dev/null; then
        kill "${openocd_pid}" 2>/dev/null || true
        wait "${openocd_pid}" 2>/dev/null || true
    fi
}
trap cleanup EXIT

require_tool() {
    if ! command -v "$1" >/dev/null 2>&1; then
        echo "missing required tool: $1" >&2
        exit 127
    fi
}

cd "${ROOT}"
require_tool idf.py
require_tool openocd
require_tool nc
require_tool gcovr
require_tool xtensa-esp32s3-elf-gcov

idf.py -B "${BUILD_DIR}" \
    -DSDKCONFIG="${BUILD_DIR}/sdkconfig" \
    -DSDKCONFIG_DEFAULTS="${SDKCONFIG_DEFAULTS}" \
    -DIDF_TARGET="${IDF_TARGET}" \
    -DTBOT_NATIVE_COVERAGE=ON \
    -DTBOT_NATIVE_COVERAGE_SOURCES="${TBOT_NATIVE_COVERAGE_SOURCES}" \
    build

if [[ "${TBOT_NATIVE_COVERAGE_FLASH}" == "1" ]]; then
    if [[ -z "${ESPPORT:-}" ]]; then
        echo "ESPPORT is required when TBOT_NATIVE_COVERAGE_FLASH=1" >&2
        exit 2
    fi
    idf.py -B "${BUILD_DIR}" -p "${ESPPORT}" flash
fi

mkdir -p "${ROOT}/${BUILD_DIR}"
openocd ${OPENOCD_ARGS} > "${OPENOCD_LOG}" 2>&1 &
openocd_pid="$!"

ready=0
for _ in $(seq 1 "${OPENOCD_WAIT_SEC}"); do
    if nc -z 127.0.0.1 "${OPENOCD_PORT}" >/dev/null 2>&1; then
        ready=1
        break
    fi
    if ! kill -0 "${openocd_pid}" 2>/dev/null; then
        break
    fi
    sleep 1
done

if [[ "${ready}" != "1" ]]; then
    echo "OpenOCD telnet port ${OPENOCD_PORT} did not become ready; see ${OPENOCD_LOG}" >&2
    exit 3
fi

printf '%s\nshutdown\n' "${DUMP_COMMAND}" | nc 127.0.0.1 "${OPENOCD_PORT}" > "${DUMP_LOG}" 2>&1 || true
wait "${openocd_pid}" 2>/dev/null || true
openocd_pid=""

gcda_count="$(find "${BUILD_DIR}" -type f -name '*.gcda' | wc -l | tr -d ' ')"
if [[ "${gcda_count}" == "0" ]]; then
    echo "no .gcda files were dumped; run the instrumented firmware on ESP32-S3 with JTAG attached" >&2
    echo "OpenOCD log: ${OPENOCD_LOG}" >&2
    echo "Dump log: ${DUMP_LOG}" >&2
    exit 4
fi

idf.py -B "${BUILD_DIR}" gcovr-report
gcovr -r "${ROOT}" \
    --gcov-executable xtensa-esp32s3-elf-gcov \
    --filter main/boards/common/afsk_demod.cc \
    --fail-under-line "${TBOT_NATIVE_COVERAGE_MIN}" \
    --fail-under-function "${TBOT_NATIVE_COVERAGE_MIN}" \
    --print-summary
