import hashlib
import json
import os
import re
from pathlib import Path


FIXTURE = Path(__file__).parent / "fixtures" / "course-mode" / "course-mode-pilot-cat-ball.persistence-v1.json"
V2_FIXTURE = Path(__file__).parent / "fixtures" / "course-mode" / "course-mode-pilot-cat-ball-v2.json"
FIXTURE_SHA256 = "b98afc2dd46026dcdfe525c024f52d6773c79570c620967d55ee776f998a3e27"
V2_FIXTURE_SHA256 = "5b3c27e6281a30a01bed3db288507f4ebfa1439f927ec331b65da23c9d41ff5f"
BACKEND_FIXTURE = Path("src/lessons/fixtures/course-mode/pilot/v1/course-mode-pilot-cat-ball.persistence-v1.json")
ESP_FIXTURE = Path("main/tbot-server/tests/fixtures/course-mode/course-mode-pilot-cat-ball.persistence-v1.json")
EXPECTED_CUES = [
    "cat-discover", "cat-meaning", "cat-joint-speech", "cat-recall",
    "cat-transfer", "ball-discover", "ball-meaning", "cat-delayed",
]
EXPECTED_V2_CUES = [
    ("cat-discover", "teach", "cat-discover-center-01"),
    ("cat-meaning", "listen", "cat-meaning-left-right-01"),
    ("cat-joint-speech", "teach", "cat-discover-center-01"),
    ("cat-recall", "listen", "cat-recall-visual-02"),
    ("cat-transfer", "listen", "cat-transfer-scene-01"),
    ("ball-discover", "teach", "ball-discover-center-01"),
    ("ball-meaning", "listen", "ball-discover-center-01"),
    ("cat-delayed", "listen", "cat-delayed-recall-01"),
]


def test_course_mode_renderer_v4_persistence_fixture_is_frozen() -> None:
    raw = FIXTURE.read_bytes()
    assert hashlib.sha256(raw).hexdigest() == FIXTURE_SHA256
    fixture = json.loads(raw)
    assert fixture["identity"]["semanticChecksum"] == "cf12b1a5f71f0a80a8ee22bb2cdc775ada5b803e26d154e5d29c76b14c9fb264"
    assert fixture["identity"]["layoutChecksum"] == "e61b56d1f8219a86c7f3986e7d5c70b91f512286604b5b206ef11e2c989d275c"
    assert fixture["identity"]["rendererId"] == "teebot-lesson-renderer.v4"
    assert fixture["identity"]["templateVersion"] == 2
    assert [cue["cueId"] for cue in fixture["cues"]] == EXPECTED_CUES
    assert all(cue["derivative"]["width"] == 480 for cue in fixture["cues"])
    assert all(cue["derivative"]["height"] == 320 for cue in fixture["cues"])
    assert all(cue["derivative"]["fps"] == 10 for cue in fixture["cues"])
    assert all(cue["derivative"]["durationMs"] == 2000 for cue in fixture["cues"])


def test_firmware_course_mode_compatibility_gate_matches_frozen_fixture() -> None:
    fixture = json.loads(FIXTURE.read_text(encoding="utf-8"))
    source = (
        Path(__file__).resolve().parents[1] / "main" / "lesson_flattened_cinematic_renderer.cc"
    ).read_text(encoding="utf-8")
    assert fixture["identity"]["semanticChecksum"] in source
    assert "renderer-v4.course-mode-layout.v1" in source
    assert "course-mode-pilot-cat-ball" in source
    assert "205784b3f97cb081ce9c226d8fd83fdd400401e706c000e1b09ba4e7ebdf36ce" in source

    source_cues = {
        cue_id: (effect, derivative_id, sha256, int(byte_count))
        for cue_id, effect, derivative_id, sha256, byte_count in re.findall(
            r'\{"([a-z0-9-]+)", "([a-z0-9-]+)", "([0-9a-f]{64})", '
            r'"([0-9a-f]{64})", ([0-9]+)\}',
            source,
        )
    }
    expected_cues = {
        cue["cueId"]: (
            cue["effect"],
            cue["derivative"]["derivativeId"],
            cue["derivative"]["sha256"],
            cue["derivative"]["bytes"],
        )
        for cue in fixture["cues"]
    }
    assert source_cues == expected_cues


