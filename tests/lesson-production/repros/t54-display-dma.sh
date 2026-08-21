#!/usr/bin/env bash
# repo: robot/TBOT-Firmware
# F-T54-19 — LCD QSPI must keep the measured bounded DMA transfer budget.
set -uo pipefail

ROOT="${T54_FIRMWARE_REPO_ROOT:-$(pwd)}"
[ -f "$ROOT/tests/test_lcdwiki_es3c35p_board.py" ] || {
  echo "FATAL: LCDWiki board contract not found under $ROOT"
  exit 2
}

cd "$ROOT" || exit 2
exec python3 -m pytest tests/test_lcdwiki_es3c35p_board.py -q
