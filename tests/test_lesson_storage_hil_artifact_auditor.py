import importlib.util
import os
import stat
from pathlib import Path

import pytest


ROOT = Path(__file__).resolve().parents[1]
SCRIPT = ROOT / "scripts/assert_lesson_storage_hil_artifacts.py"
SPEC = importlib.util.spec_from_file_location("hil_artifact_auditor", SCRIPT)
AUDITOR = importlib.util.module_from_spec(SPEC)
assert SPEC.loader is not None
SPEC.loader.exec_module(AUDITOR)


def test_parse_size_and_partition_metrics(tmp_path):
    repo = tmp_path / "repo"
    repo.mkdir()
    partition = repo / "partitions.csv"
    partition.write_text(
        "# Name,Type,SubType,Offset,Size,Flags\n"
        "ota_0,app,ota_0,0x20000,0x1000,\n"
        "ota_1,app,ota_1,,4K,\n",
        encoding="utf-8",
    )
    image = tmp_path / "xiaozhi.bin"
    image.write_bytes(b"x" * 1024)

    metrics = AUDITOR.app_partition_metrics(
        repo, {"CONFIG_PARTITION_TABLE_FILENAME": "partitions.csv"}, image
    )

    assert metrics["partitionBytes"] == 4096
    assert metrics["imageBytes"] == 1024
    assert metrics["freeBytes"] == 3072
    assert metrics["freePercent"] == 75.0


def test_literal_and_symbol_profiles_are_inverse(tmp_path):
    artifacts = {}
    hil_blob = "\n".join((*AUDITOR.TOOL_NAMES, AUDITOR.BANNER)).encode("ascii")
    for name in ("bin", "elf", "map", "mainArchive"):
        path = tmp_path / name
        path.write_bytes(hil_blob if name != "map" else b"map")
        artifacts[name] = path

    AUDITOR.audit_literals("hil", artifacts)
    with pytest.raises(AUDITOR.AuditFailure):
        AUDITOR.audit_literals("production", artifacts)

    symbols = "\n".join(AUDITOR.HIL_SYMBOLS)
    AUDITOR.audit_symbols("hil", symbols)
    with pytest.raises(AUDITOR.AuditFailure):
        AUDITOR.audit_symbols("production", symbols)
    with pytest.raises(AUDITOR.AuditFailure):
        AUDITOR.audit_symbols("hil", symbols + "\n U openat\n")


def test_resolve_nm_rejects_compiler_sibling_from_untrusted_build_metadata(
    tmp_path, monkeypatch
):
    attacker = tmp_path / "attacker"
    attacker.mkdir()
    compiler = attacker / "xtensa-esp32s3-elf-gcc"
    nm = attacker / "xtensa-esp32s3-elf-nm"
    compiler.write_text("#!/bin/sh\nexit 0\n", encoding="utf-8")
    nm.write_text("#!/bin/sh\necho forged symbols\n", encoding="utf-8")
    compiler.chmod(compiler.stat().st_mode | stat.S_IXUSR)
    nm.chmod(nm.stat().st_mode | stat.S_IXUSR)
    trusted_tools = tmp_path / "trusted-tools"
    trusted_tools.mkdir()
    monkeypatch.setenv("IDF_TOOLS_PATH", str(trusted_tools))
    monkeypatch.setenv("PATH", str(attacker))

    with pytest.raises(AUDITOR.AuditFailure, match="trusted ESP target nm"):
        AUDITOR.resolve_nm()


def test_resolve_nm_rejects_explicit_executable_outside_idf_tools_root(
    tmp_path, monkeypatch
):
    trusted_tools = tmp_path / "trusted-tools"
    trusted_tools.mkdir()
    attacker_nm = tmp_path / "attacker" / "xtensa-esp32s3-elf-nm"
    attacker_nm.parent.mkdir()
    attacker_nm.write_text("#!/bin/sh\necho forged symbols\n", encoding="utf-8")
    attacker_nm.chmod(attacker_nm.stat().st_mode | stat.S_IXUSR)
    monkeypatch.setenv("IDF_TOOLS_PATH", str(trusted_tools))

    with pytest.raises(AUDITOR.AuditFailure, match="escapes IDF tools root"):
        AUDITOR.resolve_nm(str(attacker_nm))


