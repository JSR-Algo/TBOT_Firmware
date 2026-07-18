import json
from pathlib import Path
import subprocess
import tarfile
import tempfile


ROOT = Path(__file__).resolve().parents[1]
FIXTURE = ROOT / "tests/fixtures/tvideo-teaching-word-parity.json"
CHECKER = ROOT / "scripts/check_tvideo_teaching_word_parity.mjs"
MANAGER_REPO = Path(
    "/Users/manhhodinh/Documents/TBOT/.worktrees/esp32-server-reusable-tvideo-template"
)
FAILING_MANAGER_COMMIT = "e13c7eb96761e71705f72afbb13e8154d180e1e4"


def test_teaching_word_fixture_prefers_authored_display_text():
    contract = json.loads(FIXTURE.read_text(encoding="utf-8"))

    assert contract == {
        "schemaVersion": 1,
        "contract": "tvideoTeachingWordParity",
        "managerFile": "main/manager-web/src/components/lesson/RobotLessonPreview.vue",
        "cases": [
            {
                "body": {
                    "teachingWord": {"text": "BARN", "displayText": "BARN"}
                },
                "currentStep": {"subject": "barn"},
                "expected": "BARN",
            },
            {
                "body": {"teachingWord": {"text": "COW"}},
                "currentStep": {"subject": "cow"},
                "expected": "COW",
            },
            {
                "body": {},
                "currentStep": {"subject": "barn"},
                "expected": "barn",
            },
        ],
    }


def archive_manager_commit(commit: str, destination: Path) -> None:
    archive = subprocess.run(
        ["git", "-C", str(MANAGER_REPO), "archive", commit],
        check=True,
        stdout=subprocess.PIPE,
    ).stdout
    archive_path = destination / "manager.tar"
    archive_path.write_bytes(archive)
    with tarfile.open(archive_path) as source:
        source.extractall(destination / "manager")


def test_checker_preserves_the_e13c7eb9_failure_as_a_regression_case():
    with tempfile.TemporaryDirectory(prefix="tbot-tvideo-word-") as temp:
        root = Path(temp)
        archive_manager_commit(FAILING_MANAGER_COMMIT, root)
        result = subprocess.run(
            [
                "node",
                str(CHECKER),
                "--manager-root",
                str(root / "manager"),
                "--fixture",
                str(FIXTURE),
            ],
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            check=False,
        )

    report = json.loads(result.stdout)
    assert result.returncode == 1
    assert report["check"] == "tvideoTeachingWordParity"
    assert report["status"] == "FAIL"
    assert report["actual"] == "barn"
    assert report["expected"] == "BARN"
