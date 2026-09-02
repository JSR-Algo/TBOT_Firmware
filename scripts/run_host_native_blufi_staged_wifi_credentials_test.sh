#!/usr/bin/env bash
set -euo pipefail
repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build_dir="$(mktemp -d)"
trap 'rm -rf "${build_dir}"' EXIT
compiler="${CXX:-$(command -v clang++ || command -v c++)}"
run() {
  local label="$1"; shift
  "${compiler}" -std=c++17 -pthread -Wall -Wextra -Werror -pedantic "$@" \
    -I"${repo_root}/main/boards/common" \
    "${repo_root}/tests/native/blufi_staged_wifi_credentials_host_test.cc" \
    -o "${build_dir}/${label}"
  "${build_dir}/${label}"
}
run normal
run asan_ubsan -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer
if printf '%s\n' 'int main() { return 0; }' | "${compiler}" -x c++ -std=c++17 \
    -pthread -fsanitize=thread - -o "${build_dir}/probe" >/dev/null 2>&1; then
  run tsan -O1 -g -fsanitize=thread -fno-omit-frame-pointer
fi