def test_resolve_nm_accepts_and_snapshots_explicit_executable_under_idf_tools_root(
    tmp_path, monkeypatch
):
    trusted_tools = tmp_path / "trusted-tools"
    trusted_nm = (
        trusted_tools
        / "tools/xtensa-esp-elf/version/xtensa-esp-elf/bin"
        / "xtensa-esp32s3-elf-nm"
    )
    trusted_nm.parent.mkdir(parents=True)
    trusted_nm.write_text("#!/bin/sh\necho trusted symbols\n", encoding="utf-8")
    trusted_nm.chmod(trusted_nm.stat().st_mode | stat.S_IXUSR)
    monkeypatch.setenv("IDF_TOOLS_PATH", str(trusted_tools))

    snapshot = AUDITOR.resolve_nm(str(trusted_nm))

    assert snapshot.path == trusted_nm.resolve()
    assert snapshot.data == trusted_nm.read_bytes()


def test_nm_snapshot_detects_executable_replacement_during_symbol_audit(
    tmp_path, monkeypatch
):
    trusted_tools = tmp_path / "trusted-tools"
    trusted_nm = trusted_tools / "xtensa-esp32s3-elf-nm"
    trusted_nm.parent.mkdir()
    trusted_nm.write_text("#!/bin/sh\necho trusted symbols\n", encoding="utf-8")
    trusted_nm.chmod(trusted_nm.stat().st_mode | stat.S_IXUSR)
    monkeypatch.setenv("IDF_TOOLS_PATH", str(trusted_tools))
    snapshot = AUDITOR.resolve_nm(str(trusted_nm))
    archive = tmp_path / "libmain.a"
    archive.write_bytes(b"archive")
    archive_snapshot = AUDITOR.snapshot_artifact(tmp_path, archive, "mainArchive")

    def replace_nm(_command, _cwd):
        replacement = tmp_path / "replacement-nm"
        replacement.write_text("#!/bin/sh\necho forged symbols\n", encoding="utf-8")
        replacement.chmod(replacement.stat().st_mode | stat.S_IXUSR)
        os.replace(replacement, trusted_nm)
        return "trusted symbols\n"

    monkeypatch.setattr(AUDITOR, "run", replace_nm)
    with pytest.raises(AUDITOR.AuditFailure, match="changed during audit: nmExecutable"):
        AUDITOR.run_nm_on_snapshot(snapshot, archive_snapshot, tmp_path)


def test_artifact_snapshot_detects_deterministic_path_replacement(tmp_path):
    build_dir = tmp_path / "build"
    build_dir.mkdir()
    artifact = build_dir / "xiaozhi.bin"
    artifact.write_bytes(b"audited")

    assert hasattr(AUDITOR, "snapshot_artifact")
    snapshot = AUDITOR.snapshot_artifact(build_dir, artifact, "bin")
    replacement = build_dir / "replacement"
    replacement.write_bytes(b"forged")
    os.replace(replacement, artifact)

    with pytest.raises(AUDITOR.AuditFailure, match="changed during audit"):
        AUDITOR.verify_artifact_snapshot(snapshot)


def test_literal_and_nm_checks_use_the_same_pinned_archive_bytes(tmp_path):
    build_dir = tmp_path / "build"
    build_dir.mkdir()
    archive = build_dir / "libmain.a"
    original = "\n".join((*AUDITOR.TOOL_NAMES, AUDITOR.BANNER)).encode("ascii")
    archive.write_bytes(original)
    snapshot = AUDITOR.snapshot_artifact(build_dir, archive, "mainArchive")
    archive.write_bytes(b"forged replacement")

    artifacts = {name: snapshot for name in ("bin", "elf", "map", "mainArchive")}
    AUDITOR.audit_literals("hil", artifacts)

    nm = tmp_path / "xtensa-esp32s3-elf-nm"
    nm.write_text("#!/bin/sh\ncat \"$2\"\n", encoding="utf-8")
    nm.chmod(nm.stat().st_mode | stat.S_IXUSR)
    nm_snapshot = AUDITOR.snapshot_artifact(tmp_path, nm, "nmExecutable")
    output = AUDITOR.run_nm_on_snapshot(nm_snapshot, snapshot, tmp_path)
    assert output.encode("ascii") == original


def test_verify_source_state_rechecks_head_and_cleanliness(monkeypatch, tmp_path):
    responses = iter(("a" * 40 + "\n", ""))
    calls = []

    def fake_run(command, cwd):
        calls.append((command, cwd))
        return next(responses)

    monkeypatch.setattr(AUDITOR, "run", fake_run)
    AUDITOR.verify_source_state(tmp_path, "a" * 40)

    assert calls == [
        (["git", "rev-parse", "HEAD"], tmp_path),
        (["git", "status", "--porcelain"], tmp_path),
    ]


