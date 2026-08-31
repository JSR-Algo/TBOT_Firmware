#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build_dir="$(mktemp -d)"
trap 'rm -rf "${build_dir}"' EXIT

test_source="${repo_root}/tests/native/wifi_scan_recovery_executor_host_test.cc"
executor_source="${repo_root}/components/esp-wifi-connect/wifi_scan_recovery_executor.cc"
stub_include="${repo_root}/tests/native_stubs/wifi_scan_recovery_executor"
component_include="${repo_root}/components/esp-wifi-connect/include"

run_variant() {
  local compiler="$1"
  local label="$2"
  shift 2
  local binary="${build_dir}/${label}"

  "${compiler}" -std=c++17 -pthread -Wall -Wextra -Werror -pedantic \
    -I"${stub_include}" -I"${component_include}" "$@" \
    "${test_source}" "${executor_source}" -o "${binary}"
  "${binary}"
}

supports_flags() {
  local compiler="$1"
  local label="$2"
  shift 2
  local binary="${build_dir}/probe_${label}"

  printf '%s\n' 'int main() { return 0; }' | \
    "${compiler}" -x c++ -std=c++17 -pthread "$@" - -o "${binary}" \
      >/dev/null 2>&1 && "${binary}" >/dev/null 2>&1
}

compilers=()
if [[ -n "${CXX:-}" ]]; then
  compilers+=("${CXX}")
elif command -v clang++ >/dev/null 2>&1; then
  compilers+=("$(command -v clang++)")
elif command -v c++ >/dev/null 2>&1; then
  compilers+=("$(command -v c++)")
fi

if [[ ${#compilers[@]} -eq 0 ]]; then
  echo "no C++17 compiler available" >&2
  exit 1
fi

index=0
for compiler in "${compilers[@]}"; do
  run_variant "${compiler}" "compiler_${index}"

  asan_flags=(-O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer)
  if supports_flags "${compiler}" "asan_ubsan_${index}" "${asan_flags[@]}"; then
    run_variant "${compiler}" "asan_ubsan_${index}" "${asan_flags[@]}"
  fi

  tsan_flags=(-O1 -g -fsanitize=thread -fno-omit-frame-pointer)
  if supports_flags "${compiler}" "tsan_${index}" "${tsan_flags[@]}"; then
    run_variant "${compiler}" "tsan_${index}" "${tsan_flags[@]}"
  fi
  index=$((index + 1))
done
