import json
import hashlib
from pathlib import Path

from repo_paths import resolve_robot_path


ROOT = Path(__file__).resolve().parents[1]
FIXTURE = resolve_robot_path(
    "docs/stories/US-006-learning-course-runtime/fixtures/lesson-protocol.v1.json", ROOT
)
PROTOCOL = "teebot-lesson-renderer.v1"
LAYERS = ("backgroundScene", "teachingObject", "robotOverlay")


def load_fixture() -> dict:
    return json.loads(FIXTURE.read_text(encoding="utf-8"))


def assert_no_inline_media(value, path="frame"):
    if isinstance(value, dict):
        for key, child in value.items():
            assert key.lower() not in {"bytes", "bytearray", "base64", "payload", "imagebytes", "imagedata"}, path
            assert_no_inline_media(child, f"{path}.{key}")
        return
    if isinstance(value, list):
        for index, child in enumerate(value):
            assert_no_inline_media(child, f"{path}[{index}]")
        return
    if isinstance(value, str):
        assert not value.strip().lower().startswith("data:"), path
        assert len(value) <= 2048, path


def test_frozen_protocol_fixture_uses_firmware_renderable_envelope_and_layers():
    fixture = load_fixture()
    frames = fixture["frames"]
    expected_types = {
        "lesson_prepare",
        "lesson_preload_status",
        "lesson_start",
        "lesson_step",
        "lesson_ack",
        "lesson_progress",
        "lesson_stop",
        "lesson_error",
    }
    assert set(frames) == expected_types

    session = fixture["_meta"]["session"]
    for name, frame in frames.items():
        assert frame["protocolVersion"] == PROTOCOL, name
        assert frame["assignmentId"] == session["assignmentId"], name
        assert frame["sessionId"] == session["sessionId"], name
        assert frame["lessonId"] == session["lessonId"], name
        assert frame["lessonVersion"] == session["lessonVersion"], name
        assert isinstance(frame["lessonVersion"], int), name
        assert isinstance(frame["sequence"], int) and frame["sequence"] >= 1, name
        assert_no_inline_media(frame, name)

    step = frames["lesson_step"]
    assert step["body"]["profile"] == "espTft"
    assert step["body"]["audio"] == {"via": "tts"}
    assert step["body"]["prompt"].strip()
    assert step["body"]["storyText"] == "TeeBot models the word barn, then invites the child to try."
    assert step["body"]["storyBeat"] == {
        "ask": "Can you try saying barn after TeeBot models it?",
        "waitForChild": True,
    }

    scene = step["body"]["scene"]
    assert tuple(scene.keys()) == LAYERS
    assert scene["backgroundScene"]["poster"]["key"] == "backgroundScene.poster"
    assert scene["backgroundScene"]["poster"]["src"]
    assert scene["teachingObject"]["asset"]["key"] == "teachingObject.barn"
    assert scene["teachingObject"]["asset"]["src"]
    assert scene["robotOverlay"]["asset"]["key"] == "robotOverlay.teach"
    assert scene["robotOverlay"]["asset"]["src"]
    assert scene["robotOverlay"]["atlas"]["image"]


def test_frozen_protocol_fixture_keeps_asset_identity_consistent_across_prepare_step_and_error():
    frames = load_fixture()["frames"]
    prepare_assets = frames["lesson_prepare"]["body"]["criticalAssets"]
    prepare_by_key = {asset["key"]: asset for asset in prepare_assets}
    assert set(prepare_by_key) == {"backgroundScene.poster", "teachingObject.barn"}

    preload_assets = frames["lesson_preload_status"]["body"]["assets"]
    preload_by_key = {asset["key"]: asset for asset in preload_assets}
    assert set(preload_by_key) == {"backgroundScene.poster", "teachingObject.barn", "robotOverlay.teach"}
    assert preload_by_key["robotOverlay.teach"] == {
        "key": "robotOverlay.teach",
        "state": "READY",
        "checksumOk": True,
    }

    scene = frames["lesson_step"]["body"]["scene"]
    assert scene["backgroundScene"]["poster"]["key"] in prepare_by_key
    assert scene["teachingObject"]["asset"]["key"] in prepare_by_key
    assert scene["robotOverlay"]["asset"]["key"] in preload_by_key
    assert scene["backgroundScene"]["poster"]["sha256"] == prepare_by_key["backgroundScene.poster"]["sha256"]
    assert scene["teachingObject"]["asset"]["sha256"] == prepare_by_key["teachingObject.barn"]["sha256"]
    assert scene["robotOverlay"]["asset"]["sha256"]

    error_context = frames["lesson_error"]["body"]["context"]
    assert error_context["assetKey"] in prepare_by_key


def test_frozen_protocol_fixture_models_conversational_step_not_pronunciation_scoring():
    step_body = load_fixture()["frames"]["lesson_step"]["body"]
    text = " ".join(
        str(value)
        for value in (
            step_body.get("prompt"),
            step_body.get("subject"),
            step_body.get("scene", {}).get("teachingObject", {}).get("focusTarget", {}).get("successUtterance"),
            step_body.get("scene", {}).get("teachingObject", {}).get("focusTarget", {}).get("missUtterance"),
        )
        if value
    ).lower()
    forbidden = ("pronunciation", "score", "evaluate", "assess", "correct", "final consonant")
    offenders = [term for term in forbidden if term in text]
    assert offenders == []
    assert "barn" in text

def test_multi_step_fixture_s5_keeps_full_render_layers_and_no_inline_media():
    fixture = load_fixture()
    session = fixture["_meta"]["session"]
    step = fixture["multiStep"]["frames"]["lesson_step_s5"]

    assert step["type"] == "lesson_step"
    assert step["protocolVersion"] == PROTOCOL
    assert step["assignmentId"] == session["assignmentId"]
    assert step["sessionId"] == session["sessionId"]
    assert step["lessonId"] == session["lessonId"]
    assert step["lessonVersion"] == session["lessonVersion"]
    assert step["stepId"] == "s5"
    assert_no_inline_media(step, "multiStep.lesson_step_s5")

    body = step["body"]
    assert body["profile"] == "espTft"
    assert body["stepType"] == "listen"
    assert body["audio"] == {"via": "tts"}
    assert body["prompt"] == 'Your turn. You try: "barn." TeeBot is listening.'
    assert body["storyText"] == "The child gets a turn to answer TeeBot by voice."
    assert body["storyBeat"] == {
        "ask": "What word do you see? You can say barn.",
        "waitForChild": True,
    }
    assert body["completionClass"] == "interactive"

    scene = body["scene"]
    assert tuple(scene.keys()) == LAYERS
    assert scene["backgroundScene"]["poster"]["key"] == "backgroundScene.poster"
    assert scene["backgroundScene"]["poster"]["src"]
    assert scene["teachingObject"]["asset"]["key"] == "teachingObject.barn"
    assert scene["teachingObject"]["asset"]["src"]
    assert scene["robotOverlay"]["robotState"] == "listening"
    assert scene["robotOverlay"]["asset"]["key"] == "robotOverlay.listening"
    assert scene["robotOverlay"]["asset"]["src"]
    pose_file = ROOT / "lesson" / "assets" / "robot" / "poses" / scene["robotOverlay"]["asset"]["src"]
    assert hashlib.sha256(pose_file.read_bytes()).hexdigest() == scene["robotOverlay"]["asset"]["sha256"]
