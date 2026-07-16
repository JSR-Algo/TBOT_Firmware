import importlib.util
import os
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
