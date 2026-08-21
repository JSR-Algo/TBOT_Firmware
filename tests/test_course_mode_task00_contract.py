from __future__ import annotations

import copy
import hashlib
import json
import unicodedata
from pathlib import Path
from typing import Any

import pytest


FIXTURE_DIR = Path(__file__).parent / "fixtures" / "course-mode"
CONTRACT_PATH = FIXTURE_DIR / "course-mode-pilot-cat-ball.json"
LAYOUT_PATH = FIXTURE_DIR / "renderer-v4-visual-layout.json"
CONTRACT_CHECKSUM = "cf12b1a5f71f0a80a8ee22bb2cdc775ada5b803e26d154e5d29c76b14c9fb264"
LAYOUT_CHECKSUM = "e61b56d1f8219a86c7f3986e7d5c70b91f512286604b5b206ef11e2c989d275c"
EVIDENCE_NAMES = [
    "NOT_STARTED", "EXPOSED", "UNDERSTOOD", "SUPPORTED_SPEECH",
    "INDEPENDENT_RECALL", "TRANSFERRED", "MASTERED_TODAY", "REVIEW_NEEDED",
]
INTENT_NAMES = [
    "REST_WARM", "GREET_SMALL", "INVITE_CHILD", "PRESENT_CENTER", "PRESENT_LEFT",
    "PRESENT_RIGHT", "LISTEN_STILL", "THINK_CURIOUS", "ACKNOWLEDGE_STORY",
    "MODEL_WORD", "ENCOURAGE_SMALL", "TRY_DIFFERENT_WAY", "CELEBRATE_RECALL",
    "CELEBRATE_MASTERY", "COMFORT_CALM", "PAUSE_CHOICE", "GOODBYE_SMALL",
]
LISTEN_SEQUENCE = [
    "speech_complete", "gesture_settled", "head_centered", "arms_lowered",
    "motor_stopped", "assessment_window_open",
]


def load(path: Path) -> dict[str, Any]:
    return json.loads(path.read_text(encoding="utf-8"))


def normalized(value: Any) -> Any:
    if isinstance(value, str):
        return unicodedata.normalize("NFC", value)
    if isinstance(value, list):
        return [normalized(item) for item in value]
    if isinstance(value, dict):
        return {key: normalized(value[key]) for key in sorted(value)}
    return value


def checksum(document: dict[str, Any]) -> str:
    payload = {key: value for key, value in document.items() if key != "contractChecksum"}
    canonical = json.dumps(
        normalized(payload), ensure_ascii=False, separators=(",", ":"), sort_keys=True
    ).encode("utf-8")
    return hashlib.sha256(canonical).hexdigest()


def exact_keys(value: dict[str, Any], expected: set[str]) -> None:
    assert set(value) == expected


def reject_raw_servo_fields(value: Any) -> None:
    if isinstance(value, dict):
        for key, child in value.items():
            lowered = key.lower()
            assert "servo" not in lowered
            assert not lowered.endswith(("percent", "degrees", "pwm"))
            reject_raw_servo_fields(child)
    elif isinstance(value, list):
        for child in value:
            reject_raw_servo_fields(child)


