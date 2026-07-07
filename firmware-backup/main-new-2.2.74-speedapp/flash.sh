#!/usr/bin/env bash
#
# flash.sh — Nap lai NHANH ban main MOI (2.2.74 + app "Toc do") tu backup nay.
# Khong can build lai. Dung dung cac .bin da luu trong thu muc nay.
#
# Cach dung:
#   ./flash.sh                      # nap toi /dev/cu.usbmodem1101 (port main mac dinh)
#   ./flash.sh /dev/cu.usbmodemXXXX # nap toi port khac
#
# Chi nap board MAIN (LCDWiki ES3C35P, ESP32-S3 16MB). KHONG nap vao slave.

set -euo pipefail
DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PORT="${1:-/dev/cu.usbmodem1101}"

IDF_EXPORT="${IDF_EXPORT:-$HOME/esp/esp-idf-v5.5.4/export.sh}"
if [ ! -f "$IDF_EXPORT" ]; then
    echo "ERROR: khong thay export.sh o $IDF_EXPORT (set IDF_EXPORT=...)" >&2
    exit 1
fi
set +u; source "$IDF_EXPORT"; set -u

if [ ! -e "$PORT" ]; then
    echo "ERROR: khong thay port '$PORT'. Cam board main roi thu lai." >&2
    exit 1
fi

# Kiem tra toan ven xiaozhi.bin truoc khi nap.
if [ -f "$DIR/xiaozhi.bin.sha256" ]; then
    want="$(cat "$DIR/xiaozhi.bin.sha256")"
    got="$(shasum -a256 "$DIR/xiaozhi.bin" | awk '{print $1}')"
    if [ "$want" != "$got" ]; then
        echo "ERROR: xiaozhi.bin sha256 khong khop backup -> file co the bi hong." >&2
        exit 1
    fi
fi

echo "Nap ban MOI (2.2.74 + Toc do) vao $PORT ..."
python -m esptool --chip esp32s3 -p "$PORT" -b 460800 \
    --before default_reset --after hard_reset write_flash \
    --flash_mode dio --flash_size 16MB --flash_freq 80m \
    0x0      "$DIR/bootloader.bin" \
    0x8000   "$DIR/partition-table.bin" \
    0xd000   "$DIR/ota_data_initial.bin" \
    0x20000  "$DIR/xiaozhi.bin" \
    0x800000 "$DIR/generated_assets.bin"
echo "FLASH OK ($PORT) — ban main moi da nap."
