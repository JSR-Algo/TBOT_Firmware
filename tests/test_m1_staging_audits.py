import subprocess
import sys

import pytest

from test_lcdwiki_es3c35p_board import ROOT, lcdwiki_reference_sdkconfig
from test_lesson_storage_hil_artifact_auditor import AUDITOR


def test_production_config_audit_rejects_staging_flag(tmp_path):
    config = tmp_path / "sdkconfig"
    config.write_text(lcdwiki_reference_sdkconfig() + "\nCONFIG_TBOT_M1_STAGING=y\n")
    result = subprocess.run([sys.executable, str(ROOT / "scripts/assert_lcdwiki_prod_config.py"), str(config)], capture_output=True)
    assert result.returncode != 0


def test_production_artifact_audit_rejects_staging_flag():
    with pytest.raises(AUDITOR.AuditFailure):
        AUDITOR.audit_profile_configuration("production", {
            "CONFIG_TBOT_RELEASE_CINEMATIC_EVIDENCE": "y",
            "CONFIG_TBOT_M1_STAGING": "y",
        })


@pytest.mark.parametrize("profile", ["production", "m1-staging"])
def test_admission_artifact_audit_rejects_voice_demo(profile):
    with pytest.raises(AUDITOR.AuditFailure, match="CONFIG_TBOT_VOICE_DEMO"):
        AUDITOR.audit_profile_configuration(profile, {
            "CONFIG_TBOT_RELEASE_CINEMATIC_EVIDENCE": "y",
            "CONFIG_TBOT_M1_STAGING": "y" if profile == "m1-staging" else "n",
            "CONFIG_TBOT_VOICE_DEMO": "y",
        })


def test_admission_audits_share_attended_only_flags():
    sys.path.insert(0, str(ROOT / "scripts"))
    try:
        from assert_lcdwiki_prod_config import ATTENDED_ONLY_FLAGS
    finally:
        sys.path.remove(str(ROOT / "scripts"))
    assert AUDITOR.ATTENDED_ONLY_FLAGS == ATTENDED_ONLY_FLAGS


def test_staging_audit_requires_its_own_embedded_identity():
    artifacts = {name: b"TBOT_EMBEDDED_PROFILE=m1-staging-v1\0" for name in ("bin", "elf", "mainArchive")}
    snapshots = {name: type("Snapshot", (), {"data": blob})() for name, blob in artifacts.items()}
    # Audit the real extracted literal validator with byte-backed artifacts.
    original = AUDITOR.artifact_bytes
    try:
        AUDITOR.artifact_bytes = lambda value: value.data
        assert AUDITOR.audit_profile_literals("m1-staging", snapshots) == "m1-staging-v1"
        with pytest.raises(AUDITOR.AuditFailure):
            AUDITOR.audit_profile_literals("production", snapshots)
    finally:
        AUDITOR.artifact_bytes = original