def test_course_mode_renderer_v5_fixture_is_frozen_and_exact() -> None:
    raw = V2_FIXTURE.read_bytes()
    assert hashlib.sha256(raw).hexdigest() == V2_FIXTURE_SHA256
    fixture = json.loads(raw)
    assert fixture["renderer"] == "teebot-lesson-renderer.v5"
    assert fixture["bundle"]["checksum"] == "e8ee7ff1fb67e8dbd0f8c6908b09c4a4f8e0d1cf3ce41bb38142da0fc03519dc"
    assert fixture["contractChecksum"] == "cf12b1a5f71f0a80a8ee22bb2cdc775ada5b803e26d154e5d29c76b14c9fb264"
    assert [(cue["cueId"], cue["phaseId"], cue["activityId"]) for cue in fixture["cuePhases"]] == EXPECTED_V2_CUES
    phases = {phase["phaseId"]: phase for phase in fixture["phaseIdentity"]}
    assert set(phases) == {"teach", "listen"}
    for phase in phases.values():
        assert phase["templateId"] == "layeredCinematic"
        assert phase["templateVersion"] == 1
        assert phase["timing"] == {"durationMs": 3000}
        background, teaching_object, robot = phase["layers"]
        assert background["assetVersionId"] == "75000000-0000-4000-8000-000000000011"
        assert background["sha256"] == "d4abb6087dc3122e0a00feb5e6a86b03dc7db550eb59d25e92f54d0fd09e4fc0"
        assert background["bytes"] == 43599
        assert background["metadata"]["rect"] == {"x": 0, "y": 0, "width": 480, "height": 320}
        assert background["metadata"]["fit"] == "cover"
        assert teaching_object["assetVersionId"] == "75000000-0000-4000-8000-000000000022"
        assert teaching_object["sha256"] == "c466239ff8ba202998e3827b6871906d7fbac6232aeaea3a59b7c69bec7d8777"
        assert teaching_object["bytes"] == 15086
        assert teaching_object["metadata"]["rect"] == {"x": 20, "y": 168, "width": 95, "height": 95}
        assert teaching_object["metadata"]["fit"] == "contain"
        assert robot["assetVersionId"] == "75000000-0000-4000-8000-000000000031"
        assert robot["sha256"] == "f2d496b5e750e895f7e086aec827d7b99d0bb322d73ea660a2e84ff484b602c4"
        assert robot["bytes"] == 223033
        assert robot["metadata"]["rect"] == {"x": 118, "y": 160, "width": 150, "height": 150}
        assert robot["metadata"]["codec"] == "mjpeg"
        assert robot["metadata"]["fps"] == 10
        assert robot["metadata"]["frameCount"] == 30
        assert robot["metadata"]["durationMs"] == 3000
        assert robot["metadata"]["hasAudio"] is False
        assert robot["metadata"]["chromaKey"] == {"keyColor": "#00ff00", "tolerance": 20, "featherPx": 1}


def test_firmware_course_mode_v5_identity_gate_matches_reviewed_fixture() -> None:
    fixture = json.loads(V2_FIXTURE.read_text(encoding="utf-8"))
    source = (
        Path(__file__).resolve().parents[1] / "main" / "lesson_handler.cc"
    ).read_text(encoding="utf-8")
    renderer_source = (
        Path(__file__).resolve().parents[1] / "main" / "lesson_flattened_cinematic_renderer.cc"
    ).read_text(encoding="utf-8")

    assert fixture["manifestIdentityChecksum"] in source + renderer_source
    assert fixture["bundle"]["checksum"] in source + renderer_source
    assert fixture["contractChecksum"] in source + renderer_source
    assert fixture["lesson"]["key"] in source + renderer_source
    assert fixture["template"] in source + renderer_source
    for phase in fixture["phaseIdentity"]:
        for layer in phase["layers"]:
            assert layer["assetVersionId"] in source + renderer_source
            assert layer["sha256"] in source + renderer_source
            assert str(layer["bytes"]) in source + renderer_source


