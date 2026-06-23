"""Contract tests for the packaged barn lesson content.

The robot lesson is speaking practice for young children. Early interactive
steps should model, invite a response, and continue after the child speaks; they
must not turn into immediate pronunciation scoring or correction.
"""
import json
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[1]
LESSON = json.loads((ROOT / "lesson" / "lesson.json").read_text(encoding="utf-8"))["lesson"]
RENDER_CONTRACT = json.loads((ROOT / "lesson" / "render-contract.json").read_text(encoding="utf-8"))
BACKEND_CANONICAL_MANIFEST = (
    ROOT.parent.parent
    / "tbot-backend"
    / "scripts"
    / "seed"
    / "076_canonical-manifest.espTft.json"
)

FORBIDDEN_GUIDED_PRACTICE_TERMS = (
    "watch my mouth",
    "make a strong",
    "ending sound",
    "final consonant",
    "pronunciation",
    "score",
    "evaluate",
    "assess",
    "correct",
    "clearly",
    "don't drop",
    "/n/",
    "/b/",
)
PASSIVE_RESPONSE_DEMAND_TERMS = (
    "are you ready",
    "do you remember",
    "wave and say",
    "your turn",
    "you try",
    "say it with",
    "you can say",
    "teebot is listening",
    "waits for your voice",
)

EXPECTED_STEP_IDS = tuple(f"s{index}" for index in range(1, 10))
EXPECTED_STEP_TYPES = (
    "greeting",
    "review",
    "focus",
    "model",
    "listen",
    "repeat",
    "fillBlank",
    "feedback",
    "celebrate",
)
INTERACTIVE_STEP_IDS = ("s4", "s5", "s6", "s7")
EXPECTED_COMPLETION_CLASSES = (
    "passive",
    "passive",
    "passive",
    "interactive",
    "interactive",
    "interactive",
    "interactive",
    "passive",
    "passive",
)


def _step(step_id: str) -> dict:
    return next(step for step in LESSON["steps"] if step["id"] == step_id)


def _combined_text(step: dict) -> str:
    fields = ["title", "prompt", "helperText", "l1TransferHint", "parentSummary"]
    return " ".join(str(step.get(field) or "") for field in fields).lower()

def _backend_canonical_manifest() -> dict:
    if not BACKEND_CANONICAL_MANIFEST.exists():
        pytest.skip("backend canonical espTft manifest lives in sibling tbot-backend checkout")
    return json.loads(BACKEND_CANONICAL_MANIFEST.read_text(encoding="utf-8"))

def _render_step_phase(step: dict):
    return step.get("phase") or step.get("phaseWhenSpeaking") or step.get("phaseWhenAwaitingChild")

def test_packaged_lesson_keeps_the_full_guided_story_sequence():
    steps = LESSON["steps"]
    assert tuple(step["id"] for step in steps) == EXPECTED_STEP_IDS
    assert tuple(step["type"] for step in steps) == EXPECTED_STEP_TYPES
    assert LESSON["focusItems"] == ["barn", "farm", "hay"]

    combined = " ".join(_combined_text(step) for step in steps)
    for word in ("barn", "farm", "hay"):
        assert word in combined

    for step in steps:
        assert step["prompt"].strip()

def test_packaged_lesson_story_stays_in_parity_with_backend_canonical_manifest():
    backend = _backend_canonical_manifest()

    assert LESSON["id"] == backend["lessonId"]
    assert LESSON["ageBand"] == backend["ageBand"]
    assert backend["profile"] == "espTft"
    assert LESSON["focusItems"] == ["barn", "farm", "hay"]

    backend_steps = backend["steps"]
    lesson_steps = LESSON["steps"]
    render_steps = RENDER_CONTRACT["stepRenderMap"]
    assert [step["id"] for step in lesson_steps] == [step["id"] for step in backend_steps]
    assert [step["stepId"] for step in render_steps] == [step["id"] for step in backend_steps]

    expected_interactive = {"s4", "s5", "s6", "s7"}
    by_render_step = {step["stepId"]: step for step in render_steps}
    for backend_step, lesson_step in zip(backend_steps, lesson_steps):
        step_id = backend_step["id"]
        render_step = by_render_step[step_id]

        assert lesson_step["type"] == backend_step["type"]
        assert lesson_step["prompt"] == backend_step["prompt"]
        assert lesson_step.get("helperText") == backend_step.get("helperText")
        assert lesson_step.get("l1TransferHint") == backend_step.get("l1TransferHint")
        assert lesson_step.get("choices") == backend_step.get("choices")
        assert lesson_step["completionClass"] == ("interactive" if step_id in expected_interactive else "passive")
        assert lesson_step["robotState"] == backend_step["robotState"]
        assert render_step["stepType"] == backend_step["type"]
        assert render_step["pose"] == backend_step["pose"]
        assert render_step["expression"] == backend_step["expression"]
        assert render_step["subject"] == backend_step["subject"]
        assert _render_step_phase(render_step) == backend_step["phase"]

    assert tuple(step["id"] for step in lesson_steps if step["completionClass"] == "interactive") == INTERACTIVE_STEP_IDS
    assert _step("s7")["helperText"] == "TeeBot waits for your voice."

