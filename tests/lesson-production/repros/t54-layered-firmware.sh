#!/usr/bin/env bash
# repo: robot/TBOT-Firmware
set -euo pipefail

test -f main/lesson_layered_cinematic_renderer.cc
bash scripts/run_host_native_lesson_layered_cinematic_renderer_test.sh
bash scripts/run_host_native_lesson_handler_test.sh
  python3 -m pytest tests/test_lesson_dispatch_backward_compat.py -k 'v5 or layered' -q
