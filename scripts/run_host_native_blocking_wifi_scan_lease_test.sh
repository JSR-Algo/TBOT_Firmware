#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build_dir="$(mktemp -d)"
trap 'rm -rf "${build_dir}"' EXIT
compiler="${CXX:-$(command -v clang++ || command -v c++)}"
source_file="${repo_root}/tests/native/blocking_wifi_scan_lease_host_test.cc"

run_variant() {
  local label="$1"
  shift
  "${compiler}" -std=c++17 -pthread -Wall -Wextra -Werror -pedantic \
    -I"${repo_root}/components/esp-wifi-connect/include" \
    "$@" "${source_file}" -o "${build_dir}/${label}"
  "${build_dir}/${label}"
}

supports_flags() {
  local label="$1"
  shift
  printf '%s\n' 'int main() { return 0; }' | \
    "${compiler}" -x c++ -std=c++17 -pthread "$@" - \
      -o "${build_dir}/probe_${label}" >/dev/null 2>&1
}

run_variant default
asan_flags=(-O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer)
if supports_flags asan_ubsan "${asan_flags[@]}"; then
  run_variant asan_ubsan "${asan_flags[@]}"
fi
tsan_flags=(-O1 -g -fsanitize=thread -fno-omit-frame-pointer)
if supports_flags tsan "${tsan_flags[@]}"; then
  run_variant tsan "${tsan_flags[@]}"
fi
