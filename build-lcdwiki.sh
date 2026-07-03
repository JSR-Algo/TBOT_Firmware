#!/usr/bin/env bash
#
# build-lcdwiki.sh — Fleet-safe build + flash for the LCDWiki ES3C35P robot board.
#
# WHY THIS EXISTS (the black-screen trap):
#   The committed sdkconfig.defaults has NO CONFIG_BOARD_TYPE_* line, so the
#   Kconfig default (main/Kconfig.projbuild:138) wins:
#       default BOARD_TYPE_BREAD_COMPACT_WIFI   <- no LCD driver
#   The real robot board line lives ONLY in sdkconfig.defaults.local:
#       CONFIG_BOARD_TYPE_LCDWIKI_ES3C35P=y   (main/Kconfig.projbuild:366)
#   The installed ESP-IDF (v5.5.4) does NOT auto-load "sdkconfig.defaults.local".
#   tools/cmake/kconfig.cmake:175-177 only auto-appends the ".<target>" suffix
#   (e.g. sdkconfig.defaults.esp32s3) to each SDKCONFIG_DEFAULTS entry — never
#   ".local". So a plain `idf.py build` compiles the bread-board (no LCD) image
#   -> the robot boots, WiFi works, but the screen is BLACK. The only reliable
#   fix is to EXPLICITLY list sdkconfig.defaults.local in SDKCONFIG_DEFAULTS,
#   then HARD-VERIFY the board line landed in sdkconfig before flashing. This
#   script does exactly that, per unit, repeatably.
#
#   It deliberately does NOT edit the committed sdkconfig.defaults: adding the
#   LCDWiki line there would override the Kconfig BOARD_TYPE choice default and
#   break the freenove/bread variants (and any other board) that rely on it.
#   The fix stays in the build chain, not in the shared default.
#
#   NOTE (OTA "already latest 2.2.x" trap): PROJECT_VER is hardcoded to "2.2.30"
#   in CMakeLists.txt, so EVERY image — LCD or black-screen bread-board — reports
#   the same version to OTA. ota.cc IsNewVersionAvailable() then returns false
#   ("Current is the latest version"), so OTA will NOT correct a unit that was
#   flashed with the wrong board. You cannot tell which board image is running
#   from the version alone. => For fleet flashing, trust the build-time
#   grep-CONFIG_BOARD_TYPE_LCDWIKI_ES3C35P gate in THIS script, not the device's
#   reported version, to know the LCD image actually shipped.
#
# USAGE:
#   ./build-lcdwiki.sh                      # build + flash to /dev/cu.usbmodem101
#   ./build-lcdwiki.sh /dev/cu.usbmodem1101 # build + flash to a specific port
#   ./build-lcdwiki.sh --no-flash           # build + verify only, do not flash
#
# Idempotent and safe to run once per robot: plug unit -> run -> next unit.

set -euo pipefail

# --- locate project root (the dir this script lives in) ---
PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$PROJECT_DIR"

# --- args: optional [PORT] positional + optional --no-flash flag, any order ---
PORT="/dev/cu.usbmodem101"
DO_FLASH=1
for arg in "$@"; do
    case "$arg" in
        --no-flash) DO_FLASH=0 ;;
        -*)         echo "Unknown flag: $arg" >&2; exit 2 ;;
        *)          PORT="$arg" ;;
    esac
done

# --- 1. source esp-idf ---
IDF_EXPORT="${IDF_EXPORT:-$HOME/esp/esp-idf/export.sh}"
if [ ! -f "$IDF_EXPORT" ]; then
    echo "ERROR: ESP-IDF export.sh not found at $IDF_EXPORT" >&2
    echo "       Set IDF_EXPORT=/path/to/esp-idf/export.sh and re-run." >&2
    exit 1
fi
# 'source' touches unbound vars internally; relax nounset just for the source.
set +u
# shellcheck disable=SC1090
source "$IDF_EXPORT"
set -u

