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
CONTRACT_FILE_SHA256 = "05e18ae61aee0660c653a9386854552a23f90c8a1f8cfb9e7ff4e15d1d277470"
LAYOUT_FILE_SHA256 = "031e69b82c33da87f5ec63c21cb1e756549b802e6a8bd567a1b76f51e4f77dc5"
CHECKSUM_RULES = {
    "algorithm": "SHA-256",
    "canonicalization": "tbot-json-c14n.v1",
    "encoding": "UTF-8",
    "unicodeNormalization": "NFC",
    "objectKeyOrder": "lexicographic",
    "arrayOrder": "preserved",
    "whitespace": "none",
    "excludedJsonPointers": ["/contractChecksum"],
}
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
ACTIVITY_IDENTITIES = [
    ("cat-discover-center-01", "animals.cat", "DISCOVER", "single_visual_discovery", "EXPOSED", "cat_primary_visual", "PRESENT_CENTER", "focus.center.primary"),
    ("cat-meaning-left-right-01", "animals.cat", "UNDERSTAND", "authored_two_choice_visual", "UNDERSTOOD", "cat_dog_visual_contrast", "PRESENT_LEFT", "focus.left.choice"),
    ("cat-recall-visual-02", "animals.cat", "RECALL", "independent_visual_naming", "INDEPENDENT_RECALL", "cat_primary_visual_recall", "PRESENT_CENTER", "focus.center.primary"),
    ("cat-transfer-scene-01", "animals.cat", "TRANSFER", "second_context_scene_naming", "TRANSFERRED", "cat_second_visual_scene", "PRESENT_RIGHT", "focus.right.choice"),
    ("cat-delayed-recall-01", "animals.cat", "DELAYED_RECALL", "delayed_independent_naming", "MASTERED_TODAY", "cat_delayed_callback", "PRESENT_CENTER", "focus.center.primary"),
    ("ball-discover-center-01", "toys.ball", "DISCOVER", "single_visual_discovery", "EXPOSED", "ball_primary_visual", "PRESENT_CENTER", "focus.center.primary"),
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
    assert document["checksumRules"] == CHECKSUM_RULES
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
    for target in document["targets"]:
        exact_keys(target, {"targetId", "targetWord", "role", "vietnameseMeanings", "activityIds"})
    assert document["targets"][0]["activityIds"] == [
        "cat-discover-center-01", "cat-meaning-left-right-01", "cat-recall-visual-02",
        "cat-transfer-scene-01", "cat-delayed-recall-01",
    ]
    assert document["targets"][1]["activityIds"] == ["ball-discover-center-01"]
    assert [
        (
            activity["activityId"], activity["targetId"], activity["stage"],
            activity["activityType"], activity["evidenceName"], activity["contextId"],
            activity["embodiedIntent"], activity["visualFocusRegion"],
        )
        for activity in document["activities"]
    ] == ACTIVITY_IDENTITIES
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
    assert document["schemaVersion"] == 1
    assert document["contractVersion"] == "renderer-v4.course-mode-layout.v1"
    assert document["contractChecksum"] == expected_checksum == checksum(document)
    assert document["checksumRules"] == CHECKSUM_RULES
    assert document["rendererId"] == "teebot-lesson-renderer.v4"
    assert document["canvas"] == {"width": 480, "height": 320}
    assert document["layerOrder"] == [
        "background", "teachingObject", "robotOverlay", "transientFocusCue",
    ]
    assert set(document["layers"]) == set(document["layerOrder"])
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
    assert document["collisionLimits"] == {
        "robotTeachingObjectMaxOverlapPixels": 0,
        "minimumHorizontalGapPixels": 3,
    }
    assert overlap(teaching, robot) == 0
    assert robot["x"] - (teaching["x"] + teaching["width"]) >= document["collisionLimits"]["minimumHorizontalGapPixels"]
    exact_keys(document["captionSafeArea"], {"x", "y", "width", "height"})
    assert document["captionSafeArea"] == {"x": 16, "y": 16, "width": 448, "height": 52}
    exact_keys(document["listeningCue"], {"bounds", "minimumTextHeightPixels", "textKey"})
    exact_keys(document["listeningCue"]["bounds"], {"x", "y", "width", "height"})
    assert document["listeningCue"] == {
        "bounds": {"x": 282, "y": 168, "width": 182, "height": 52},
        "minimumTextHeightPixels": 24,
        "textKey": "course_mode.listening",
    }
    assert document["focusAnchors"] == {
        "focus.center.primary": {"x": 67, "y": 215},
        "focus.left.choice": {"x": 67, "y": 215},
        "focus.right.choice": {"x": 366, "y": 215},
    }
    for anchor in document["focusAnchors"].values():
        exact_keys(anchor, {"x", "y"})
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
    assert hashlib.sha256(CONTRACT_PATH.read_bytes()).hexdigest() == CONTRACT_FILE_SHA256
    assert hashlib.sha256(LAYOUT_PATH.read_bytes()).hexdigest() == LAYOUT_FILE_SHA256
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


@pytest.mark.parametrize("mutation", [
    lambda value: value["targets"][0].update({"unknownKey": True}),
    lambda value: value["checksumRules"].update({"unknownKey": True}),
])
def test_nested_contract_schema_mutations_fail_closed(mutation) -> None:
    value = load(CONTRACT_PATH)
    mutation(value)
    value["contractChecksum"] = checksum(value)
    with pytest.raises(AssertionError):
        validate_contract(value, value["contractChecksum"])


@pytest.mark.parametrize("mutation", [
    lambda value: value["activities"][0].update({"targetId": "toys.ball"}),
    lambda value: value["activities"][0].update({"stage": "BOGUS"}),
    lambda value: value["activities"][0].update({"activityType": "bogus"}),
    lambda value: value["activities"].append(copy.deepcopy(value["activities"][0])),
    lambda value: value["targets"][0].update({"activityIds": ["ball-discover-center-01"]}),
])
def test_exact_activity_identity_mutations_fail_closed(mutation) -> None:
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


@pytest.mark.parametrize("mutation", [
    lambda value: value.update({"schemaVersion": 99}),
    lambda value: value["layers"].update({"unexpectedLayer": copy.deepcopy(value["layers"]["background"])}),
    lambda value: value["collisionLimits"].update({"robotTeachingObjectMaxOverlapPixels": 999}),
    lambda value: value["captionSafeArea"].update({"unknownKey": True}),
    lambda value: value["focusAnchors"]["focus.center.primary"].update({"unknownKey": True}),
    lambda value: value["listeningCue"].update({"unknownKey": True}),
])
def test_nested_layout_schema_mutations_fail_closed(mutation) -> None:
    value = load(LAYOUT_PATH)
    mutation(value)
    value["contractChecksum"] = checksum(value)
    with pytest.raises(AssertionError):
        validate_layout(value, value["contractChecksum"])


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
