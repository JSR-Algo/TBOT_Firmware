#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
: "${ORIGINAL_SOURCE_PROBE:?Set ORIGINAL_SOURCE_PROBE to the pinned R01 portable-probe directory}"
: "${ORIGINAL_SOURCE_FIXTURES:?Set ORIGINAL_SOURCE_FIXTURES to the generated original fixture manifest}"
BUILD_DIR="$(mktemp -d "${TMPDIR:-/tmp}/tbot-original-source.XXXXXX")"
trap 'rm -rf "${BUILD_DIR}"' EXIT
SRC="$ORIGINAL_SOURCE_PROBE/FFmpeg-db69d06eeeab4f46da15030a80d539efb4503ca8"
LIB="${ORIGINAL_SOURCE_LIBS:-$ORIGINAL_SOURCE_PROBE/host-sanitized}"
python3 "$ROOT/scripts/build_original_source_vp9_patch.py" --source "$SRC" --libraries "$LIB" --output "$BUILD_DIR/patched-vp9"
"${CC:-clang}" -g -fsanitize=address,undefined -fno-omit-frame-pointer -I"$LIB" -I"$SRC" \
 -DHAVE_AV_CONFIG_H -DBUILDING_avutil -DMALLOC_PREFIX=source_ -c "$SRC/libavutil/mem.c" -o "$BUILD_DIR/mem.o"
"${CC:-clang}" -g -fsanitize=address,undefined -fno-omit-frame-pointer \
 -c "$ROOT/tests/native/lesson_original_source_allocator.c" -o "$BUILD_DIR/allocator.o"
cp "$LIB/libavutil/libavutil.a" "$BUILD_DIR/libavutil.a"
ar r "$BUILD_DIR/libavutil.a" "$BUILD_DIR/mem.o"
"${CXX:-clang++}" -std=c++17 -g -Wall -Wextra -Werror -fsanitize=address,undefined -fno-omit-frame-pointer \
 -Dfread=source_fread -Dav_read_frame=source_read_frame -Davcodec_receive_frame=source_receive_frame -Davcodec_send_packet=source_send_packet -I"$ROOT/main" -I"$SRC" -I"$LIB" \
 -c "$ROOT/main/lesson_original_source_session.cc" -o "$BUILD_DIR/session.o"
"${CXX:-clang++}" -std=c++17 -g -Wall -Wextra -Werror -fsanitize=address,undefined -fno-omit-frame-pointer \
 -I"$ROOT/main" -I"$SRC" -I"$LIB" "$BUILD_DIR/session.o" \
 "$ROOT/tests/native/lesson_original_source_session_test.cc" \
 "$LIB/libavformat/libavformat.a" "$BUILD_DIR/patched-vp9/libavcodec.a" "$BUILD_DIR/libavutil.a" "$BUILD_DIR/allocator.o" \
 -lz -lm -o "$BUILD_DIR/test"
ASAN_OPTIONS="${ASAN_OPTIONS:-detect_leaks=0}" UBSAN_OPTIONS=halt_on_error=1 "$BUILD_DIR/test" "$ORIGINAL_SOURCE_FIXTURES"
