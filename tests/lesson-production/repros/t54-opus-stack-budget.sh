#!/usr/bin/env bash
# repo: robot/TBOT-Firmware
set -euo pipefail

python3 - <<'PY'
from pathlib import Path

header = Path("main/audio/audio_service.h").read_text(encoding="utf-8")
source = Path("main/audio/audio_service.cc").read_text(encoding="utf-8")

assert "kOpusCodecTaskStackBytes = 12 * 1024" in header
assert "kOpusCodecTaskStackBytes" in source
assert "2048 * 12" not in source
PY
