import json
import os
from pathlib import Path
import subprocess
import tarfile
import tempfile


ROOT = Path(__file__).resolve().parents[1]
FIXTURE = ROOT / "tests/fixtures/tvideo-static-fallback-immediate-reveal.json"
CHECKER = ROOT / "scripts/check_tvideo_static_fallback_parity.mjs"


def manager_repo() -> Path:
    configured = os.environ.get("TBOT_ESP32_SERVER_REPO")
    if configured:
        return Path(configured)
    for parent in ROOT.parents:
        candidate = parent / "robot/esp32-server"
        if (candidate / ".git").exists():
            return candidate
    raise RuntimeError("set TBOT_ESP32_SERVER_REPO to the canonical manager repository")


MANAGER_REPO = manager_repo()
OLD_MANAGER_COMMIT = "edec89d88e3b81219b5fde40cb7b9d7e1ee13a07"
CORRECTED_MANAGER_COMMIT = "e13c7eb96761e71705f72afbb13e8154d180e1e4"


def test_static_fallback_fixture_locks_portable_behavior():
    contract = json.loads(FIXTURE.read_text(encoding="utf-8"))

    assert contract["schemaVersion"] == 1
    assert contract["managerFiles"] == {
        "layouts": "main/manager-web/src/components/lesson/tvideo-layout-presets.js",
        "preview": "main/manager-web/src/components/lesson/TvideoJourneyPreview.vue",
    }
    assert contract["fallbackPhase"] == "arriveNear"
    assert contract["revealPhase"] == "revealTeachingContent"
    assert contract["behaviorCases"] == [
        {
            "phase": "arriveNear",
            "staticFallback": True,
            "contentVisible": True,
        },
        {
            "phase": "arriveNear",
            "staticFallback": False,
            "contentVisible": False,
        },
        {
            "phase": "revealTeachingContent",
            "staticFallback": False,
            "contentVisible": True,
        },
    ]


def archive_manager_commit(commit: str, destination: Path) -> None:
    archive = subprocess.run(
        ["git", "-C", str(MANAGER_REPO), "archive", commit],
        check=True,
        stdout=subprocess.PIPE,
    ).stdout
    archive_path = destination / "manager.tar"
    archive_path.write_bytes(archive)
    with tarfile.open(archive_path) as source:
        manager_files = json.loads(FIXTURE.read_text(encoding="utf-8"))["managerFiles"]
        required = set(manager_files.values())
        target_root = destination / "manager"
        for member in source:
            if member.name not in required:
                continue
            if not member.isfile() or member.name.startswith("/") or ".." in Path(member.name).parts:
                raise RuntimeError(f"unsafe manager archive member: {member.name}")
            payload = source.extractfile(member)
            if payload is None:
                raise RuntimeError(f"missing manager archive member: {member.name}")
            target = target_root / member.name
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(payload.read())
        for required_path in required:
            if not (target_root / required_path).is_file():
                raise RuntimeError(f"missing required manager file: {required_path}")


def run_checker(manager_root: Path) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [
            "node",
            str(CHECKER),
            "--manager-root",
            str(manager_root),
            "--fixture",
            str(FIXTURE),
        ],
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
    )


def test_checker_proves_old_commit_red_and_corrected_commit_green():
    with tempfile.TemporaryDirectory(prefix="tbot-tvideo-parity-") as temp:
        root = Path(temp)
        old_root = root / "old"
        corrected_root = root / "corrected"
        old_root.mkdir()
        corrected_root.mkdir()
        archive_manager_commit(OLD_MANAGER_COMMIT, old_root)
        archive_manager_commit(CORRECTED_MANAGER_COMMIT, corrected_root)

        old_result = run_checker(old_root / "manager")
        corrected_result = run_checker(corrected_root / "manager")

    old_report = json.loads(old_result.stdout)
    corrected_report = json.loads(corrected_result.stdout)
    assert old_result.returncode == 1
    assert old_report["status"] == "FAIL"
    assert old_report["check"] == "staticFallbackImmediateRevealParity"
    assert corrected_result.returncode == 0
    assert corrected_report == {
        "check": "staticFallbackImmediateRevealParity",
        "status": "PASS",
        "fallbackPhase": "arriveNear",
        "revealPhase": "revealTeachingContent",
        "behaviorCases": 3,
    }


def test_checker_fails_closed_when_required_arguments_are_missing():
    result = subprocess.run(
        ["node", str(CHECKER)],
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
    )

    assert result.returncode == 2
    assert json.loads(result.stdout) == {
        "check": "staticFallbackImmediateRevealParity",
        "status": "ERROR",
        "reason": "required arguments: --manager-root <path> --fixture <path>",
    }
