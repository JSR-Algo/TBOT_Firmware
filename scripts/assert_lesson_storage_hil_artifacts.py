#!/usr/bin/env python3
"""Fail-closed identity audit for production and attended HIL firmware builds."""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path


MANIFEST_NAME = "lesson-storage-hil-build.json"
SHA_NAME = "lesson-storage-hil-build.sha256"
TOOL_NAMES = (
    "self.lesson_assets.hil.arm_fault",
    "self.lesson_assets.hil.status",
    "self.lesson_assets.hil.stage_fixture",
    "self.lesson_assets.hil.cleanup_fixture",
    "self.lesson_assets.hil.inspect",
)
BANNER = "TBOT_HIL_STORAGE_FAULTS_ENABLED non-production-image"
HIL_SYMBOLS = (
    "LessonStorageHilController",
    "RegisterLessonStorageHilMcpTools",
    "RunLessonStorageHilCheckpoint",
    "RunLessonStorageHilStagingCheckpoint",
    "StageLessonStorageHilFixture",
    "CleanupLessonStorageHilFixture",
    "InspectLessonStorageHilStorage",
)
BANNED_APIS = ("lstat", "openat", "fstatat", "fdopendir", "unlinkat")
GIT_HEAD_COMMAND = "git rev-parse HEAD"
GIT_STATUS_COMMAND = "git status --porcelain"
GIT_COMMIT_TIME_COMMAND = "git show -s --format=%ct HEAD"
NM_COMMAND = "nm"


class AuditFailure(RuntimeError):
    pass


def run(command: list[str], cwd: Path) -> str:
    result = subprocess.run(
        command,
        cwd=cwd,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
    )
    if result.returncode != 0:
        raise AuditFailure(
            f"command failed ({' '.join(command)}): {result.stderr.strip()}"
        )
    return result.stdout


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def read_bytes(path: Path) -> bytes:
    return path.read_bytes()


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AuditFailure(message)


def parse_sdkconfig(path: Path) -> dict[str, str]:
    values: dict[str, str] = {}
    for raw_line in path.read_text(encoding="utf-8").splitlines():
        line = raw_line.strip()
        if not line or line.startswith("#") or "=" not in line:
            continue
        key, value = line.split("=", 1)
        values[key] = value.strip().strip('"')
    return values


def parse_size(text: str) -> int:
    value = text.strip().upper()
    multiplier = 1
    if value.endswith("K"):
        multiplier = 1024
        value = value[:-1]
    elif value.endswith("M"):
        multiplier = 1024 * 1024
        value = value[:-1]
    return int(value, 0) * multiplier


def app_partition_metrics(repo: Path, sdkconfig: dict[str, str], app_bin: Path) -> dict:
    partition_name = sdkconfig.get("CONFIG_PARTITION_TABLE_FILENAME")
    require(bool(partition_name), "partition table filename missing from sdkconfig")
    partition_path = (repo / partition_name).resolve()
    require(partition_path.is_file(), "partition table source missing")
    app_sizes: list[int] = []
    with partition_path.open(encoding="utf-8", newline="") as source:
        for row in csv.reader(line for line in source if not line.lstrip().startswith("#")):
            if len(row) < 5:
                continue
            partition_type = row[1].strip()
            subtype = row[2].strip()
            if partition_type == "app" and subtype in {"factory", "ota_0", "ota_1"}:
                app_sizes.append(parse_size(row[4]))
    require(bool(app_sizes), "no application partition found")
    partition_bytes = min(app_sizes)
    image_bytes = app_bin.stat().st_size
    require(image_bytes <= partition_bytes, "application image exceeds partition")
    free_bytes = partition_bytes - image_bytes
    return {
        "partitionTable": str(partition_path.relative_to(repo)),
        "partitionBytes": partition_bytes,
        "imageBytes": image_bytes,
        "freeBytes": free_bytes,
        "freePercent": round((free_bytes * 100.0) / partition_bytes, 6),
    }


def confined_regular_file(build_dir: Path, path: Path, label: str) -> Path:
    build_root = build_dir.resolve()
    try:
        relative = path.relative_to(build_dir)
    except ValueError as error:
        raise AuditFailure(f"artifact path escapes build directory: {label}") from error
    probe = build_dir
    for component in relative.parts:
        probe = probe / component
        require(not probe.is_symlink(), f"artifact path contains symlink: {label}")
    try:
        resolved = path.resolve(strict=True)
    except FileNotFoundError as error:
        raise AuditFailure(f"required artifact missing: {label}") from error
    require(resolved != build_root and build_root in resolved.parents,
            f"artifact path escapes build directory: {label}")
    require(resolved.is_file(), f"required artifact is not a regular file: {label}")
    require(resolved.stat().st_size > 0, f"required artifact empty: {label}")
    return resolved


def declared_artifact(build_dir: Path, value: str, label: str) -> Path:
    declared = Path(value)
    require(
        not declared.is_absolute() and ".." not in declared.parts,
        f"declared artifact must be a confined relative path: {label}",
    )
    return build_dir / declared


