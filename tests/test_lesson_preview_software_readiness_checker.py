import json
import os
from pathlib import Path
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[1]
CHECKER = ROOT / "scripts/check_lesson_preview_software_readiness.py"
CONTRACT = ROOT / "tests/fixtures/lesson-preview-software-readiness.json"


def workspace_repo(environment_name: str, relative_path: str) -> Path:
    configured = os.environ.get(environment_name)
    if configured:
        return Path(configured)
    for parent in ROOT.parents:
        candidate = parent / relative_path
        if (candidate / ".git").exists():
            return candidate
    raise RuntimeError(f"set {environment_name} to the required canonical repository")


MANAGER_REPO = workspace_repo("TBOT_ESP32_SERVER_REPO", "robot/esp32-server")
BACKEND_REPO = workspace_repo("TBOT_BACKEND_REPO", "tbot-backend")
EVIDENCE_DIR = (
    Path(os.environ["TBOT_PREVIEW_EVIDENCE_DIR"])
    if os.environ.get("TBOT_PREVIEW_EVIDENCE_DIR")
    else MANAGER_REPO.parents[1]
    / "robot/docs/evidence/artifacts/lesson-preview-parity/20260718T064659Z/preview"
)


def test_checker_exists():
    assert CHECKER.is_file()


def run_checker(
    *extra: str, contract: Path = CONTRACT
) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [
            "python3",
            str(CHECKER),
            "--contract",
            str(contract),
            "--manager-repo",
            str(MANAGER_REPO),
            "--backend-repo",
            str(BACKEND_REPO),
            "--evidence-dir",
            str(EVIDENCE_DIR),
            *extra,
        ],
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
    )


def test_checker_attests_exact_offline_preview_bundle_and_writes_local_env():
    with tempfile.TemporaryDirectory(prefix="tbot-preview-readiness-") as temp:
        env_path = Path(temp) / "lesson-preview.env"
        result = run_checker("--compose-env-out", str(env_path))

        report = json.loads(result.stdout)
        assert result.returncode == 0, result.stderr or report
        assert report == {
            "check": "lessonPreviewSoftwareReadiness",
            "status": "PASS",
            "managerCommit": "c1f10742ea2841bc015f1f54e78679394503a948",
            "backendCommit": "4f4a8e9fa72734e7cfc3a5dd943782f6b2937048",
            "manifestChecksum": "6e44a135a457f78806e39ddf77a66c8af353fc508d2c879e0b2aa92dffd22b79",
            "assetCount": 20,
            "preview": {"width": 480, "height": 320, "word": "BARN"},
            "composeEnvSha256": report["composeEnvSha256"],
        }
        assert env_path.read_text(encoding="utf-8") == (
            "LESSON_ASSET_ORIGIN_BASE=http://127.0.0.1:18102/tvideo-demo\n"
            "LESSON_ASSET_PACK_MOUNT_ROOT=/opt/tbot-esp32-server/data/lesson-assets\n"
            "LESSON_ASSET_PUBLIC_BASE_URL=http://127.0.0.1:18003\n"
            "LESSON_SAMPLE_ENABLED=false\n"
        )


def test_checker_fails_closed_without_required_arguments():
    result = subprocess.run(
        ["python3", str(CHECKER)],
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
    )

    assert result.returncode == 2
    assert json.loads(result.stdout) == {
        "check": "lessonPreviewSoftwareReadiness",
        "status": "ERROR",
        "reason": (
            "required arguments: --contract, --manager-repo, --backend-repo, "
            "--evidence-dir"
        ),
    }


def test_checker_rejects_local_bootstrap_that_reuses_asset_origin_as_public_base():
    with tempfile.TemporaryDirectory(prefix="tbot-preview-bootstrap-") as temp:
        temp_root = Path(temp)
        contract = json.loads(CONTRACT.read_text(encoding="utf-8"))
        contract["localBootstrap"]["LESSON_ASSET_PUBLIC_BASE_URL"] = contract[
            "localBootstrap"
        ]["LESSON_ASSET_ORIGIN_BASE"]
        contract_path = temp_root / "contract.json"
        contract_path.write_text(json.dumps(contract), encoding="utf-8")

        result = run_checker(contract=contract_path)

    assert result.returncode == 1
    assert json.loads(result.stdout) == {
        "check": "lessonPreviewSoftwareReadiness",
        "status": "FAIL",
        "reason": "local origin and runtime public base must be distinct",
    }
