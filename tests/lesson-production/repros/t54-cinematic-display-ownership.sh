#!/usr/bin/env bash
# repo: robot/TBOT-Firmware
set -euo pipefail

python3 - <<'PY'
from pathlib import Path

source = Path("main/lesson_handler.cc").read_text(encoding="utf-8")
start = source.index("if (cinematic_v3 || cinematic_v4 || cinematic_v5)")
end = source.index("const bool is_prepare", start)
cinematic = source[start:end]

assert "claim_cinematic_display" in cinematic, (
    "cinematic playback does not define lesson display ownership"
)
start_branch = cinematic.index("} else if (start_command)")
accepted = cinematic.index("if (response.accepted) {", start_branch)
accepted_end = cinematic.index("}", accepted)
assert "claim_cinematic_display();" in cinematic[accepted:accepted_end], (
    "accepted cinematic start does not hide the conversation face"
)

terminal = cinematic.index("if (response.accepted && terminal_command)")
terminal_end = cinematic.index("emit_cinematic_ack(response)", terminal)
assert "release_cinematic_display();" in cinematic[terminal:terminal_end], (
    "accepted cinematic stop/cancel does not restore the conversation face"
)

print("T54 cinematic display ownership: PASS")
PY
