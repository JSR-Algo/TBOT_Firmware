#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build_dir="$(mktemp -d)"
trap 'rm -rf "${build_dir}"' EXIT

"${CXX:-c++}" -std=c++17 -pthread -Wall -Wextra -Werror \
  "${repo_root}/tests/native/blufi_wifi_scan_controller_host_test.cc" \
  -o "${build_dir}/blufi_wifi_scan_controller_host_test"

"${build_dir}/blufi_wifi_scan_controller_host_test"
