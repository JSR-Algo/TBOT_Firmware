import json
import subprocess
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
RUNNER = ROOT / "scripts" / "run_host_native_lesson_renderer_trace_test.sh"
FIXTURE = ROOT / "tests" / "fixtures" / "renderer-v2-manifest.json"


def test_renderer_trace_runner_has_complete_default_fixture():
    manifest = json.loads(FIXTURE.read_text(encoding="utf-8"))

    assert manifest["schemaVersion"] == "renderer-v2-trace.v1"
    assert manifest["steps"][0]["scene"].keys() >= {
        "backgroundScene",
        "teachingObject",
        "robotOverlay",
    }
    assert manifest["steps"][0]["templateProjection"]["phases"]
    assert manifest["trace"]["boundaries"]
    assert manifest["trace"]["fallbacks"]


def test_renderer_trace_runner_defaults_to_repo_fixture_without_compiling():
    result = subprocess.run(
        ["bash", "-n", str(RUNNER)],
        text=True,
        capture_output=True,
        check=False,
    )

    assert result.returncode == 0, result.stderr
    script = RUNNER.read_text(encoding="utf-8")
    assert 'FIXTURE="${1:-${ROOT}/tests/fixtures/renderer-v2-manifest.json}"' in script
    assert "usage:" not in script
