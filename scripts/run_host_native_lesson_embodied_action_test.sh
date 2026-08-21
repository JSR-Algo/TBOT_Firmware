#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="$(mktemp -d "${TMPDIR:-/tmp}/tbot-host-native-lesson-embodied.XXXXXX")"
trap 'rm -rf "${BUILD_DIR}"' EXIT

CXX="${CXX:-clang++}"
CC="${CC:-clang}"
CJSON_DIR="${CJSON_DIR:-${HOME}/esp/esp-idf-v5.5.2/components/json/cJSON}"
if [[ ! -f "${CJSON_DIR}/cJSON.c" ]]; then
    echo "missing cJSON.c at ${CJSON_DIR}; set CJSON_DIR" >&2
    exit 127
fi

"${CC}" -std=c11 -O0 -g -Wall -Wextra -Werror \
    -I"${CJSON_DIR}" -c "${CJSON_DIR}/cJSON.c" -o "${BUILD_DIR}/cJSON.o"

"${CXX}" -std=c++17 -O0 -g -Wall -Wextra -Werror \
    -fsanitize=address,undefined -fno-omit-frame-pointer \
    -I"${CJSON_DIR}" -I"${ROOT}/main" \
    "${ROOT}/tests/native/lesson_embodied_action_host_test.cc" \
    "${ROOT}/main/lesson_embodied_action.cc" "${BUILD_DIR}/cJSON.o" \
    -o "${BUILD_DIR}/lesson_embodied_action_host_test"

"${BUILD_DIR}/lesson_embodied_action_host_test"

python3 - "${ROOT}" <<'PY'
from pathlib import Path
import sys

root = Path(sys.argv[1])
header = (root / "main/application.h").read_text(encoding="utf-8")
source = (root / "main/application.cc").read_text(encoding="utf-8")

required_header = (
    "LessonRuntimeToken GetLessonRuntimeToken() const;",
    "LessonEmbodiedMotionResult ApplyLessonEmbodiedPreset(",
    "LessonEmbodiedMotionResult CancelLessonEmbodiedAction(",
    "LessonEmbodiedMotionResult RestoreLessonRestPose(",
)
for declaration in required_header:
    assert declaration in header, f"missing application authority declaration: {declaration}"

for method in (
    "SendLeftArmRaise", "SendRightArmRaise", "SendLeftArmLower", "SendRightArmLower",
    "SendBothArmsRaise", "SendBothArmsLower", "SendLeftArmSetPercent",
    "SendRightArmSetPercent", "SendBothArmsSetPercent", "SendHeadTurnLeft",
    "SendHeadTurnRight", "SendHeadCenter", "SendHeadSetAngle", "SendHeadSetPercent",
):
    start = source.index(f"bool Application::{method}(")
    body = source[start:source.index("\n}", start)]
    assert "lesson_runtime_active_.load()" in body, f"normal MCP bypass reopened: {method}"

apply_start = source.index("LessonEmbodiedMotionResult Application::ApplyLessonEmbodiedPreset(")
apply_body = source[apply_start:source.index("\n}", apply_start)]
assert "IsLessonRuntimeTokenAuthorized" in apply_body
assert "robot_uart_.SendHeadSetPercent" in apply_body
assert "robot_uart_.SendLeftArmSetPercent" in apply_body
assert "robot_uart_.SendRightArmSetPercent" in apply_body
assert "SendRightArmRaise" not in apply_body

runtime_start = source.index("void Application::SetLessonRuntimeActive(bool active)")
runtime_body = source[runtime_start:source.index("\n}", runtime_start)]
assert "was_active != active" in runtime_body
assert runtime_body.count("NextLessonRuntimeGeneration") == 2
assert "lesson_runtime_generation_" in header
print("lesson embodied application authority source contract OK")
PY
