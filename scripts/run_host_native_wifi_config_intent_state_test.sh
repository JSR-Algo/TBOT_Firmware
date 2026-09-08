#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build_dir="$(mktemp -d)"
trap 'rm -rf "${build_dir}"' EXIT
compiler="${CXX:-$(command -v clang++ || command -v c++)}"

"${compiler}" -std=c++17 -pthread -Wall -Wextra -Werror -pedantic \
  -I"${repo_root}/main/boards/common" \
  "${repo_root}/tests/native/wifi_config_intent_state_host_test.cc" \
  -o "${build_dir}/wifi_config_intent_state_host_test"
"${build_dir}/wifi_config_intent_state_host_test"
