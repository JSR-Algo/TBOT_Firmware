#!/usr/bin/env bash
# repo: robot/TBOT-Firmware
set -euo pipefail

python3 - <<'PY'
from pathlib import Path

header = Path("main/audio/audio_service.h").read_text(encoding="utf-8")
source = Path("main/audio/audio_service.cc").read_text(encoding="utf-8")

assert "20 KiB task left only 428 bytes free" in header
assert "~19.6 KiB peak" in header
assert "about 8 KiB headroom" in header
assert "kOpusCodecTaskStackBytes = 28 * 1024" in header
assert '"opus_codec", kOpusCodecTaskStackBytes, this' in source
assert "kOpusCodecTaskStackBytes / sizeof(StackType_t)" not in source
assert "kOpusCodecTaskStackBytes / sizeof(StackType_t::value_type)" not in source
assert "2048 * 12" not in source
PY
