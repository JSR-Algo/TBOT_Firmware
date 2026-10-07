#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
: "${TMPDIR:?owned TMPDIR required}"
: "${CJSON_DIR:?qualified cJSON required}"
: "${ORIGINAL_SOURCE_PROBE:?Set ORIGINAL_SOURCE_PROBE to the pinned R01 portable-probe directory}"
: "${ORIGINAL_SOURCE_LIBS:?Set ORIGINAL_SOURCE_LIBS to the device-equivalent host FFmpeg build}"
: "${ORIGINAL_SOURCE_FRAME_VECTORS:?canonical backend tvideo-journey-frame-state.v1 vectors (cue plan) required}"
: "${ORIGINAL_SOURCE_V6_SCENE:?Set ORIGINAL_SOURCE_V6_SCENE to the Farm scene.original-source.v1 document}"
: "${ORIGINAL_SOURCE_V6_MEDIA:?Set ORIGINAL_SOURCE_V6_MEDIA to the directory of its originals, named <assetVersionId>.<ext>}"
IDF_ROOT="${IDF_PATH:-${HOME}/esp/esp-idf}"
MBEDTLS_ROOT="${IDF_ROOT}/components/mbedtls/mbedtls"
BUILD_DIR="$(mktemp -d "${TMPDIR}/original-source-fragmentation.XXXXXX")"
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
# The robot's PSRAM allocator: ESP-IDF TLSF multi_heap. Darwin has no symbol aliases, so the
# copy drops the public aliases and the test calls the *_impl entry points. TLSF assumes the
# 32-bit target's header alignment; the 64-bit host build only skips that UBSan check.
HEAP_ROOT="${IDF_ROOT}/components/heap"
sed -E 's/__attribute__\(\(alias\("[a-z_]+"\)\)\)//' "${HEAP_ROOT}/multi_heap.c" > "${BUILD_DIR}/multi_heap.c"
HEAP=(-I"${HEAP_ROOT}" -I"${HEAP_ROOT}/include" -I"${HEAP_ROOT}/tlsf" -I"${HEAP_ROOT}/tlsf/include")
for source in "${BUILD_DIR}/multi_heap.c" "${HEAP_ROOT}/tlsf/tlsf.c"; do
  "${CC:-clang}" -std=gnu11 "${SAN[@]}" -fno-sanitize=alignment "${HEAP[@]}" -DCONFIG_LOG_DEFAULT_LEVEL \
   -c "$source" -o "${BUILD_DIR}/heap_$(basename "$source" .c).o"
done
# Vendored cJSON uses sprintf, deprecated by the macOS SDK; our sources keep -Werror.
"${CC:-clang}" -std=c99 "${SAN[@]}" -Wall -Wextra -Wno-deprecated-declarations -I"${CJSON_DIR}" \
 -c "${CJSON_DIR}/cJSON.c" -o "${BUILD_DIR}/cJSON.o"
"${CXX:-clang++}" -std=c++17 -ffp-contract=off -pthread "${SAN[@]}" -Wall -Wextra -Werror "${MB[@]}" \
 -I"$ROOT/main" -I"${CJSON_DIR}" -I"$SRC" -I"$LIB" "${HEAP[@]}" \
 "$ROOT/main/lesson_original_source_session.cc" "$ROOT/main/lesson_original_source_allocator.cc" \
 "$ROOT/main/lesson_original_source_pack_media.cc" "$ROOT/main/lesson_original_source_player.cc" \
 "$ROOT/main/lesson_original_source_scene_controller.cc" "$ROOT/main/lesson_original_source_contract.cc" \
 "$ROOT/main/lesson_original_source_compositor.cc" "$ROOT/main/lesson_tvideo_frame_state.cc" \
 "$ROOT/main/lesson_tvideo_painter.cc" "$ROOT/main/lesson_tvideo_raster_canvas.cc" \
 "$ROOT/main/lesson_asset_sync_path_policy.cc" "$ROOT/main/lesson_asset_storage_coordinator.cc" \
 "$ROOT/main/sd_fat_session_guard.cc" "$ROOT/main/lesson_asset_cache_evict.cc" \
 "$ROOT/main/lesson_asset_retained_selection.cc" \
 "$ROOT/tests/native/lesson_original_source_player_fragmentation_test.cc" \
 "${BUILD_DIR}/cJSON.o" "${BUILD_DIR}/mb_sha256.o" "${BUILD_DIR}/mb_platform_util.o" \
 "${BUILD_DIR}/heap_multi_heap.o" "${BUILD_DIR}/heap_tlsf.o" \
 "$LIB/libavformat/libavformat.a" "$BUILD_DIR/patched-vp9/libavcodec.a" "$BUILD_DIR/libavutil.a" \
 "${zlib_objects[@]}" -lm -o "$BUILD_DIR/test"
# SD-layout pack: <root>/<cacheKey>/<percent-encoded key>; the scene pins every original.
KEY="farm-original/v1-$(printf 'a%.0s' $(seq 64))"
PACK="$BUILD_DIR/pack/$KEY"
mkdir -p "$PACK"
cp "$ORIGINAL_SOURCE_V6_SCENE" "$PACK/scene.original-source.v1"
for original in "$ORIGINAL_SOURCE_V6_MEDIA"/*; do
  id="$(basename "$original")"
  cp -L "$original" "$PACK/original.${id%.*}"
done
SHA="$(shasum -a 256 "$PACK/scene.original-source.v1" | cut -d' ' -f1)"
BYTES="$(wc -c < "$PACK/scene.original-source.v1" | tr -d ' ')"
CUES=()
while read -r cue duration; do CUES+=("$cue" "$duration"); done < <(python3 -c '
import json, sys
for cue in json.load(open(sys.argv[1]))["cues"]: print(cue["cueId"], cue["durationMs"])' "$ORIGINAL_SOURCE_FRAME_VECTORS")
# Seeds of the foreign PSRAM owner; each runs the plan with and without the production chain.
ASAN_OPTIONS="${ASAN_OPTIONS:-detect_leaks=0}" UBSAN_OPTIONS=halt_on_error=1 \
 "$BUILD_DIR/test" "$BUILD_DIR/pack" "$KEY" "$SHA" "$BYTES" "${ORIGINAL_SOURCE_FRAGMENTATION_SEEDS:-0x0B19,0x51ED,0x7A3C,0x1234}" "${CUES[@]}"