def validate_contract(document: dict[str, Any], expected_checksum: str = CONTRACT_CHECKSUM) -> None:
    exact_keys(document, {
        "schemaVersion", "contractVersion", "contractChecksum", "checksumRules", "fixtureId",
        "preset", "lesson", "targets", "evidenceNames", "embodiedIntentNames", "visualFocus",
        "activities", "renderer",
    })
    assert document["schemaVersion"] == 1
    assert document["contractVersion"] == "courseCompanion.v2.contract.v1"
    assert document["contractChecksum"] == expected_checksum == checksum(document)
    assert document["fixtureId"] == "course-mode-pilot-cat-ball"
    assert document["preset"] == {"presetId": "courseCompanion", "presetVersion": 2}
    assert document["lesson"] == {
        "lessonId": "course-mode-pilot-cat-ball", "lessonVersion": 1,
        "lessonSessionId": "00000000-0000-4000-8000-000000000201",
    }
    assert document["evidenceNames"] == EVIDENCE_NAMES
    assert document["embodiedIntentNames"] == INTENT_NAMES
    assert document["visualFocus"] == {
        "directionSource": "authored_visual_focus_region",
        "regions": ["focus.center.primary", "focus.left.choice", "focus.right.choice"],
        "presentCenterTarget": "single_teaching_object",
    }
    assert document["renderer"] == {
        "rendererId": "teebot-lesson-renderer.v4",
        "visualLayoutContract": "renderer-v4.course-mode-layout.v1",
    }
    assert [
        (target["role"], target["targetId"], target["targetWord"], target["vietnameseMeanings"])
        for target in document["targets"]
    ] == [
        ("primary", "animals.cat", "cat", ["con mèo"]),
        ("optional_secondary", "toys.ball", "ball", ["quả bóng"]),
    ]
    activity_ids = {activity["activityId"] for activity in document["activities"]}
    assert activity_ids == {
        "cat-discover-center-01", "cat-meaning-left-right-01", "cat-recall-visual-02",
        "cat-transfer-scene-01", "cat-delayed-recall-01", "ball-discover-center-01",
    }
    for activity in document["activities"]:
        exact_keys(activity, {
            "activityId", "targetId", "stage", "activityType", "evidenceName", "contextId",
            "embodiedIntent", "visualFocusRegion", "answerPolicy", "listeningTransition",
            "reducedMotionFallback",
        })
        assert activity["evidenceName"] in EVIDENCE_NAMES
        assert activity["embodiedIntent"] in INTENT_NAMES
        assert activity["visualFocusRegion"] in document["visualFocus"]["regions"]
        assert activity["listeningTransition"] == LISTEN_SEQUENCE
        exact_keys(activity["answerPolicy"], {
            "targetTextVisible", "targetAudioBeforeAssessment", "spokenTargetInPrompt",
            "multipleChoiceContainsTarget", "minElapsedSinceFullModelMs",
            "minInterveningActivityCount",
        })
        if activity["stage"] in {"RECALL", "TRANSFER", "DELAYED_RECALL"}:
            policy = activity["answerPolicy"]
            assert not any(policy[key] for key in (
                "targetTextVisible", "targetAudioBeforeAssessment", "spokenTargetInPrompt",
                "multipleChoiceContainsTarget",
            ))
            assert policy["minElapsedSinceFullModelMs"] >= 20_000
            assert policy["minInterveningActivityCount"] >= 1
    reject_raw_servo_fields(document)


def inside(rect: dict[str, int], canvas: dict[str, int]) -> bool:
    return rect["x"] >= 0 and rect["y"] >= 0 and rect["width"] > 0 and rect["height"] > 0 \
        and rect["x"] + rect["width"] <= canvas["width"] \
        and rect["y"] + rect["height"] <= canvas["height"]


def overlap(a: dict[str, int], b: dict[str, int]) -> int:
    width = max(0, min(a["x"] + a["width"], b["x"] + b["width"]) - max(a["x"], b["x"]))
    height = max(0, min(a["y"] + a["height"], b["y"] + b["height"]) - max(a["y"], b["y"]))
    return width * height


