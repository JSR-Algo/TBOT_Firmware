#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CONFIG="${ROOT}/build/config/sdkconfig.h"
ELF="${ROOT}/build/xiaozhi.elf"
MAP="${ROOT}/build/xiaozhi.map"
NM="${NM:-xtensa-esp32s3-elf-nm}"

test -f "${CONFIG}"
test -f "${ELF}"
test -f "${MAP}"
rg -q '^#define CONFIG_IDF_TARGET_ESP32S3 1$' "${CONFIG}"
rg -q '^#define CONFIG_JD_USE_ROM 1$' "${CONFIG}"

SYMBOLS="$(${NM} -C "${ELF}")"
for required in esp_jpeg_decode esp_jpeg_get_image_info jd_prepare jd_decomp jpeg_enc_open jpeg_enc_process; do
    rg -q "[[:space:]]${required}$" <<<"${SYMBOLS}"
done
for forbidden in jpeg_dec_open jpeg_dec_process jpeg_dec_parse_header; do
    if rg -q "[[:space:]]${forbidden}$" <<<"${SYMBOLS}"; then
        echo "forbidden esp_new_jpeg decoder symbol linked: ${forbidden}" >&2
        exit 1
    fi
done

rg -q 'PROVIDE \(jd_prepare = 0x4000' "${MAP}"
rg -q 'PROVIDE \(jd_decomp = 0x4000' "${MAP}"
echo "ESP32-S3 build uses ROM TJPG symbols; esp_new_jpeg encoder remains linked"
