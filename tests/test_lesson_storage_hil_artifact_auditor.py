import importlib.util
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