def test_firmware_ci_runs_local_fixture_gate_without_cross_repo_dependencies() -> None:
    workflow = (Path(__file__).resolve().parents[1] / ".github" / "workflows" / "build.yml").read_text(
        encoding="utf-8"
    )
    local_gate = (
        "tests/test_course_mode_renderer_v4_persistence.py::"
        "test_course_mode_renderer_v4_persistence_fixture_is_frozen"
    )
    compatibility_gate = (
        "tests/test_course_mode_renderer_v4_persistence.py::"
        "test_firmware_course_mode_compatibility_gate_matches_frozen_fixture"
    )
    ci_scope_gate = (
        "tests/test_course_mode_renderer_v4_persistence.py::"
        "test_firmware_ci_runs_local_fixture_gate_without_cross_repo_dependencies"
    )
    assert local_gate in workflow
    assert compatibility_gate in workflow
    assert ci_scope_gate in workflow
    assert "tests/test_course_mode_renderer_v4_persistence.py \\" not in workflow


def _required_authority_fixture_paths(test_file: Path = Path(__file__)) -> list[Path]:
    workspace = next((parent for parent in test_file.resolve().parents if parent.name == "TBOT"), None)
    if workspace is None:
        assert "TBOT_BACKEND_WORKTREE" in os.environ and "TBOT_ESP_WORKTREE" in os.environ, (
            "TBOT workspace parent or both explicit authority roots are required"
        )
        backend_root = Path(os.environ["TBOT_BACKEND_WORKTREE"])
        esp_root = Path(os.environ["TBOT_ESP_WORKTREE"])
    else:
        backend_root = workspace / "tbot-backend"
        esp_root = workspace / "robot" / "esp32-server"
    return [
        _authority_fixture_path(
            "TBOT_BACKEND_WORKTREE",
            backend_root,
            BACKEND_FIXTURE,
        ),
        _authority_fixture_path(
            "TBOT_ESP_WORKTREE",
            esp_root,
            ESP_FIXTURE,
        ),
    ]


def _authority_fixture_path(env_var: str, canonical_root: Path, fixture_relative: Path) -> Path:
    if env_var in os.environ:
        fixture = Path(os.environ[env_var]) / fixture_relative
        assert fixture.is_file(), f"{env_var} fixture is missing: {fixture}"
        return fixture

    canonical_fixture = canonical_root / fixture_relative
    if canonical_fixture.is_file():
        return canonical_fixture

    matches = sorted(
        path
        for path in (canonical_root / ".worktrees").glob(f"*/{fixture_relative.as_posix()}")
        if path.is_file()
    )
    assert len(matches) == 1, (
        f"expected exactly one {canonical_root.name} authority fixture in "
        f"{canonical_root / '.worktrees'}, found {len(matches)}: "
        + ", ".join(str(path) for path in matches)
    )
    return matches[0]


def test_course_mode_renderer_v4_persistence_matches_available_authorities() -> None:
    for authority in _required_authority_fixture_paths():
        assert authority.read_bytes() == FIXTURE.read_bytes()


def test_explicit_authority_roots_work_outside_a_tbot_workspace(tmp_path, monkeypatch) -> None:
    backend = tmp_path / "backend"
    esp = tmp_path / "esp"
    backend_fixture = backend / BACKEND_FIXTURE
    esp_fixture = esp / ESP_FIXTURE
    backend_fixture.parent.mkdir(parents=True)
    esp_fixture.parent.mkdir(parents=True)
    backend_fixture.write_bytes(FIXTURE.read_bytes())
    esp_fixture.write_bytes(FIXTURE.read_bytes())
    monkeypatch.setenv("TBOT_BACKEND_WORKTREE", str(backend))
    monkeypatch.setenv("TBOT_ESP_WORKTREE", str(esp))

    assert _required_authority_fixture_paths(tmp_path / "standalone" / "test.py") == [
        backend_fixture,
        esp_fixture,
    ]
