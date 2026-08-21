#!/usr/bin/env bash
# repo: robot/TBOT-Firmware
# regate: skip
set -euo pipefail

python3 - <<'PY'
from pathlib import Path

path = Path("tests/native/lesson_handler_host_test.cc")
text = path.read_text()
if "renderer-v2 motion waits for successful visual application" not in text:
    before = '''    Handle(V2VisualFrameWithStepId(3, "valid-step", 2));
    require(display.visual_state_calls == 1 &&
'''
    after = '''    Handle(V2VisualFrameWithStepId(3, "valid-step", 2));
    require(App().robot_uart_.calls.empty(),
            "renderer-v2 motion waits for successful visual application");
    require(display.visual_state_calls == 1 &&
'''
    text = text.replace(before, after, 1)
    before = '''    display.CompleteVisualState(LessonVisualApplyResult::kApplied, nullptr);
    App().DrainLessonVisualQueue();
    require(FrameType(Sent().size() - 1) == "lesson_ack" &&
'''
    after = '''    display.CompleteVisualState(LessonVisualApplyResult::kApplied, nullptr);
    App().DrainLessonVisualQueue();
    require(App().robot_uart_.calls == std::vector<std::string>({"both_arms_raise", "head_center"}),
            "renderer-v2 motion dispatches only after successful visual application");
    require(FrameType(Sent().size() - 1) == "lesson_ack" &&
'''
    text = text.replace(before, after, 1)
    path.write_text(text)
PY

bash scripts/run_host_native_lesson_handler_test.sh
