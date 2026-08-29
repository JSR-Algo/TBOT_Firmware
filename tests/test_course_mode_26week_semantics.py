import json
import os
import re
import shutil
import subprocess
from pathlib import Path


SUPPORTED_RENDERER = "teebot-lesson-renderer.v5"
SUPPORTED_LAYOUT = "renderer-v5.layered-cinematic-layout.v1"
SUPPORTED_PHASES = {"flyIn", "teach", "listen", "celebrate"}
SUPPORTED_LAYERS = {"background", "teachingObject", "robotOverlay"}
SUPPORTED_SLOTS = {"backgroundScene", "teachingObject", "robotOverlay"}
SUPPORTED_ACTIONS = {"advance", "support", "pause", "complete"}
SUPPORTED_VISUAL_STATES = {
    "teach", "listen", "thinking", "nearMiss", "incorrect", "retry",
    "correct", "celebrate", "completion",
}
SUPPORTED_WIRE_INTENTS = {
    "NONE", "PRESENT_LEFT", "PRESENT_CENTER", "PRESENT_RIGHT",
    "ENCOURAGE_RETRY", "CELEBRATE_MASTERY", "CALM_REGULATE", "LISTEN_ATTENTIVELY",
}


def _workspace_root() -> Path:
    root = next(
        (parent for parent in Path(__file__).resolve().parents if parent.name == "TBOT"),
        None,
    )
    assert root is not None, "firmware checkout must live under the TBOT workspace"
    return root


def _node(backend: Path) -> str:
    explicit = os.environ.get("COURSE_MODE_NODE")
    node = explicit
    if node is None:
        nvmrc = backend / ".nvmrc"
        pinned = nvmrc.read_text(encoding="utf-8").strip().removeprefix("v") \
            if nvmrc.is_file() else ""
        pinned_parts = tuple(int(part) for part in pinned.split(".")) \
            if re.fullmatch(r"\d+(?:\.\d+){0,2}", pinned) else ()
        nvm_dir = Path(os.environ.get("NVM_DIR", "")).expanduser() \
            if os.environ.get("NVM_DIR") else Path.home() / ".nvm"
        version_pattern = re.compile(r"v(\d+)\.(\d+)\.(\d+)$")
        candidates = sorted(
            (
                (tuple(int(part) for part in match.groups()), path)
                for path in nvm_dir.glob("versions/node/v*/bin/node")
                if path.is_file() and os.access(path, os.X_OK)
                if (match := version_pattern.fullmatch(path.parents[1].name)) is not None
                if pinned_parts and tuple(int(part) for part in match.groups())[
                    :len(pinned_parts)
                ] == pinned_parts
            ),
            key=lambda item: item[0],
        )
        node = str(candidates[-1][1]) if candidates else None
    node = node or shutil.which("node")
    assert node, (
        "COURSE_MODE_NODE, node on PATH, or the backend-pinned NVM runtime is required "
        "for canonical source export"
    )
    return node


def test_node_falls_back_to_backend_pinned_nvm_runtime(tmp_path: Path, monkeypatch) -> None:
    backend = tmp_path / "backend"
    backend.mkdir()
    (backend / ".nvmrc").write_text("20\n", encoding="utf-8")
    nvm_dir = tmp_path / "custom-nvm"
    older = nvm_dir / "versions" / "node" / "v20.18.1" / "bin" / "node"
    pinned = nvm_dir / "versions" / "node" / "v20.20.2" / "bin" / "node"
    unrelated = nvm_dir / "versions" / "node" / "v22.23.2" / "bin" / "node"
    prerelease = nvm_dir / "versions" / "node" / "v99.0.0-rc.1" / "bin" / "node"
    for executable in (older, pinned, unrelated, prerelease):
        executable.parent.mkdir(parents=True, exist_ok=True)
        executable.write_text("#!/bin/sh\n", encoding="utf-8")
        executable.chmod(0o755)
    monkeypatch.delenv("COURSE_MODE_NODE", raising=False)
    monkeypatch.setenv("PATH", "")
    monkeypatch.setenv("NVM_DIR", str(nvm_dir))

    assert _node(backend) == str(pinned)


def _canonical_curriculum(tmp_path: Path) -> dict:
    backend = Path(os.environ.get("COURSE_MODE_BACKEND_ROOT", _workspace_root() / "tbot-backend"))
    verifier = backend / "scripts" / "verify-course-mode-curriculum.mjs"
    assert verifier.is_file(), f"canonical curriculum verifier missing: {verifier}"
    output = tmp_path / "curriculum.json"
    node = _node(backend)
    env = {**os.environ, "PATH": f"{Path(node).parent}:{os.environ.get('PATH', '')}"}
    result = subprocess.run(
        [node, str(verifier), "--contracts-output", str(output)],
        cwd=backend,
        env=env,
        text=True,
        capture_output=True,
        timeout=60,
        check=False,
    )
    assert result.returncode == 0, result.stdout + result.stderr
    return json.loads(output.read_text(encoding="utf-8"))