def resolve_artifacts(build_dir: Path, description: dict) -> dict[str, Path]:
    candidates = {
        "bin": declared_artifact(
            build_dir, description.get("app_bin", "xiaozhi.bin"), "bin"
        ),
        "elf": declared_artifact(
            build_dir, description.get("app_elf", "xiaozhi.elf"), "elf"
        ),
        "map": build_dir / "xiaozhi.map",
        "mainArchive": build_dir / "esp-idf/main/libmain.a",
        "projectDescription": build_dir / "project_description.json",
        "sdkconfig": build_dir / "sdkconfig",
        "partitionBinary": build_dir / "partition_table/partition-table.bin",
    }
    return {
        label: confined_regular_file(build_dir, path, label)
        for label, path in candidates.items()
    }


def validate_artifact_chronology(artifacts: dict[str, Path]) -> None:
    archive_time = artifacts["mainArchive"].stat().st_mtime_ns
    elf_time = artifacts["elf"].stat().st_mtime_ns
    binary_time = artifacts["bin"].stat().st_mtime_ns
    require(archive_time <= elf_time, "ELF predates the linked main archive")
    require(elf_time <= binary_time, "application binary predates the linked ELF")


def validate_artifact_freshness(
    artifacts: dict[str, Path], commit_timestamp_seconds: int
) -> None:
    commit_time_ns = commit_timestamp_seconds * 1_000_000_000
    for label in (
        "mainArchive", "elf", "map", "bin", "projectDescription",
        "sdkconfig", "partitionBinary",
    ):
        require(
            artifacts[label].stat().st_mtime_ns >= commit_time_ns,
            f"build artifact predates source commit: {label}",
        )


def resolve_nm(description: dict) -> str:
    compiler = Path(str(description.get("c_compiler", "")))
    require(compiler.name.endswith("gcc"), "C compiler identity missing")
    candidate = compiler.with_name(compiler.name[:-3] + "nm")
    require(candidate.is_file(), "target nm executable missing")
    return str(candidate)


def audit_symbols(profile: str, nm_output: str) -> None:
    for api in BANNED_APIS:
        pattern = rf"(?:^|\s)_?{re.escape(api)}(?:$|\s)"
        require(not re.search(pattern, nm_output, re.MULTILINE),
                f"banned API symbol present: {api}")
    for symbol in HIL_SYMBOLS:
        present = symbol in nm_output
        require(present == (profile == "hil"),
                f"HIL symbol profile mismatch: {symbol}")


def audit_profile_configuration(profile: str, sdkconfig: dict[str, str]) -> None:
    require(
        "CONFIG_TBOT_HIL_PROFILE" not in sdkconfig,
        "manual profile override forbidden",
    )
    enabled = sdkconfig.get("CONFIG_TBOT_HIL_STORAGE_FAULTS") == "y"
    require(enabled == (profile == "hil"), "CONFIG_TBOT_HIL_STORAGE_FAULTS profile mismatch")


def audit_profile_literals(profile: str, artifacts: dict[str, Path]) -> str:
    expected = "task14-hil-v1" if profile == "hil" else "production"
    forbidden = "production" if profile == "hil" else "task14-hil-v1"
    expected_literal = f"TBOT_EMBEDDED_PROFILE={expected}".encode("ascii")
    forbidden_literal = f"TBOT_EMBEDDED_PROFILE={forbidden}".encode("ascii")
    for name in ("bin", "elf", "mainArchive"):
        content = read_bytes(artifacts[name])
        require(expected_literal in content, "embedded profile literal mismatch")
        require(forbidden_literal not in content, "embedded profile literal mismatch")
    return expected


def audit_literals(profile: str, artifacts: dict[str, Path]) -> None:
    searchable = {
        name: read_bytes(artifacts[name])
        for name in ("bin", "elf", "map", "mainArchive")
    }
    for literal in (*TOOL_NAMES, BANNER):
        encoded = literal.encode("ascii")
        locations = {name for name, content in searchable.items() if encoded in content}
        if profile == "production":
            require(not locations, f"production artifact contains HIL literal: {literal}")
        else:
            required = {"bin", "elf", "mainArchive"}
            require(required.issubset(locations),
                    f"HIL literal missing from artifacts: {literal}")


def defaults_manifest(repo: Path, description: dict, profile: str) -> list[dict]:
    raw_defaults = str(description.get("config_defaults", ""))
    require(bool(raw_defaults), "config-default chain missing")
    paths = [Path(item).resolve() for item in raw_defaults.split(";") if item]
    require(bool(paths), "config-default chain empty")
    has_hil = any(path.name == "sdkconfig.defaults.hil-storage" for path in paths)
    require(has_hil == (profile == "hil"), "HIL defaults profile mismatch")
    records = []
    for path in paths:
        require(path.is_file(), f"config default missing: {path.name}")
        try:
            display = str(path.relative_to(repo))
        except ValueError:
            require(profile == "hil" and path.parent == Path(description["build_dir"]),
                    "config default is outside repository/build directory")
            display = f"{Path(description['build_dir']).name}/{path.name}"
        records.append({"path": display, "sha256": sha256(path)})
    return records