def test_interrupted_checksum_first_publish_removes_both_outputs(tmp_path):
    manifest = tmp_path / AUDITOR.MANIFEST_NAME
    checksum = tmp_path / AUDITOR.SHA_NAME

    def interrupt(phase):
        if phase == "after_checksum":
            raise RuntimeError("interrupted")

    with pytest.raises(RuntimeError, match="interrupted"):
        AUDITOR.publish_manifest_pair(
            manifest, checksum, b'{"status":"PASS"}\n', lambda: None, interrupt
        )

    assert not manifest.exists()
    assert not checksum.exists()


@pytest.mark.parametrize("mutation_phase", ("after_checksum", "after_manifest"))
def test_publication_recheck_rejects_artifact_mutation_and_removes_pair(
    tmp_path, mutation_phase
):
    build_dir = tmp_path / "build"
    build_dir.mkdir()
    artifact = build_dir / "xiaozhi.bin"
    artifact.write_bytes(b"audited")
    snapshot = AUDITOR.snapshot_artifact(build_dir, artifact, "bin")
    manifest = build_dir / AUDITOR.MANIFEST_NAME
    checksum = build_dir / AUDITOR.SHA_NAME

    def mutate(phase):
        if phase == mutation_phase:
            replacement = build_dir / "replacement"
            replacement.write_bytes(b"forged")
            os.replace(replacement, artifact)

    with pytest.raises(AUDITOR.AuditFailure, match="changed during audit"):
        AUDITOR.publish_manifest_pair(
            manifest,
            checksum,
            b'{"status":"PASS"}\n',
            lambda: AUDITOR.verify_artifact_snapshot(snapshot),
            mutate,
        )

    assert not manifest.exists()
    assert not checksum.exists()


def test_published_pass_requires_a_coherent_manifest_checksum_pair(tmp_path):
    manifest = tmp_path / AUDITOR.MANIFEST_NAME
    checksum = tmp_path / AUDITOR.SHA_NAME
    manifest_bytes = b'{"status":"PASS"}\n'

    AUDITOR.publish_manifest_pair(
        manifest, checksum, manifest_bytes, lambda: None
    )
    AUDITOR.verify_published_manifest_pair(manifest, checksum)

    checksum.write_text("0" * 64 + f"  {AUDITOR.MANIFEST_NAME}\n", encoding="ascii")
    with pytest.raises(AUDITOR.AuditFailure, match="coherent"):
        AUDITOR.verify_published_manifest_pair(manifest, checksum)


def test_embedded_profile_is_boolean_derived_and_manual_override_is_rejected(tmp_path):
    AUDITOR.audit_profile_configuration(
        "hil", {"CONFIG_TBOT_HIL_STORAGE_FAULTS": "y"}
    )
    AUDITOR.audit_profile_configuration(
        "production", {"CONFIG_TBOT_RELEASE_CINEMATIC_EVIDENCE": "y"}
    )
    with pytest.raises(AUDITOR.AuditFailure, match="manual profile override forbidden"):
        AUDITOR.audit_profile_configuration(
            "hil",
            {
                "CONFIG_TBOT_HIL_STORAGE_FAULTS": "y",
                "CONFIG_TBOT_HIL_PROFILE": "production",
            },
        )

    artifacts = {}
    for name in ("bin", "elf", "mainArchive"):
        path = tmp_path / name
        path.write_bytes(b"TBOT_EMBEDDED_PROFILE=task14-hil-v1")
        artifacts[name] = path
    AUDITOR.audit_profile_literals("hil", artifacts)
    with pytest.raises(AUDITOR.AuditFailure, match="embedded profile literal mismatch"):
        AUDITOR.audit_profile_literals("production", artifacts)


@pytest.mark.parametrize(
    "payload",
    (
        b"TBOT_EMBEDDED_PROFILE=production-debug",
        b"TBOT_EMBEDDED_PROFILE=production TBOT_EMBEDDED_PROFILE=development",
        b"TBOT_EMBEDDED_PROFILE=production TBOT_EMBEDDED_PROFILE=production",
        b"TBOT_EMBEDDED_PROFILE=",
        b"no embedded profile",
    ),
)
def test_production_embedded_profile_requires_one_exact_token_per_artifact(
    tmp_path, payload
):
    artifacts = {}
    for name in ("bin", "elf", "mainArchive"):
        path = tmp_path / name
        path.write_bytes(payload)
        artifacts[name] = path

    with pytest.raises(AUDITOR.AuditFailure, match="embedded profile literal mismatch"):
        AUDITOR.audit_profile_literals("production", artifacts)


