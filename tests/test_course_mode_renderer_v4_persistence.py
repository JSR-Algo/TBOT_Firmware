import hashlib
import json
import os
import re
from pathlib import Path


FIXTURE = Path(__file__).parent / "fixtures" / "course-mode" / "course-mode-pilot-cat-ball.persistence-v1.json"
FIXTURE_SHA256 = "b98afc2dd46026dcdfe525c024f52d6773c79570c620967d55ee776f998a3e27"
BACKEND_FIXTURE = Path("src/lessons/fixtures/course-mode/pilot/v1/course-mode-pilot-cat-ball.persistence-v1.json")
ESP_FIXTURE = Path("main/tbot-server/tests/fixtures/course-mode/course-mode-pilot-cat-ball.persistence-v1.json")
EXPECTED_CUES = [
    "cat-discover", "cat-meaning", "cat-joint-speech", "cat-recall",
    "cat-transfer", "ball-discover", "ball-meaning", "cat-delayed",
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
