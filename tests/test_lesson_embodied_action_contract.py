import hashlib
import json
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
HANDOFF_FIXTURE = (
    ROOT.parents[2]
    / "esp32-server/main/tbot-server/tests/fixtures/course-mode/lesson-embodied-action-wire-contract.json"
)


def test_frozen_task02_fixture_checksum_and_ack_outcomes():
    payload = HANDOFF_FIXTURE.read_bytes()
    assert hashlib.sha256(payload).hexdigest() == (
        "3d8f83ccdc159c4881675c306c0f0e7248680766ca2860ef041ab5937fc4224f"
    )
    fixture = json.loads(payload)
    assert fixture["ackOutcomes"] == ["applied", "degraded", "rejected"]
    assert fixture["localTerminalOutcomes"] == ["superseded", "timed_out"]


def test_handler_builds_the_closed_embodied_ack_separately_from_legacy_ack():
    source = (ROOT / "main/lesson_handler.cc").read_text()
    builder = source[source.index("std::string BuildLessonEmbodiedAck") :]
    builder = builder[: builder.index("const char* EmbodiedOutcome")]
    for required in (
        '"type", "lesson_ack"',
        '"assignmentId"',
        '"sessionId"',
        '"stepId"',
        '"sequence"',
        '"acks"',
        '"embodiedAction"',
        '"actionId"',
        '"actionGeneration"',
        '"outcome"',
        '"returnedToRest"',
    ):
        assert required in builder
    for forbidden in (
        '"protocolVersion"',
        '"timestamp"',
        '"rendered"',
        '"degradedReason"',
        '"superseded"',
        '"timed_out"',
    ):
        assert forbidden not in builder


def test_course_mode_faces_are_supportive_and_v1_incorrect_remains_unchanged():
    action_source = (ROOT / "main/lesson_embodied_action.cc").read_text()
    handler_source = (ROOT / "main/lesson_handler.cc").read_text()
    for face in ("neutral", "happy", "thinking", "relaxed"):
        assert f'"{face}"' in action_source
    for forbidden in ("crying", "angry", "shocked", "embarrassed"):
        assert f'"{forbidden}"' not in action_source
    assert 'std::strcmp(state, "incorrect") == 0' in handler_source
    assert 'return {"sad", "Chưa đúng"};' in handler_source


def test_capability_shape_and_reduced_motion_branch_are_present():
    source = (ROOT / "main/lesson_handler.cc").read_text()
    assert '"lessonCourseMode"' in source
    assert '"embodiedActions", true' in source
    assert '"reducedMotion"' in source
    assert "g_course_mode_reduced_motion.load" in source
    assert "LessonEmbodiedMotionResult::kDegraded" in source
