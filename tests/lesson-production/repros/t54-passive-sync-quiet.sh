#!/usr/bin/env bash
set -euo pipefail
# repo: $TBOT_REPRO_REPO_ROOT

python3 - <<'PY'
from pathlib import Path


def require(condition: bool, message: str) -> None:
    if not condition:
        raise SystemExit(message)


def function_body(text: str, signature: str) -> str:
    start = text.index(signature)
    brace = text.index("{", start)
    depth = 0
    for index in range(brace, len(text)):
        if text[index] == "{":
            depth += 1
        elif text[index] == "}":
            depth -= 1
            if depth == 0:
                return text[brace:index + 1]
    raise AssertionError("unterminated function")


source = Path("main/application.cc").read_text(encoding="utf-8")
body = function_body(source, "bool Application::BeginLessonAssetSyncQuiet")
compact = " ".join(body.split())
require(
    "state == kDeviceStateListening && !IsVoiceDetected()" in compact,
    "missing passive listening detection",
)
require(
    "state != kDeviceStateIdle && !passive_listening" in compact,
    "missing active-state rejection",
)
passive = body[body.index("if (passive_listening)"):body.index("tts_audio_accepting_.store(false)")]
require("protocol_->SendStopListening()" in passive, "missing passive stop-listening request")
require("SetDeviceState(kDeviceStateIdle)" in passive, "missing passive idle transition")
require(
    passive.index("protocol_->SendStopListening()")
    < passive.index("SetDeviceState(kDeviceStateIdle)"),
    "passive idle transition precedes stop-listening request",
)
print("t54 passive sync quiet contract: PASS")
PY