def test_hil_embedded_profile_rejects_extra_profile_token(tmp_path):
    artifacts = {}
    for name in ("bin", "elf", "mainArchive"):
        path = tmp_path / name
        path.write_bytes(
            b"TBOT_EMBEDDED_PROFILE=task14-hil-v1 "
            b"TBOT_EMBEDDED_PROFILE=production"
        )
        artifacts[name] = path

    with pytest.raises(AUDITOR.AuditFailure, match="embedded profile literal mismatch"):
        AUDITOR.audit_profile_literals("hil", artifacts)


def test_production_profile_requires_release_evidence_and_disables_both_hil_flags():
    expected = {
        "releaseCinematicEvidence": True,
        "hilCinematicTelemetry": False,
        "hilStorageFaults": False,
    }

    assert AUDITOR.audit_profile_configuration(
        "production", {"CONFIG_TBOT_RELEASE_CINEMATIC_EVIDENCE": "y"}
    ) == expected

    with pytest.raises(AUDITOR.AuditFailure, match="release cinematic evidence"):
        AUDITOR.audit_profile_configuration("production", {})
    with pytest.raises(AUDITOR.AuditFailure, match="cinematic telemetry"):
        AUDITOR.audit_profile_configuration(
            "production",
            {
                "CONFIG_TBOT_RELEASE_CINEMATIC_EVIDENCE": "y",
                "CONFIG_TBOT_HIL_CINEMATIC_TELEMETRY": "y",
            },
        )
    with pytest.raises(AUDITOR.AuditFailure, match="storage faults"):
        AUDITOR.audit_profile_configuration(
            "production",
            {
                "CONFIG_TBOT_RELEASE_CINEMATIC_EVIDENCE": "y",
                "CONFIG_TBOT_HIL_STORAGE_FAULTS": "y",
            },
        )


def test_storage_hil_profile_does_not_require_release_cinematic_evidence():
    assert AUDITOR.audit_profile_configuration(
        "hil", {"CONFIG_TBOT_HIL_STORAGE_FAULTS": "y"}
    ) == {
        "releaseCinematicEvidence": False,
        "hilCinematicTelemetry": False,
        "hilStorageFaults": True,
    }


def test_manifest_checks_record_release_and_hil_config_state():
    checks = AUDITOR.manifest_checks(
        "production",
        {
            "releaseCinematicEvidence": True,
            "hilCinematicTelemetry": False,
            "hilStorageFaults": False,
        },
        "production",
    )

    assert checks == {
        "releaseCinematicEvidence": True,
        "hilCinematicTelemetry": False,
        "hilStorageFaults": False,
        "hilConfigEnabled": False,
        "embeddedProfile": "production",
        "toolLiterals": "absent",
        "hilSymbols": "absent",
        "bannedApis": "absent",
    }


def test_production_artifacts_require_release_evidence_literal_and_symbol(tmp_path):
    artifacts = {}
    release_blob = b"CINE_EVIDENCE TBOT_EMBEDDED_PROFILE=production"
    for name in ("bin", "elf", "map", "mainArchive"):
        path = tmp_path / name
        path.write_bytes(release_blob if name != "map" else b"map")
        artifacts[name] = path

    AUDITOR.audit_literals("production", artifacts)
    AUDITOR.audit_symbols(
        "production",
        "00000000 T tbot::LessonCinematicEvidenceBeginCue\n",
    )

    artifacts["elf"].write_bytes(b"TBOT_EMBEDDED_PROFILE=production")
    with pytest.raises(AUDITOR.AuditFailure, match="CINE_EVIDENCE"):
        AUDITOR.audit_literals("production", artifacts)
    with pytest.raises(AUDITOR.AuditFailure, match="LessonCinematicEvidence"):
        AUDITOR.audit_symbols("production", "")


@pytest.mark.parametrize(
    ("legacy_literal", "legacy_symbol"),
    (
        ("HIL_CINE", "LessonCinematicHilTelemetryBeginCue"),
        ("self.lesson_assets.hil.status", "RegisterLessonStorageHilMcpTools"),
    ),
)
def test_production_artifacts_reject_legacy_hil_leakage(
    tmp_path, legacy_literal, legacy_symbol
):
    artifacts = {}
    for name in ("bin", "elf", "map", "mainArchive"):
        path = tmp_path / name
        path.write_bytes(
            f"CINE_EVIDENCE {legacy_literal} TBOT_EMBEDDED_PROFILE=production".encode()
        )
        artifacts[name] = path

    with pytest.raises(AUDITOR.AuditFailure, match="HIL"):
        AUDITOR.audit_literals("production", artifacts)
    with pytest.raises(AUDITOR.AuditFailure, match="HIL"):
        AUDITOR.audit_symbols(
            "production",
            f"LessonCinematicEvidenceBeginCue\n{legacy_symbol}\n",
        )

