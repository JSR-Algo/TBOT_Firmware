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
    raise SystemExit(f"unterminated function: {signature}")


header = Path("main/application.h").read_text(encoding="utf-8")
source = Path("main/application.cc").read_text(encoding="utf-8")

require(
    "std::atomic<std::uint64_t> lesson_terminal_audio_generation_{0};" in header,
    "missing atomic terminal audio generation token",
)

begin = " ".join(
    function_body(source, "void Application::BeginLessonTerminalAudioQuiet").split()
)
require(
    "lesson_terminal_audio_generation_.store(" in begin
    and "static_cast<std::uint64_t>(speaking_generation_.load()) + 1" in begin,
    "terminal quiet does not arm the current encoded speaking generation",
)

stop = source[
    source.index('strcmp(state->valuestring, "stop") == 0'):
    source.index('} else if (strcmp(state->valuestring, "sentence_start") == 0)')
]
compact_stop = " ".join(stop.split())
for needle, message in (
    ("const std::uint64_t stopped_audio_generation", "missing stopped generation capture"),
    ("static_cast<std::uint64_t>(speaking_generation_.load()) + 1", "stopped generation is not encoded"),
    ("lesson_terminal_audio_generation_.exchange(0)", "terminal token is not consumed atomically"),
    ("terminal_audio_generation == stopped_audio_generation", "matching terminal generation is not checked"),
    ("terminal_audio_generation != 0", "stale terminal generation fallthrough is missing"),
):
    require(needle in compact_stop, message)

scheduled = stop[stop.index("Schedule([this"):]
capture_list = scheduled[:scheduled.index("]()")]
require("stopped_audio_generation" in capture_list, "stopped generation is not captured by value")

setter = function_body(source, "void Application::SetLessonRuntimeActive")
require(
    "lesson_terminal_audio_generation_.store(0);" in setter,
    "lesson activation does not clear terminal generation",
)

print("t54 terminal audio generation contract: PASS")
PY
