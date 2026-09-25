#!/usr/bin/env bash
# repo: robot/TBOT-Firmware
set -euo pipefail

python3 - <<'PY'
from pathlib import Path
import re

source = Path("main/lesson_handler.cc").read_text(encoding="utf-8")
start = source.index("const bool cinematic_v3 =")
end = source.index("const bool is_prepare", start)
cinematic = source[start:end]
normalized = re.sub(r"\s+", " ", cinematic)
assert "if ((cinematic_v3 && !renderer_v3_standard_frame) || cinematic_v4 || cinematic_v5)" in normalized, (
    "cinematic ownership must retain v3 standard-frame routing and include v4/v5"
)

def block(text, marker):
    begin = text.index("{", text.index(marker))
    depth = 1
    end = begin + 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[begin:end]

claim = block(cinematic, "auto claim_cinematic_display")
release = block(cinematic, "auto release_cinematic_display")
assert "ScheduleChatLesson(context," in claim and "SetLessonMode(true)" in claim
assert "ScheduleChatLesson(context," in release and "SetLessonMode(false)" in release
for layer in ("Background", "Object", "RobotOverlay"):
    assert f"SetLesson{layer}(nullptr)" in release, "terminal clears every lesson layer"
start_branch = cinematic.index("} else if (start_command)")
accepted = block(cinematic[start_branch:], "if (response.accepted && !g_session.cinematic_runtime_failed)")
assert "claim_cinematic_display();" in accepted, (
    "accepted cinematic start does not hide the conversation face"
)

terminal = block(cinematic, "if (response.accepted && terminal_command)")
assert "release_cinematic_display();" in terminal, (
    "accepted cinematic stop/cancel does not restore the conversation face"
)
assert "DiscardSession()" in terminal and "EndLessonSession(" in terminal
assert cinematic.index("emit_cinematic_ack(response);") < cinematic.index(
    "if (reset_cinematic_session_after_ack) g_session = LessonSession{};"
), "terminal identity survives until the ACK is built"

print("T54 cinematic display ownership: PASS")
PY