def validate_layout(document: dict[str, Any], expected_checksum: str = LAYOUT_CHECKSUM) -> None:
    exact_keys(document, {
        "schemaVersion", "contractVersion", "contractChecksum", "checksumRules", "rendererId",
        "canvas", "layerOrder", "layers", "collisionLimits", "captionSafeArea", "focusAnchors",
        "listeningCue", "reducedMotion", "mirroring",
    })
    assert document["contractVersion"] == "renderer-v4.course-mode-layout.v1"
    assert document["contractChecksum"] == expected_checksum == checksum(document)
    assert document["rendererId"] == "teebot-lesson-renderer.v4"
    assert document["canvas"] == {"width": 480, "height": 320}
    assert document["layerOrder"] == [
        "background", "teachingObject", "robotOverlay", "transientFocusCue",
    ]
    for index, name in enumerate(document["layerOrder"]):
        layer = document["layers"][name]
        exact_keys(layer, {"zIndex", "bounds"})
        exact_keys(layer["bounds"], {"x", "y", "width", "height"})
        assert layer["zIndex"] == index
        assert inside(layer["bounds"], document["canvas"])
    teaching = document["layers"]["teachingObject"]["bounds"]
    robot = document["layers"]["robotOverlay"]["bounds"]
    assert teaching == {"x": 20, "y": 168, "width": 95, "height": 95}
    assert robot == {"x": 118, "y": 160, "width": 150, "height": 150}
    assert overlap(teaching, robot) == 0
    assert robot["x"] - (teaching["x"] + teaching["width"]) >= document["collisionLimits"]["minimumHorizontalGapPixels"]
    assert inside(document["captionSafeArea"], document["canvas"])
    assert inside(document["listeningCue"]["bounds"], document["canvas"])
    assert overlap(document["listeningCue"]["bounds"], teaching) == 0
    assert overlap(document["listeningCue"]["bounds"], robot) == 0
    assert document["listeningCue"]["minimumTextHeightPixels"] >= 24
    assert document["reducedMotion"] == {
        "fallback": "face_and_transient_focus_cue", "preservesLearningMeaning": True,
        "requiresServoMotion": False,
    }
    assert document["mirroring"] == {
        "mode": "authored_focus_regions_only", "automaticWholeCompositionMirror": False,
        "inferDirectionFromModelText": False,
    }


def test_canonical_course_mode_fixture_is_strict_and_checksum_pinned() -> None:
    validate_contract(load(CONTRACT_PATH))


@pytest.mark.parametrize("mutation", [
    lambda value: value.update({"unknownKey": True}),
    lambda value: value["activities"][0].update({"servoValue": 60}),
    lambda value: value["activities"][0].update({"embodiedIntent": "DANCE_RANDOM"}),
    lambda value: value["activities"][2]["answerPolicy"].update({"targetTextVisible": True}),
])
def test_contract_mutations_fail_closed(mutation) -> None:
    value = load(CONTRACT_PATH)
    mutation(value)
    value["contractChecksum"] = checksum(value)
    with pytest.raises(AssertionError):
        validate_contract(value, value["contractChecksum"])


def test_renderer_v4_static_composition_and_mutations() -> None:
    value = load(LAYOUT_PATH)
    validate_layout(value)
    for mutation in (
        lambda item: item["layers"]["teachingObject"]["bounds"].update({"x": 450}),
        lambda item: item.update({"layerOrder": ["background", "robotOverlay", "teachingObject", "transientFocusCue"]}),
        lambda item: item["layers"]["robotOverlay"]["bounds"].update({"x": 100}),
    ):
        drifted = copy.deepcopy(value)
        mutation(drifted)
        drifted["contractChecksum"] = checksum(drifted)
        with pytest.raises(AssertionError):
            validate_layout(drifted, drifted["contractChecksum"])


def test_renderer_v1_and_existing_manifest_checksum_fixture_are_unchanged() -> None:
    render_contract = Path(__file__).parents[1] / "lesson" / "render-contract.json"
    assert hashlib.sha256(render_contract.read_bytes()).hexdigest() == (
        "bf612583a15a88613e57e5226b9f51bff5a05e66b618d830c5a78615ff20209a"
    )
    assert load(render_contract)["contractId"] == "teebot-lesson-render-pack.v1"
    command = Path(__file__).parent / "fixtures" / "tvideo_farm_command_v2.json"
    assert hashlib.sha256(command.read_bytes()).hexdigest() == (
        "caaffcb293ed243acea2577c83f17bec2403708b4c2003622b33a667f1222f91"
    )
    assert load(command)["source"]["manifestChecksum"] == (
        "bb7d4dcdf6318096c0b9224dc48bcdcb3ff78b325706cdc9c5d39bd4e7da94e4"
    )
