#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
: "${ORIGINAL_SOURCE_PROBE:?Set ORIGINAL_SOURCE_PROBE to the pinned portable-probe directory}"
BUILD_DIR="$(mktemp -d "${TMPDIR:-/tmp}/tbot-original-zlib-allocator.XXXXXX")"
trap 'rm -rf "$BUILD_DIR"' EXIT
ZLIB="$ORIGINAL_SOURCE_PROBE/zlib-1.3.1"
# Same routing flags the target zlib build must use; the pinned tree is not modified.
ROUTING=(-DMY_ZCALLOC -Dzcalloc=tbot_original_zcalloc -Dzcfree=tbot_original_zcfree)
if [[ "${ORIGINAL_ALLOCATOR_UNPATCHED:-0}" == 1 ]]; then ROUTING=(); fi
objects=()
for source in adler32 compress crc32 deflate inffast inflate inftrees trees uncompr zutil; do
  "${CC:-clang}" -g -fsanitize=address,undefined -fno-omit-frame-pointer -DHAVE_HIDDEN \
    ${ROUTING[@]+"${ROUTING[@]}"} -I"$ZLIB" -c "$ZLIB/$source.c" -o "$BUILD_DIR/$source.o"
  objects+=("$BUILD_DIR/$source.o")
done
"${CXX:-clang++}" -std=c++17 -g -Wall -Wextra -Werror \
  -fsanitize=address,undefined -fno-omit-frame-pointer -I"$ROOT/main" -I"$ZLIB" \
  "$ROOT/main/lesson_original_source_allocator.cc" \
  "$ROOT/tests/native/lesson_original_source_zlib_allocator_test.cc" \
  "${objects[@]}" -o "$BUILD_DIR/test"
ASAN_OPTIONS="${ASAN_OPTIONS:-detect_leaks=0}" UBSAN_OPTIONS=halt_on_error=1 "$BUILD_DIR/test"
