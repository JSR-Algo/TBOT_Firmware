#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
: "${ORIGINAL_SOURCE_PROBE:?Set ORIGINAL_SOURCE_PROBE to the pinned R01 portable-probe directory}"
: "${ORIGINAL_SOURCE_FIXTURES:?Set ORIGINAL_SOURCE_FIXTURES to the generated original fixture manifest}"
# ORIGINAL_SOURCE_BUILD_DIR keeps a new owned build (e.g. for sharded fault runs).
if [[ -n "${ORIGINAL_SOURCE_BUILD_DIR:-}" ]]; then
  BUILD_DIR="$ORIGINAL_SOURCE_BUILD_DIR"
  mkdir "$BUILD_DIR"
else
  BUILD_DIR="$(mktemp -d "${TMPDIR:-/tmp}/tbot-original-source.XXXXXX")"
  trap 'rm -rf "${BUILD_DIR}"' EXIT
fi
SRC="$ORIGINAL_SOURCE_PROBE/FFmpeg-db69d06eeeab4f46da15030a80d539efb4503ca8"
ZLIB="$ORIGINAL_SOURCE_PROBE/zlib-1.3.1"
LIB="${ORIGINAL_SOURCE_LIBS:-$ORIGINAL_SOURCE_PROBE/host-sanitized}"
SAN=(-g -fsanitize=address,undefined -fno-omit-frame-pointer)
python3 "$ROOT/scripts/build_original_source_vp9_patch.py" --source "$SRC" --libraries "$LIB" --output "$BUILD_DIR/patched-vp9"
# Production routing: patched mem.c and pinned zlib reach the tbot_original_* owner.
cp "$SRC/libavutil/mem.c" "$BUILD_DIR/mem.c"
patch --batch "$BUILD_DIR/mem.c" "$ROOT/third_party/ffmpeg/patches/0002-avutil-notify-early-allocation-refusal.patch"
"${CC:-clang}" "${SAN[@]}" -I"$LIB" -I"$SRC" -I"$SRC/libavutil" \
 -DHAVE_AV_CONFIG_H -DBUILDING_avutil -DMALLOC_PREFIX=tbot_original_ -DTBOT_FFMPEG_ALLOC_FAILURE \
 -c "$BUILD_DIR/mem.c" -o "$BUILD_DIR/mem.o"
cp "$LIB/libavutil/libavutil.a" "$BUILD_DIR/libavutil.a"
ar r "$BUILD_DIR/libavutil.a" "$BUILD_DIR/mem.o"
zlib_objects=()
for source in adler32 compress crc32 deflate inffast inflate inftrees trees uncompr zutil; do
  "${CC:-clang}" "${SAN[@]}" -DHAVE_HIDDEN -DMY_ZCALLOC -Dzcalloc=tbot_original_zcalloc \
   -Dzcfree=tbot_original_zcfree -I"$ZLIB" -c "$ZLIB/$source.c" -o "$BUILD_DIR/z_$source.o"
  zlib_objects+=("$BUILD_DIR/z_$source.o")
done
"${CXX:-clang++}" -std=c++17 "${SAN[@]}" -Wall -Wextra -Werror \
 -Dfread=source_fread -Dav_read_frame=source_read_frame -Davcodec_receive_frame=source_receive_frame -Davcodec_send_packet=source_send_packet -I"$ROOT/main" -I"$SRC" -I"$LIB" \
 -c "$ROOT/main/lesson_original_source_session.cc" -o "$BUILD_DIR/session.o"
"${CXX:-clang++}" -std=c++17 "${SAN[@]}" -Wall -Wextra -Werror \
 -I"$ROOT/main" -I"$SRC" -I"$LIB" "$BUILD_DIR/session.o" \
 "$ROOT/main/lesson_original_source_allocator.cc" \
 "$ROOT/tests/native/lesson_original_source_fault_backend.cc" \
 "$ROOT/tests/native/lesson_original_source_session_test.cc" \
 "$LIB/libavformat/libavformat.a" "$BUILD_DIR/patched-vp9/libavcodec.a" "$BUILD_DIR/libavutil.a" \
 "${zlib_objects[@]}" -lm -o "$BUILD_DIR/test"
# The libc allocator must be unreachable from routed FFmpeg/zlib objects.
if nm "$BUILD_DIR/mem.o" "${zlib_objects[@]}" | grep -Eq " U _?(malloc|calloc|realloc|free|posix_memalign|memalign)$"; then
  echo "FAIL routed FFmpeg/zlib object references libc allocator" >&2; exit 1
fi
if [[ "${ORIGINAL_SOURCE_BUILD_ONLY:-0}" == 1 ]]; then echo "BUILT $BUILD_DIR/test"; exit 0; fi
ASAN_OPTIONS="${ASAN_OPTIONS:-detect_leaks=0}" UBSAN_OPTIONS=halt_on_error=1 "$BUILD_DIR/test" "$ORIGINAL_SOURCE_FIXTURES"
