#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
: "${ORIGINAL_SOURCE_PROBE:?Set ORIGINAL_SOURCE_PROBE to the pinned portable-probe directory}"
: "${ORIGINAL_SOURCE_LIBS:?Set ORIGINAL_SOURCE_LIBS to the pinned sanitizer libraries}"
BUILD_DIR="$(mktemp -d "${TMPDIR:-/tmp}/tbot-original-ffmpeg-allocator.XXXXXX")"
trap 'rm -rf "$BUILD_DIR"' EXIT
SRC="$ORIGINAL_SOURCE_PROBE/FFmpeg-db69d06eeeab4f46da15030a80d539efb4503ca8"
cp "$SRC/libavutil/mem.c" "$BUILD_DIR/mem.c"
if [[ "${ORIGINAL_ALLOCATOR_UNPATCHED:-0}" != 1 ]]; then
  patch --batch "$BUILD_DIR/mem.c" "$ROOT/third_party/ffmpeg/patches/0002-avutil-notify-early-allocation-refusal.patch"
fi
# Host config selects posix_memalign; the ESP32-S3 target config selects memalign.
for branch in posix_memalign memalign; do
  OUT="$BUILD_DIR/$branch"
  mkdir -p "$OUT"
  if [[ "$branch" == posix_memalign ]]; then posix=1; memalign=0; else posix=0; memalign=1; fi
  sed -e "s/^#define HAVE_POSIX_MEMALIGN .*/#define HAVE_POSIX_MEMALIGN $posix/" \
      -e "s/^#define HAVE_MEMALIGN .*/#define HAVE_MEMALIGN $memalign/" \
      "$ORIGINAL_SOURCE_LIBS/config.h" > "$OUT/config.h"
  grep -q "^#define HAVE_POSIX_MEMALIGN $posix$" "$OUT/config.h"
  grep -q "^#define HAVE_MEMALIGN $memalign$" "$OUT/config.h"
  "${CC:-clang}" -g -fsanitize=address,undefined -fno-omit-frame-pointer \
    -I"$OUT" -I"$ORIGINAL_SOURCE_LIBS" -I"$SRC" -I"$SRC/libavutil" \
    -DHAVE_AV_CONFIG_H -DBUILDING_avutil -DMALLOC_PREFIX=tbot_original_ \
    -DTBOT_FFMPEG_ALLOC_FAILURE -c "$BUILD_DIR/mem.c" -o "$OUT/mem.o"
  nm "$OUT/mem.o" | grep -Eq " U _?tbot_original_${branch}$"
  cp "$ORIGINAL_SOURCE_LIBS/libavutil/libavutil.a" "$OUT/libavutil.a"
  ar r "$OUT/libavutil.a" "$OUT/mem.o"
  "${CXX:-clang++}" -std=c++17 -g -Wall -Wextra -Werror \
    -fsanitize=address,undefined -fno-omit-frame-pointer -I"$ROOT/main" -I"$SRC" \
    "$ROOT/main/lesson_original_source_allocator.cc" \
    "$ROOT/tests/native/lesson_original_source_ffmpeg_allocator_test.cc" \
    "$OUT/libavutil.a" -lm -o "$OUT/test"
  echo "branch=$branch"
  ASAN_OPTIONS="${ASAN_OPTIONS:-detect_leaks=0}" UBSAN_OPTIONS=halt_on_error=1 "$OUT/test"
done