def test_failed_audit_removes_stale_outputs_before_any_check(tmp_path):
    manifest = tmp_path / AUDITOR.MANIFEST_NAME
    checksum = tmp_path / AUDITOR.SHA_NAME
    manifest.write_text("stale", encoding="utf-8")
    checksum.write_text("stale", encoding="utf-8")

    with pytest.raises(AUDITOR.AuditFailure):
        AUDITOR.audit("production", str(tmp_path))

    assert not manifest.exists()
    assert not checksum.exists()


def test_atomic_write_replaces_destination_without_temp_residue(tmp_path):
    output = tmp_path / "artifact"
    output.write_bytes(b"old")

    AUDITOR.atomic_write(output, b"new")

    assert output.read_bytes() == b"new"
    assert list(tmp_path.glob(".artifact.*")) == []


def test_artifact_chronology_rejects_bin_older_than_linked_elf(tmp_path):
    archive = tmp_path / "libmain.a"
    elf = tmp_path / "xiaozhi.elf"
    binary = tmp_path / "xiaozhi.bin"
    for path in (archive, elf, binary):
        path.write_bytes(b"artifact")
    archive.touch()
    elf.touch()
    binary.touch()

    AUDITOR.validate_artifact_chronology(
        {"mainArchive": archive, "elf": elf, "bin": binary}
    )
    binary.touch()
    archive.touch()
    with pytest.raises(AUDITOR.AuditFailure):
        AUDITOR.validate_artifact_chronology(
            {"mainArchive": archive, "elf": elf, "bin": binary}
        )


def test_artifact_freshness_rejects_every_file_older_than_source_commit(tmp_path):
    commit_time = 2_000_000_000
    artifacts = {}
    for name in (
        "mainArchive", "elf", "map", "bin", "projectDescription",
        "sdkconfig", "partitionBinary",
    ):
        path = tmp_path / name
        path.write_bytes(b"artifact")
        os.utime(path, ns=((commit_time + 1) * 1_000_000_000,) * 2)
        artifacts[name] = path

    AUDITOR.validate_artifact_freshness(artifacts, commit_time)
    os.utime(
        artifacts["mainArchive"],
        ns=((commit_time - 1) * 1_000_000_000,) * 2,
    )
    with pytest.raises(AUDITOR.AuditFailure, match="predates source commit"):
        AUDITOR.validate_artifact_freshness(artifacts, commit_time)


def _artifact_fixture(build_dir: Path):
    paths = {
        "bin": build_dir / "xiaozhi.bin",
        "elf": build_dir / "xiaozhi.elf",
        "map": build_dir / "xiaozhi.map",
        "mainArchive": build_dir / "esp-idf/main/libmain.a",
        "projectDescription": build_dir / "project_description.json",
        "sdkconfig": build_dir / "sdkconfig",
        "partitionBinary": build_dir / "partition_table/partition-table.bin",
    }
    for path in paths.values():
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(b"artifact")
    return paths


@pytest.mark.parametrize(
    "label",
    (
        "bin", "elf", "map", "mainArchive", "projectDescription",
        "sdkconfig", "partitionBinary",
    ),
)
def test_resolve_artifacts_rejects_symlink_for_every_artifact(tmp_path, label):
    build_dir = tmp_path / "build"
    paths = _artifact_fixture(build_dir)
    outside = tmp_path / "outside"
    outside.write_bytes(b"outside")
    paths[label].unlink()
    paths[label].symlink_to(outside)
    description = {"app_bin": "xiaozhi.bin", "app_elf": "xiaozhi.elf"}

    with pytest.raises(AUDITOR.AuditFailure, match="symlink"):
        AUDITOR.resolve_artifacts(build_dir, description)


@pytest.mark.parametrize("field", ("app_bin", "app_elf"))
def test_resolve_artifacts_rejects_declared_path_traversal(tmp_path, field):
    build_dir = tmp_path / "build"
    _artifact_fixture(build_dir)
    outside = tmp_path / "outside.bin"
    outside.write_bytes(b"outside")
    description = {"app_bin": "xiaozhi.bin", "app_elf": "xiaozhi.elf"}
    description[field] = "../outside.bin"

    with pytest.raises(AUDITOR.AuditFailure, match="relative path"):
        AUDITOR.resolve_artifacts(build_dir, description)
