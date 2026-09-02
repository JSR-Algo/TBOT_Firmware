#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build_dir="$(mktemp -d)"
trap 'rm -rf "${build_dir}"' EXIT

test_source="${repo_root}/tests/native/wifi_scan_lease_coordinator_host_test.cc"

run_variant() {
  local compiler="$1"
  local label="$2"
  shift 2
  local binary="${build_dir}/${label}"

  "${compiler}" -std=c++17 -pthread -Wall -Wextra -Werror -pedantic \
    "$@" "${test_source}" -o "${binary}"
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
else
  if command -v clang++ >/dev/null 2>&1; then
    compilers+=("$(command -v clang++)")
  elif command -v c++ >/dev/null 2>&1; then
    compilers+=("$(command -v c++)")
  fi

  for candidate in g++-15 g++-14 g++-13 g++-12 g++; do
    if command -v "${candidate}" >/dev/null 2>&1 && \
        ! "${candidate}" --version 2>&1 | head -n 1 | grep -qi clang; then
      compilers+=("$(command -v "${candidate}")")
      break
    fi
  done
fi

if [[ ${#compilers[@]} -eq 0 ]]; then
  echo "no C++17 compiler available" >&2
  exit 1
fi

index=0
for compiler in "${compilers[@]}"; do
  compiler_label="compiler_${index}"
  run_variant "${compiler}" "${compiler_label}"

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