def _firmware_activity_frames(export: dict, tmp_path: Path) -> list[dict]:
    server = _workspace_root() / "robot" / "esp32-server" / "main" / "tbot-server"
    assert (server / "core" / "lesson" / "runtime.py").is_file()
    source = tmp_path / "canonical.json"
    output = tmp_path / "wire-frames.json"
    source.write_text(json.dumps(export), encoding="utf-8")
    program = r'''
import asyncio, json, sys
from core.lesson.course_orchestrator import CourseDecision, SessionState
from core.lesson.embodied_intent import EmbodiedIntent
from core.lesson.runtime import LessonRuntime

async def main():
    export = json.load(open(sys.argv[1], encoding="utf-8"))
    sent = []
    lesson = object.__new__(LessonRuntime)
    lesson.assignment_id = "semantic-assignment"
    lesson.session_id = "semantic-session"
    lesson._step_id = "semantic-step"
    lesson._seq = 0
    async def send(payload): sent.append(json.loads(payload))
    lesson._send = send
    visual_states = ("teach", "listen", "thinking", "nearMiss", "incorrect", "retry",
                     "correct", "celebrate", "completion")
    index = 0
    for item in export["lessons"]:
        for activity in item["contract"]["activities"]:
            for visual_state in visual_states:
                decision = CourseDecision(
                    f"decision-{index}", True, SessionState.WORD_ACTIVE, "QUALIFY_WIRE",
                    None, None, None, EmbodiedIntent(activity["embodiedIntent"]), False, None,
                    activity_id=activity["activityId"], visual_state=visual_state,
                    replay_entrance=visual_state == "teach",
                )
                await lesson._send_course_activity_decision(decision, delivery_id=f"delivery-{index}")
                index += 1
    json.dump(sent, open(sys.argv[2], "w", encoding="utf-8"), separators=(",", ":"))

asyncio.run(main())
'''
    env = {**os.environ, "PYTHONPATH": str(server)}
    result = subprocess.run(
        ["python3", "-c", program, str(source), str(output)],
        cwd=server, env=env, text=True, capture_output=True, timeout=120, check=False,
    )
    assert result.returncode == 0, result.stdout + result.stderr
    return json.loads(output.read_text(encoding="utf-8"))


def test_all_26_canonical_contracts_use_firmware_supported_semantics(tmp_path: Path) -> None:
    export = _canonical_curriculum(tmp_path)
    lessons = export["lessons"]
    assert export["courseKey"] == "english-6month-4-6"
    assert len(lessons) == 26
    assert export["activityCount"] == 256

    for lesson in lessons:
        contract = lesson["contract"]
        assert contract["renderer"] == {
            "rendererId": SUPPORTED_RENDERER,
            "visualLayoutContract": SUPPORTED_LAYOUT,
        }
        activity_ids = {activity["activityId"] for activity in contract["activities"]}
        assert activity_ids
        for activity in contract["activities"]:
            assert set(activity["outcomes"])
            assert {
                outcome["action"] for outcome in activity["outcomes"].values()
            } <= SUPPORTED_ACTIONS
        phase_activity_ids = set()
        for phase in lesson["phases"]:
            assert phase["phaseId"] in SUPPORTED_PHASES
            assert phase["templateId"] == "layeredCinematic"
            assert phase["templateVersion"] == 1
            assert phase["playbackMode"] == "once"
            phase_activity_ids.update(phase["activityIds"])
            slots = set()
            for layer in phase["layers"]:
                assert layer["layer"] in SUPPORTED_LAYERS
                assert layer["slot"] in SUPPORTED_SLOTS
                assert len(layer["sha256"]) == 64
                assert layer["bytes"] > 0
                slots.add(layer["slot"])
            assert {"backgroundScene", "robotOverlay"} <= slots
        assert phase_activity_ids == activity_ids


def test_firmware_source_declares_every_semantic_value_accepted_by_all_26() -> None:
    root = Path(__file__).resolve().parents[1]
    source = "\n".join(
        (root / path).read_text(encoding="utf-8")
        for path in (
            "main/lesson_handler.cc",
            "main/lesson_layered_cinematic_renderer.h",
        )
    )
    for token in SUPPORTED_PHASES | SUPPORTED_LAYERS | SUPPORTED_SLOTS:
        assert token in source
    for token in (SUPPORTED_RENDERER, "layeredCinematic"):
        assert token in source


def test_all_26_project_to_actual_firmware_course_activity_frames(tmp_path: Path) -> None:
    export = _canonical_curriculum(tmp_path)
    frames = _firmware_activity_frames(export, tmp_path)
    assert len(frames) == export["activityCount"] * len(SUPPORTED_VISUAL_STATES)
    canonical_activity_ids = {
        activity["activityId"]
        for lesson in export["lessons"]
        for activity in lesson["contract"]["activities"]
    }
    seen_activity_ids = set()
    for frame in frames:
        assert frame["type"] == "lesson_course_activity"
        body = frame["body"]
        assert body["contractVersion"] == "courseCompanion.v2.contract.v1"
        assert body["visualState"] in SUPPORTED_VISUAL_STATES
        assert body["embodiedIntent"] in SUPPORTED_WIRE_INTENTS
        assert body["retainStaticLayers"] is True
        assert isinstance(body["deliveryId"], str) and body["deliveryId"]
        seen_activity_ids.add(body["activityId"])
    assert seen_activity_ids == canonical_activity_ids