def test_packaged_lesson_opens_child_turns_only_for_guided_response_steps():
    listening_steps = tuple(
        step["id"]
        for step in LESSON["steps"]
        if step.get("robotState") in {"modeling", "listening", "thinking"}
    )
    assert listening_steps == INTERACTIVE_STEP_IDS

    for step_id in INTERACTIVE_STEP_IDS:
        step = _step(step_id)
        assert "barn" in step["prompt"].lower()
        assert step["type"] in {"model", "listen", "repeat", "fillBlank"}


def test_packaged_lesson_declares_completion_class_for_every_step():
    steps = LESSON["steps"]
    assert tuple(step["completionClass"] for step in steps) == EXPECTED_COMPLETION_CLASSES
    assert tuple(
        step["id"] for step in steps if step["completionClass"] == "interactive"
    ) == INTERACTIVE_STEP_IDS

def test_packaged_media_uses_references_not_inline_payloads():
    media = LESSON["media"]
    assert media["videoSrc"] == "backgrounds/scenes/barn-round-field.mp4"
    assert media["subjectSlug"] == "barn"

    payload = json.dumps(LESSON, ensure_ascii=False).lower()
    assert "data:image" not in payload
    assert "base64" not in payload
    assert "imagebytes" not in payload
    assert "imagedata" not in payload


def test_lesson_metadata_targets_speaking_practice_not_pronunciation_scoring():
    assert LESSON.get("practiceMode") == "guided_speaking"
    assert LESSON.get("immediatePronunciationAssessment") is False
    assert LESSON.get("l1TransferTarget") != "final_consonants"

    source_text = json.dumps(LESSON.get("sourceReferences", []), ensure_ascii=False).lower()
    assert "score" not in source_text
    assert "final_consonants" not in source_text
    assert "consonant_clusters" not in source_text


def test_interactive_steps_prompt_child_to_speak_without_correction_language():
    for step_id in ("s4", "s5", "s6", "s7"):
        text = _combined_text(_step(step_id))
        offenders = [term for term in FORBIDDEN_GUIDED_PRACTICE_TERMS if term in text]
        assert offenders == [], f"{step_id} uses assessment language: {offenders}"

    assert "listen" in _step("s4")["prompt"].lower()
    assert "you try" in _step("s5")["prompt"].lower()
    assert "say it with teebot" in _step("s6")["prompt"].lower()
    assert "you can say barn" in _step("s7")["prompt"].lower()


def test_interactive_steps_are_guided_turns_that_wait_for_child_response():
    expected_guidance = {
        "s4": ("listen", "barn"),
        "s5": ("your turn", "you try"),
        "s6": ("say it with teebot", "barn"),
        "s7": ("you can say barn", "teebot waits"),
    }
    for step_id, required_phrases in expected_guidance.items():
        step = _step(step_id)
        text = _combined_text(step)
        assert step["completionClass"] == "interactive"
        assert step["robotState"] in {"modeling", "listening", "thinking"}
        for phrase in required_phrases:
            assert phrase in text, f"{step_id} missing guided response phrase: {phrase}"

    assert tuple(step["id"] for step in LESSON["steps"] if step["completionClass"] == "interactive") == INTERACTIVE_STEP_IDS

def test_feedback_acknowledges_response_without_scoring_it():
    feedback = _combined_text(_step("s8"))
    assert "i heard you" in feedback
    offenders = [term for term in FORBIDDEN_GUIDED_PRACTICE_TERMS if term in feedback]
    assert offenders == [], f"feedback uses assessment language: {offenders}"

def test_passive_steps_do_not_prompt_for_child_response():
    for step in LESSON["steps"]:
        if step["completionClass"] != "passive":
            continue
        text = _combined_text(step)
        offenders = [term for term in PASSIVE_RESPONSE_DEMAND_TERMS if term in text]
        assert offenders == [], f"{step['id']} is passive but asks for a child response: {offenders}"