# --- 2. export SDKCONFIG_DEFAULTS chain (base -> s3 tuning -> board overlay) ---
#     Order matters: later files override earlier. sdkconfig.defaults.local is
#     LAST so CONFIG_BOARD_TYPE_LCDWIKI_ES3C35P=y wins. We list .esp32s3
#     explicitly rather than relying on ESP-IDF's implicit ".<target>" append,
#     so the resolution order is deterministic and self-documenting.
export SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.esp32s3;sdkconfig.defaults.local"

# Guard: every file in the chain must exist, else ESP-IDF FATAL_ERRORs mid-build.
IFS=';' read -r -a _defaults_arr <<< "$SDKCONFIG_DEFAULTS"
for f in "${_defaults_arr[@]}"; do
    if [ ! -f "$PROJECT_DIR/$f" ]; then
        echo "ERROR: SDKCONFIG_DEFAULTS entry '$f' not found in $PROJECT_DIR" >&2
        exit 1
    fi
done

# --- 3. remove stale sdkconfig + pin target so the board line is re-derived ---
#     A stale sdkconfig or build dir (e.g. left over from a freenove/bread build
#     or a failed toolchain switch) would otherwise be reused and silently keep
#     the wrong board or stale compiler paths. set-target regenerates sdkconfig
#     from the SDKCONFIG_DEFAULTS chain above.
rm -f sdkconfig
rm -rf "$PROJECT_DIR/build"
idf.py set-target esp32s3

# --- 4. HARD GATE: abort unless the LCDWiki board line actually landed ---
if ! grep -q '^CONFIG_BOARD_TYPE_LCDWIKI_ES3C35P=y$' sdkconfig; then
    echo "============================================================" >&2
    echo "ABORT: CONFIG_BOARD_TYPE_LCDWIKI_ES3C35P=y NOT in sdkconfig." >&2
    echo "       The build would produce a BLACK-SCREEN (bread-board)" >&2
    echo "       image. Refusing to build/flash. Check that"            >&2
    echo "       sdkconfig.defaults.local still has the LCDWiki line"   >&2
    echo "       and that SDKCONFIG_DEFAULTS includes it."              >&2
    echo "============================================================" >&2
    exit 1
fi
echo "OK: LCDWiki ES3C35P board confirmed in sdkconfig."

# --- 5. build ---
if ! idf.py build; then
    echo "WARN: idf.py build failed; retrying once (fresh build dependency-dir race)." >&2
    idf.py build
fi

# --- 5b. report app binary size (source of truth: the flashed .bin) ---
APP_BIN="$PROJECT_DIR/build/xiaozhi.bin"   # project(xiaozhi) in CMakeLists.txt
if [ -f "$APP_BIN" ]; then
    APP_BYTES="$(wc -c < "$APP_BIN" | tr -d ' ')"
    APP_KIB=$(( APP_BYTES / 1024 ))
    echo "BUILD OK: app image $APP_BIN = ${APP_BYTES} bytes (~${APP_KIB} KiB)."
else
    echo "WARN: expected app binary $APP_BIN not found after build." >&2
fi
# Detailed component breakdown (best-effort; never fail the build over it).
idf.py size || true

# --- 6. flash (unless --no-flash) ---
if [ "$DO_FLASH" -eq 1 ]; then
    if [ ! -e "$PORT" ]; then
        echo "ERROR: serial port '$PORT' not found. Plug in the robot, or pass the" >&2
        echo "       correct port: ./build-lcdwiki.sh /dev/cu.usbmodemXXXX"          >&2
        echo "       (list with: ls /dev/cu.usbmodem* /dev/cu.wch*)"                 >&2
        exit 1
    fi
    echo "Flashing LCDWiki image to $PORT ..."
    idf.py -p "$PORT" flash
    echo "FLASH OK for $PORT"
else
    echo "DONE: built + verified LCDWiki image (skipped flash; --no-flash)."
fi
