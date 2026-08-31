#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build_dir="$(mktemp -d)"
trap 'rm -rf "${build_dir}"' EXIT

compiler="${CXX:-}"
if [[ -z "${compiler}" ]]; then
  compiler="$(command -v clang++ || command -v c++)"
fi

"${compiler}" -std=c++17 -pthread -Wall -Wextra -Werror -pedantic \
  -I"${repo_root}/tests/native_stubs/default_event_loop_barrier" \
  -I"${repo_root}/components/esp-wifi-connect/include" \
  "${repo_root}/tests/native/default_event_loop_barrier_host_test.cc" \
  -o "${build_dir}/default_event_loop_barrier_host_test"
"${build_dir}/default_event_loop_barrier_host_test"