def atomic_write(path: Path, data: bytes) -> None:
    descriptor, temp_name = tempfile.mkstemp(prefix=f".{path.name}.", dir=path.parent)
    try:
        with os.fdopen(descriptor, "wb") as output:
            output.write(data)
            output.flush()
            os.fsync(output.fileno())
        os.replace(temp_name, path)
    except BaseException:
        try:
            os.unlink(temp_name)
        except FileNotFoundError:
            pass
        raise


def audit(profile: str, build_dir_argument: str) -> dict:
    script = Path(__file__).resolve()
    repo = script.parents[1]
    build_dir = Path(build_dir_argument).expanduser().resolve()
    manifest_path = build_dir / MANIFEST_NAME
    sha_path = build_dir / SHA_NAME
    for stale in (manifest_path, sha_path):
        try:
            stale.unlink()
        except FileNotFoundError:
            pass

    require(build_dir.is_dir(), "build directory missing")
    head = run(["git", "rev-parse", "HEAD"], repo).strip()
    require(re.fullmatch(r"[0-9a-f]{40}", head) is not None, "invalid source commit")
    require(run(["git", "status", "--porcelain"], repo) == "", "source tree is not clean")
    commit_timestamp = int(
        run(["git", "show", "-s", "--format=%ct", "HEAD"], repo).strip()
    )

    description_path = confined_regular_file(
        build_dir, build_dir / "project_description.json", "projectDescription"
    )
    description = json.loads(description_path.read_text(encoding="utf-8"))
    require(description.get("target") == "esp32s3", "unexpected build target")
    require(description.get("project_name") == "xiaozhi", "unexpected project name")
    require(Path(description.get("project_path", "")).resolve() == repo,
            "build project path does not match repository")
    require(Path(description.get("build_dir", "")).resolve() == build_dir,
            "project description build directory mismatch")

    artifacts = resolve_artifacts(build_dir, description)
    validate_artifact_chronology(artifacts)
    validate_artifact_freshness(artifacts, commit_timestamp)
    sdkconfig = parse_sdkconfig(artifacts["sdkconfig"])
    enabled = sdkconfig.get("CONFIG_TBOT_HIL_STORAGE_FAULTS") == "y"
    audit_profile_configuration(profile, sdkconfig)
    defaults = defaults_manifest(repo, description, profile)
    nm_path = resolve_nm(description)
    nm_output = run([nm_path, "-C", str(artifacts["mainArchive"])], repo)
    audit_symbols(profile, nm_output)
    audit_literals(profile, artifacts)
    embedded_profile = audit_profile_literals(profile, artifacts)
    partition = app_partition_metrics(repo, sdkconfig, artifacts["bin"])

    manifest = {
        "status": "PASS",
        "profile": profile,
        "sourceCommit": head,
        "sourceCommitTimestamp": commit_timestamp,
        "target": description["target"],
        "project": description["project_name"],
        "buildDirectory": build_dir.name,
        "configDefaults": defaults,
        "artifacts": {
            label: {
                "path": str(path.relative_to(build_dir)),
                "bytes": path.stat().st_size,
                "sha256": sha256(path),
            }
            for label, path in artifacts.items()
        },
        "partition": partition,
        "checks": {
            "hilConfigEnabled": enabled,
            "embeddedProfile": embedded_profile,
            "toolLiterals": "present" if profile == "hil" else "absent",
            "hilSymbols": "present" if profile == "hil" else "absent",
            "bannedApis": "absent",
        },
    }
    manifest_bytes = (json.dumps(manifest, indent=2, sort_keys=True) + "\n").encode("utf-8")
    try:
        atomic_write(manifest_path, manifest_bytes)
        manifest_hash = hashlib.sha256(manifest_bytes).hexdigest()
        atomic_write(
            sha_path, f"{manifest_hash}  {MANIFEST_NAME}\n".encode("ascii")
        )
    except BaseException:
        for partial in (manifest_path, sha_path):
            try:
                partial.unlink()
            except FileNotFoundError:
                pass
        raise
    return manifest


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--profile", required=True, choices=("production", "hil"))
    parser.add_argument("--build-dir", required=True)
    arguments = parser.parse_args()
    try:
        manifest = audit(arguments.profile, arguments.build_dir)
    except (AuditFailure, OSError, ValueError, json.JSONDecodeError) as error:
        print(f"lesson storage HIL artifact audit: FAIL: {error}", file=sys.stderr)
        return 1
    print(
        "lesson storage HIL artifact audit: PASS "
        f"profile={manifest['profile']} commit={manifest['sourceCommit']}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
