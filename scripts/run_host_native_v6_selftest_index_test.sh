#!/usr/bin/env bash
# Host test (ASan/UBSan) of the renderer v6 self-test media index parser.
# Usage: run_host_native_v6_selftest_index_test.sh [packed image]
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="${TMPDIR:-/tmp}/v6-selftest-index-test"
clang++ -std=c++17 -g -fsanitize=address,undefined -fno-omit-frame-pointer -Wall -Wextra -Werror \
  -I"$ROOT/main" "$ROOT/main/lesson_original_source_device_selftest.cc" \
  "$ROOT/tests/native/lesson_original_source_device_selftest_index_test.cc" -o "$OUT"
UBSAN_OPTIONS=halt_on_error=1 "$OUT" "$@"
