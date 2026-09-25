#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "$0")/.." && pwd)"
build_dir="$(mktemp -d)"
trap 'rm -rf "$build_dir"' EXIT

c++ -std=c++17 -Wall -Wextra -Werror \
  -I"$repo_root/main" \
  "$repo_root/tests/native/backend_recovery_window_test.cc" \
  -o "$build_dir/backend_recovery_window_test"

"$build_dir/backend_recovery_window_test"
