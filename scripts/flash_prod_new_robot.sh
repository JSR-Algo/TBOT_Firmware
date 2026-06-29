#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-$ROOT/build-prod}"
BAUD="${BAUD:-460800}"
WAIT_SECONDS="${WAIT_SECONDS:-120}"
PORT="${PORT:-}"
MIN_ARTIFACT_BYTES="${MIN_ARTIFACT_BYTES:-4096}"

if [[ -f "$HOME/esp/esp-idf/export.sh" ]]; then
  # shellcheck disable=SC1091
  source "$HOME/esp/esp-idf/export.sh" >/tmp/tbot_idf_export.log
fi

required=(
  "$BUILD_DIR/bootloader/bootloader.bin"
  "$BUILD_DIR/partition_table/partition-table.bin"
  "$BUILD_DIR/ota_data_initial.bin"
  "$BUILD_DIR/xiaozhi.bin"
  "$BUILD_DIR/generated_assets.bin"
)
for file in "${required[@]}"; do
  [[ -s "$file" ]] || { echo "missing build artifact: $file" >&2; exit 2; }
  min_bytes="$MIN_ARTIFACT_BYTES"
  case "$(basename "$file")" in
    partition-table.bin) min_bytes=1024 ;;
  esac
  if size=$(stat -f%z "$file" 2>/dev/null); then
    :
  else
    size=$(stat -c%s "$file")
  fi
  if (( size < min_bytes )); then
    echo "artifact too small: $file (${size} bytes, minimum ${min_bytes})" >&2
    exit 2
  fi
done

detect_port() {
  ls /dev/cu.usbmodem* /dev/cu.usbserial* /dev/cu.wchusb* /dev/cu.SLAB_USBtoUART* /dev/cu.OGVN* 2>/dev/null | head -1 || true
}

deadline=$((SECONDS + WAIT_SECONDS))
while [[ -z "$PORT" ]]; do
  PORT="$(detect_port)"
  [[ -n "$PORT" ]] && break
  if (( SECONDS >= deadline )); then
    echo "NO_ESP_USB_PORT after ${WAIT_SECONDS}s" >&2
    echo "Current serial ports:" >&2
    ls -1 /dev/cu.* 2>/dev/null >&2 || true
    exit 3
  fi
  sleep 2
done

echo "Using port: $PORT"
python3 -m esptool --chip esp32s3 --port "$PORT" --after no_reset chip_id
python3 -m esptool \
  --chip esp32s3 \
  -b "$BAUD" \
  --before default_reset \
  --after hard_reset \
  write_flash \
  --flash_mode dio \
  --flash_size 16MB \
  --flash_freq 80m \
  0x0 "$BUILD_DIR/bootloader/bootloader.bin" \
  0x8000 "$BUILD_DIR/partition_table/partition-table.bin" \
  0xd000 "$BUILD_DIR/ota_data_initial.bin" \
  0x20000 "$BUILD_DIR/xiaozhi.bin" \
  0x800000 "$BUILD_DIR/generated_assets.bin"

echo "FLASH_OK $PORT"
