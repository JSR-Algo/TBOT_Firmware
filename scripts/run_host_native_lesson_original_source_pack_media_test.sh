#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
: "${TMPDIR:?owned TMPDIR required}"
: "${CJSON_DIR:?qualified cJSON required}"
: "${ORIGINAL_SOURCE_PROBE:?Set ORIGINAL_SOURCE_PROBE to the pinned R01 portable-probe directory}"
: "${ORIGINAL_SOURCE_LIBS:?Set ORIGINAL_SOURCE_LIBS to the device-equivalent host FFmpeg build}"
: "${ORIGINAL_SOURCE_FIXTURES:?Set ORIGINAL_SOURCE_FIXTURES to the generated original fixture manifest}"
IDF_ROOT="${IDF_PATH:-${HOME}/esp/esp-idf}"
MBEDTLS_ROOT="${IDF_ROOT}/components/mbedtls/mbedtls"
BUILD_DIR="$(mktemp -d "${TMPDIR}/original-source-pack.XXXXXX")"
trap 'rm -rf "${BUILD_DIR}"' EXIT
SRC="$ORIGINAL_SOURCE_PROBE/FFmpeg-db69d06eeeab4f46da15030a80d539efb4503ca8"
ZLIB="$ORIGINAL_SOURCE_PROBE/zlib-1.3.1"
LIB="$ORIGINAL_SOURCE_LIBS"
SAN=(-g -fsanitize=address,undefined -fno-omit-frame-pointer)
python3 "$ROOT/scripts/build_original_source_vp9_patch.py" --source "$SRC" --libraries "$LIB" --output "$BUILD_DIR/patched-vp9" >/dev/null
# Production routing: patched mem.c and pinned zlib reach the tbot_original_* owner.
cp "$SRC/libavutil/mem.c" "$BUILD_DIR/mem.c"
patch --batch --quiet "$BUILD_DIR/mem.c" "$ROOT/third_party/ffmpeg/patches/0002-avutil-notify-early-allocation-refusal.patch"
"${CC:-clang}" "${SAN[@]}" -I"$LIB" -I"$SRC" -I"$SRC/libavutil" \
 -DHAVE_AV_CONFIG_H -DBUILDING_avutil -DMALLOC_PREFIX=tbot_original_ -DTBOT_FFMPEG_ALLOC_FAILURE \
 -c "$BUILD_DIR/mem.c" -o "$BUILD_DIR/mem.o"
cp "$LIB/libavutil/libavutil.a" "$BUILD_DIR/libavutil.a"
ar r "$BUILD_DIR/libavutil.a" "$BUILD_DIR/mem.o" 2>/dev/null
zlib_objects=()
for source in adler32 compress crc32 deflate inffast inflate inftrees trees uncompr zutil; do
  "${CC:-clang}" "${SAN[@]}" -DHAVE_HIDDEN -DMY_ZCALLOC -Dzcalloc=tbot_original_zcalloc \
   -Dzcfree=tbot_original_zcfree -I"$ZLIB" -c "$ZLIB/$source.c" -o "$BUILD_DIR/z_$source.o"
  zlib_objects+=("$BUILD_DIR/z_$source.o")
done
printf '#define MBEDTLS_SHA256_C\n' > "${BUILD_DIR}/sha256_config.h"
MB=(-DMBEDTLS_CONFIG_FILE='"sha256_config.h"' -I"${BUILD_DIR}" -I"${MBEDTLS_ROOT}/include" -I"${MBEDTLS_ROOT}/library")
for source in sha256 platform_util; do
  "${CC:-clang}" -std=c99 "${SAN[@]}" "${MB[@]}" -c "${MBEDTLS_ROOT}/library/${source}.c" -o "${BUILD_DIR}/mb_${source}.o"
done
"${CXX:-clang++}" -std=c++17 -pthread "${SAN[@]}" -Wall -Wextra -Werror "${MB[@]}" \
 -I"$ROOT/main" -I"${CJSON_DIR}" -I"$SRC" -I"$LIB" \
 "$ROOT/main/lesson_original_source_pack_media.cc" "$ROOT/main/lesson_original_source_session.cc" \
 "$ROOT/main/lesson_original_source_allocator.cc" "$ROOT/main/lesson_asset_sync_path_policy.cc" \
 "$ROOT/main/lesson_asset_storage_coordinator.cc" "$ROOT/main/sd_fat_session_guard.cc" \
 "$ROOT/main/lesson_asset_cache_evict.cc" "$ROOT/main/lesson_asset_retained_selection.cc" \
 "$ROOT/tests/native/lesson_original_source_pack_media_host_test.cc" \
 "${BUILD_DIR}/mb_sha256.o" "${BUILD_DIR}/mb_platform_util.o" \
 "$LIB/libavformat/libavformat.a" "$BUILD_DIR/patched-vp9/libavcodec.a" "$BUILD_DIR/libavutil.a" \
 "${zlib_objects[@]}" -lm -o "$BUILD_DIR/test"
ASAN_OPTIONS="${ASAN_OPTIONS:-detect_leaks=0}" UBSAN_OPTIONS=halt_on_error=1 "$BUILD_DIR/test" "$ORIGINAL_SOURCE_FIXTURES" "$BUILD_DIR/work"
