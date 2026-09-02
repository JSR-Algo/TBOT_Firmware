#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build_dir="$(mktemp -d)"
trap 'rm -rf "${build_dir}"' EXIT
compiler="${CXX:-$(command -v clang++ || command -v c++)}"

run_variant() {
  local label="$1"
  shift
  "${compiler}" -std=c++17 -pthread -Wall -Wextra -Werror -pedantic \
    -Wno-sign-compare \
    -I"${repo_root}/tests/native_stubs/ssid_manager" \
    -I"${repo_root}/components/esp-wifi-connect/include" \
    "$@" \
    "${repo_root}/tests/native/ssid_manager_transaction_host_test.cc" \
    "${repo_root}/components/esp-wifi-connect/ssid_manager.cc" \
    -o "${build_dir}/${label}"
  "${build_dir}/${label}"
}

run_variant normal
run_variant asan_ubsan -O1 -g -fsanitize=address,undefined \
  -fno-omit-frame-pointer
if printf '%s\n' 'int main() { return 0; }' | "${compiler}" -x c++ -std=c++17 \
    -pthread -fsanitize=thread - -o "${build_dir}/probe" >/dev/null 2>&1; then
  run_variant tsan -O1 -g -fsanitize=thread -fno-omit-frame-pointer
fi
