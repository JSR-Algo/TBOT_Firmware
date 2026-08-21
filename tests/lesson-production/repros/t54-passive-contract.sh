#!/usr/bin/env bash
# repo: robot/TBOT-Firmware
# F-T54-18 — passive liveness assertions must cover the semantic block rather
# than an arbitrary character window around one call.
set -uo pipefail

ROOT="${T54_FIRMWARE_REPO_ROOT:-$(pwd)}"
[ -f "$ROOT/tests/test_lesson_passive_websocket_contract.py" ] || {
  echo "FATAL: firmware passive websocket contract not found under $ROOT"
  exit 2
}

cd "$ROOT" || exit 2
exec python3 -m pytest tests/test_lesson_passive_websocket_contract.py -q
