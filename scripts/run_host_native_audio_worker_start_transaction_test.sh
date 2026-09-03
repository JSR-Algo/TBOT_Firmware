#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build_dir="$(mktemp -d "${TMPDIR:-/tmp}/tbot-audio-rearm.XXXXXX")"
trap 'rm -rf "${build_dir}"' EXIT

compiler="${CXX:-$(command -v clang++ || command -v c++)}"
source_file="${repo_root}/tests/native/audio_worker_start_transaction_test.cc"

run_variant() {
  local label="$1"
  shift
  "${compiler}" -std=c++17 -pthread -Wall -Wextra -Werror "$@" \
    -I"${repo_root}/main" "${source_file}" -o "${build_dir}/${label}"
  "${build_dir}/${label}"
}

run_variant normal
run_variant asan_ubsan -O1 -g -fsanitize=address,undefined \
  -fno-omit-frame-pointer
if printf '%s\n' 'int main() { return 0; }' | "${compiler}" -x c++ \
    -std=c++17 -pthread -fsanitize=thread - -o "${build_dir}/tsan_probe" \
    >/dev/null 2>&1; then
  run_variant tsan -O1 -g -fsanitize=thread -fno-omit-frame-pointer
fi
