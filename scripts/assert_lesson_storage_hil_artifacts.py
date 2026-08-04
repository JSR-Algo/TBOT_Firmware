#!/usr/bin/env python3
"""Fail-closed identity audit for production and attended HIL firmware builds."""

from __future__ import annotations

import argparse
import csv
import hashlib
import io
import json
import os
import re
import stat
import subprocess
import sys
import tempfile
from collections.abc import Callable
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
RELEASE_CINEMATIC_LITERAL = "CINE_EVIDENCE"
LEGACY_CINEMATIC_LITERAL = "HIL_CINE"
RELEASE_CINEMATIC_MARKERS = (
    "CINE_EVIDENCE event=boot",
    "CINE_EVIDENCE event=cue_end",
)
RELEASE_CINEMATIC_SYMBOLS = (
    "LessonCinematicEvidenceBoot",
    "LessonCinematicEvidenceEmitCueEnd",
)
LEGACY_CINEMATIC_SYMBOL = "LessonCinematicHilTelemetry"
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
CRITICAL_BOOLEAN_CONFIGS = (
    "CONFIG_TBOT_RELEASE_CINEMATIC_EVIDENCE",
    "CONFIG_TBOT_HIL_CINEMATIC_TELEMETRY",
    "CONFIG_TBOT_HIL_STORAGE_FAULTS",
)
GIT_HEAD_COMMAND = "git rev-parse HEAD"
GIT_STATUS_COMMAND = "git status --porcelain"
GIT_COMMIT_TIME_COMMAND = "git show -s --format=%ct HEAD"
NM_COMMAND = "nm"


class AuditFailure(RuntimeError):
    pass


class ArtifactSnapshot:
    __slots__ = (
        "root", "path", "label", "data", "device", "inode", "size",
        "mtime_ns", "ctime_ns", "sha256",
    )

    def __init__(
        self,
        root: Path,
        path: Path,
        label: str,
        data: bytes,
        device: int,
        inode: int,
        size: int,
        mtime_ns: int,
        ctime_ns: int,
        digest: str,
    ) -> None:
        self.root = root
        self.path = path
        self.label = label
        self.data = data
        self.device = device
        self.inode = inode
        self.size = size
        self.mtime_ns = mtime_ns
        self.ctime_ns = ctime_ns
        self.sha256 = digest


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


def hash_bytes(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AuditFailure(message)


def parse_sdkconfig(path: Path) -> dict[str, str]:
    return parse_sdkconfig_data(path.read_bytes())


def parse_sdkconfig_data(data: bytes) -> dict[str, str]:
    values: dict[str, str] = {}
    critical_seen: set[str] = set()
    for raw_line in data.decode("utf-8").splitlines():
        line = raw_line.strip()
        critical = next(
            (key for key in CRITICAL_BOOLEAN_CONFIGS if key in line), None
        )
        if critical is not None:
            require(critical not in critical_seen,
                    f"duplicate sdkconfig boolean: {critical}")
            if line == f"{critical}=y":
                values[critical] = "y"
            elif line == f"# {critical} is not set":
                values[critical] = "n"
            else:
                raise AuditFailure(f"malformed sdkconfig boolean: {critical}")
            critical_seen.add(critical)
            continue
        if not line or line.startswith("#") or "=" not in line:
            continue
        key, value = line.split("=", 1)
        values[key] = value.strip().strip('"')
    missing = set(CRITICAL_BOOLEAN_CONFIGS) - critical_seen
    if missing:
        raise AuditFailure(f"missing sdkconfig boolean: {sorted(missing)[0]}")
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


def app_partition_metrics(
    repo: Path,
    sdkconfig: dict[str, str],
    app_bin: Path | ArtifactSnapshot,
    snapshots: list[ArtifactSnapshot] | None = None,
) -> dict:
    partition_name = sdkconfig.get("CONFIG_PARTITION_TABLE_FILENAME")
    require(bool(partition_name), "partition table filename missing from sdkconfig")
    partition_path = repo / partition_name
    partition_snapshot = snapshot_artifact(repo, partition_path, "partitionTableSource")
    if snapshots is not None:
        snapshots.append(partition_snapshot)
    app_sizes: list[int] = []
    with io.StringIO(partition_snapshot.data.decode("utf-8"), newline="") as source:
        for row in csv.reader(line for line in source if not line.lstrip().startswith("#")):
            if len(row) < 5:
                continue
            partition_type = row[1].strip()
            subtype = row[2].strip()
            if partition_type == "app" and subtype in {"factory", "ota_0", "ota_1"}:
                app_sizes.append(parse_size(row[4]))
    require(bool(app_sizes), "no application partition found")
    partition_bytes = min(app_sizes)
    image_bytes = app_bin.size if isinstance(app_bin, ArtifactSnapshot) else app_bin.stat().st_size
    require(image_bytes <= partition_bytes, "application image exceeds partition")
    free_bytes = partition_bytes - image_bytes
    return {
        "partitionTable": str(partition_snapshot.path.relative_to(repo)),
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


def snapshot_artifact(root: Path, path: Path, label: str) -> ArtifactSnapshot:
    trusted_root = root.resolve()
    resolved = confined_regular_file(trusted_root, path, label)
    flags = os.O_RDONLY
    if hasattr(os, "O_NOFOLLOW"):
        flags |= os.O_NOFOLLOW
    descriptor = os.open(resolved, flags)
    try:
        before = os.fstat(descriptor)
        require(stat.S_ISREG(before.st_mode), f"required artifact is not regular: {label}")
        chunks: list[bytes] = []
        while True:
            block = os.read(descriptor, 1024 * 1024)
            if not block:
                break
            chunks.append(block)
        after = os.fstat(descriptor)
    finally:
        os.close(descriptor)
    identity_before = (
        before.st_dev, before.st_ino, before.st_size,
        before.st_mtime_ns, before.st_ctime_ns,
    )
    identity_after = (
        after.st_dev, after.st_ino, after.st_size,
        after.st_mtime_ns, after.st_ctime_ns,
    )
    require(identity_before == identity_after, f"artifact changed during audit: {label}")
    data = b"".join(chunks)
    require(len(data) == before.st_size and bool(data),
            f"artifact changed during audit: {label}")
    return ArtifactSnapshot(
        trusted_root, resolved, label, data,
        before.st_dev, before.st_ino, before.st_size,
        before.st_mtime_ns, before.st_ctime_ns, hash_bytes(data),
    )


def verify_artifact_snapshot(snapshot: ArtifactSnapshot) -> None:
    current = snapshot_artifact(snapshot.root, snapshot.path, snapshot.label)
    expected = (
        snapshot.device, snapshot.inode, snapshot.size,
        snapshot.mtime_ns, snapshot.ctime_ns, snapshot.sha256,
    )
    observed = (
        current.device, current.inode, current.size,
        current.mtime_ns, current.ctime_ns, current.sha256,
    )
    require(observed == expected,
            f"artifact changed during audit: {snapshot.label}")


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


def snapshot_artifacts(
    build_dir: Path, artifacts: dict[str, Path]
) -> dict[str, ArtifactSnapshot]:
    return {
        label: snapshot_artifact(build_dir, path, label)
        for label, path in artifacts.items()
    }


def artifact_mtime_ns(artifact: Path | ArtifactSnapshot) -> int:
    if isinstance(artifact, ArtifactSnapshot):
        return artifact.mtime_ns
    return artifact.stat().st_mtime_ns


def validate_artifact_chronology(
    artifacts: dict[str, Path | ArtifactSnapshot]
) -> None:
    archive_time = artifact_mtime_ns(artifacts["mainArchive"])
    elf_time = artifact_mtime_ns(artifacts["elf"])
    binary_time = artifact_mtime_ns(artifacts["bin"])
    require(archive_time <= elf_time, "ELF predates the linked main archive")
    require(elf_time <= binary_time, "application binary predates the linked ELF")


def validate_artifact_freshness(
    artifacts: dict[str, Path | ArtifactSnapshot], commit_timestamp_seconds: int
) -> None:
    commit_time_ns = commit_timestamp_seconds * 1_000_000_000
    for label in (
        "mainArchive", "elf", "map", "bin", "projectDescription",
        "sdkconfig", "partitionBinary",
    ):
        require(
            artifact_mtime_ns(artifacts[label]) >= commit_time_ns,
            f"build artifact predates source commit: {label}",
        )


def validate_nm_executable(
    candidate: Path, trusted_root: Path
) -> ArtifactSnapshot:
    try:
        resolved = candidate.expanduser().resolve(strict=True)
    except FileNotFoundError as error:
        raise AuditFailure("trusted ESP target nm executable missing") from error
    require(resolved.name == "xtensa-esp32s3-elf-nm",
            "trusted ESP target nm has unexpected name")
    require(resolved.is_file() and os.access(resolved, os.X_OK),
            "trusted ESP target nm is not executable")
    root = trusted_root.expanduser().resolve(strict=True)
    require(root == resolved.parent or root in resolved.parents,
            "trusted ESP target nm escapes IDF tools root")
    return snapshot_artifact(root, resolved, "nmExecutable")


def resolve_nm(explicit_nm: str | None = None) -> ArtifactSnapshot:
    tools_root = Path(os.environ.get("IDF_TOOLS_PATH", "~/.espressif"))
    try:
        trusted_root = tools_root.expanduser().resolve(strict=True)
    except FileNotFoundError as error:
        raise AuditFailure("trusted ESP target nm tools root missing") from error

    if explicit_nm:
        return validate_nm_executable(Path(explicit_nm), trusted_root)

    path_candidate = None
    for directory in os.environ.get("PATH", "").split(os.pathsep):
        if directory:
            candidate = Path(directory) / "xtensa-esp32s3-elf-nm"
            if candidate.exists():
                path_candidate = candidate
                break
    if path_candidate is not None:
        try:
            return validate_nm_executable(path_candidate, trusted_root)
        except AuditFailure:
            pass

    matches = sorted(trusted_root.glob(
        "tools/xtensa-esp-elf/*/xtensa-esp-elf/bin/xtensa-esp32s3-elf-nm"
    ))
    require(bool(matches), "trusted ESP target nm executable missing")
    return validate_nm_executable(matches[-1], trusted_root)


def parse_defined_nm_symbols(nm_output: str) -> list[str]:
    symbols: list[str] = []
    pattern = re.compile(r"^\s*(?:[0-9a-fA-F]+\s+)?([A-Za-z])\s+(.+?)\s*$")
    for line in nm_output.splitlines():
        match = pattern.match(line)
        if match is None or match.group(1).upper() == "U":
            continue
        symbols.append(match.group(2).split("(", 1)[0].strip())
    return symbols


def has_terminal_api(symbols: list[str], api: str) -> bool:
    return any(symbol == api or symbol.endswith(f"::{api}") for symbol in symbols)


def has_api_family(symbols: list[str], api: str) -> bool:
    return any(
        symbol == api
        or symbol.endswith(f"::{api}")
        or f"::{api}::" in symbol
        or symbol.startswith(f"{api}::")
        for symbol in symbols
    )


def audit_symbols(
    profile: str, elf_nm_output: str, archive_nm_output: str = ""
) -> None:
    elf_symbols = parse_defined_nm_symbols(elf_nm_output)
    archive_symbols = parse_defined_nm_symbols(archive_nm_output)
    all_symbols = [*elf_symbols, *archive_symbols]
    for api in BANNED_APIS:
        present = has_terminal_api(all_symbols, api) or has_terminal_api(
            all_symbols, f"_{api}"
        )
        require(not present,
                f"banned API symbol present: {api}")
    for symbol in HIL_SYMBOLS:
        present = has_api_family(elf_symbols, symbol)
        require(present == (profile == "hil"),
                f"HIL symbol profile mismatch: {symbol}")
    if profile == "production":
        for symbol in RELEASE_CINEMATIC_SYMBOLS:
            require(has_terminal_api(elf_symbols, symbol),
                    f"linked ELF missing defined symbol: {symbol}")
        legacy_present = any(
            component.startswith(LEGACY_CINEMATIC_SYMBOL)
            for symbol in all_symbols
            for component in symbol.split("::")
        )
        require(not legacy_present,
                f"production artifact contains HIL symbol: {LEGACY_CINEMATIC_SYMBOL}")


def artifact_bytes(artifact: Path | ArtifactSnapshot) -> bytes:
    return artifact.data if isinstance(artifact, ArtifactSnapshot) else read_bytes(artifact)


def audit_profile_configuration(
    profile: str, sdkconfig: dict[str, str]
) -> dict[str, bool]:
    require(
        "CONFIG_TBOT_HIL_PROFILE" not in sdkconfig,
        "manual profile override forbidden",
    )
    checks = {
        "releaseCinematicEvidence":
            sdkconfig.get("CONFIG_TBOT_RELEASE_CINEMATIC_EVIDENCE") == "y",
        "hilCinematicTelemetry":
            sdkconfig.get("CONFIG_TBOT_HIL_CINEMATIC_TELEMETRY") == "y",
        "hilStorageFaults":
            sdkconfig.get("CONFIG_TBOT_HIL_STORAGE_FAULTS") == "y",
    }
    require(
        checks["hilStorageFaults"] == (profile == "hil"),
        "HIL storage faults profile mismatch",
    )
    if profile == "production":
        require(checks["releaseCinematicEvidence"],
                "production release cinematic evidence must be enabled")
        require(not checks["hilCinematicTelemetry"],
                "production cinematic telemetry HIL must be disabled")
        require(not checks["hilStorageFaults"],
                "production storage faults HIL must be disabled")
    return checks


def audit_profile_literals(
    profile: str, artifacts: dict[str, Path | ArtifactSnapshot]
) -> str:
    expected = "task14-hil-v1" if profile == "hil" else "production"
    prefix = b"TBOT_EMBEDDED_PROFILE="
    expected_token = expected.encode("ascii")
    token_pattern = re.compile(re.escape(prefix) + rb"([^\x00\s]+)")
    for name in ("bin", "elf", "mainArchive"):
        content = artifact_bytes(artifacts[name])
        tokens = token_pattern.findall(content)
        require(
            content.count(prefix) == 1 and tokens == [expected_token],
            f"embedded profile literal mismatch: {name}",
        )
    return expected


def audit_literals(profile: str, artifacts: dict[str, Path | ArtifactSnapshot]) -> None:
    searchable = {
        name: artifact_bytes(artifacts[name])
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
    if profile == "production":
        for name in ("bin", "elf"):
            for marker in RELEASE_CINEMATIC_MARKERS:
                pattern = rb"(?<![A-Za-z0-9_])" + re.escape(marker.encode("ascii"))
                pattern += rb"(?=$|[\x00\s])"
                require(re.search(pattern, searchable[name]) is not None,
                        f"production artifact missing {marker}: {name}")
        legacy = LEGACY_CINEMATIC_LITERAL.encode("ascii")
        require(
            not any(legacy in content for content in searchable.values()),
            f"production artifact contains HIL literal: {LEGACY_CINEMATIC_LITERAL}",
        )


def manifest_checks(
    profile: str, config_checks: dict[str, bool], embedded_profile: str
) -> dict[str, bool | str]:
    return {
        **config_checks,
        "hilConfigEnabled": config_checks["hilStorageFaults"],
        "embeddedProfile": embedded_profile,
        "toolLiterals": "present" if profile == "hil" else "absent",
        "hilSymbols": "present" if profile == "hil" else "absent",
        "bannedApis": "absent",
    }


def defaults_manifest(
    repo: Path,
    build_dir: Path,
    description: dict,
    profile: str,
    snapshots: list[ArtifactSnapshot],
) -> list[dict]:
    raw_defaults = str(description.get("config_defaults", ""))
    require(bool(raw_defaults), "config-default chain missing")
    paths = [Path(item).resolve() for item in raw_defaults.split(";") if item]
    require(bool(paths), "config-default chain empty")
    has_hil = any(path.name == "sdkconfig.defaults.hil-storage" for path in paths)
    require(has_hil == (profile == "hil"), "HIL defaults profile mismatch")
    records = []
    for path in paths:
        try:
            display = str(path.relative_to(repo))
            root = repo
        except ValueError:
            require(profile == "hil" and path.parent == build_dir,
                    "config default is outside repository/build directory")
            display = f"{build_dir.name}/{path.name}"
            root = build_dir
        snapshot = snapshot_artifact(root, path, f"configDefault:{path.name}")
        snapshots.append(snapshot)
        records.append({"path": display, "sha256": snapshot.sha256})
    return records


def run_nm_on_snapshot(
    nm: ArtifactSnapshot, artifact: ArtifactSnapshot, repo: Path
) -> str:
    suffix = artifact.path.suffix or ".artifact"
    descriptor, temp_name = tempfile.mkstemp(
        prefix=f"lesson-audit-{artifact.label}-", suffix=suffix
    )
    try:
        with os.fdopen(descriptor, "wb") as output:
            output.write(artifact.data)
            output.flush()
            os.fsync(output.fileno())
        verify_artifact_snapshot(nm)
        return run([str(nm.path), "-C", "--defined-only", temp_name], repo)
    finally:
        verify_artifact_snapshot(nm)
        try:
            os.unlink(temp_name)
        except FileNotFoundError:
            pass


def verify_source_state(repo: Path, expected_head: str) -> None:
    current_head = run(["git", "rev-parse", "HEAD"], repo).strip()
    require(current_head == expected_head, "source HEAD changed during audit")
    require(run(["git", "status", "--porcelain"], repo) == "",
            "source tree changed during audit")


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


def remove_manifest_pair(manifest_path: Path, sha_path: Path) -> None:
    for path in (manifest_path, sha_path):
        try:
            path.unlink()
        except FileNotFoundError:
            pass


def verify_published_manifest_pair(
    manifest_path: Path,
    sha_path: Path,
) -> None:
    root = manifest_path.parent.resolve()
    require(sha_path.parent.resolve() == root,
            "published manifest pair is not coherent")
    manifest = snapshot_artifact(root, manifest_path, "publishedManifest")
    checksum = snapshot_artifact(root, sha_path, "publishedChecksum")
    expected_line = f"{manifest.sha256}  {MANIFEST_NAME}\n".encode("ascii")
    require(checksum.data == expected_line,
            "published manifest pair is not coherent")
    try:
        payload = json.loads(manifest.data.decode("utf-8"))
    except (UnicodeDecodeError, json.JSONDecodeError) as error:
        raise AuditFailure("published manifest pair is not coherent") from error
    require(payload.get("status") == "PASS",
            "published manifest pair is not coherent")


def publish_manifest_pair(
    manifest_path: Path,
    sha_path: Path,
    manifest_bytes: bytes,
    post_publish_verify: Callable[[], None],
    publication_hook: Callable[[str], None] | None = None,
) -> None:
    manifest_hash = hashlib.sha256(manifest_bytes).hexdigest()
    checksum_bytes = f"{manifest_hash}  {MANIFEST_NAME}\n".encode("ascii")
    try:
        # A checksum without a manifest is not a PASS result. Publish the PASS
        # manifest last, then verify the entire bounded audit/publication window.
        atomic_write(sha_path, checksum_bytes)
        if publication_hook is not None:
            publication_hook("after_checksum")
        atomic_write(manifest_path, manifest_bytes)
        if publication_hook is not None:
            publication_hook("after_manifest")
        post_publish_verify()
        verify_published_manifest_pair(manifest_path, sha_path)
    except BaseException:
        remove_manifest_pair(manifest_path, sha_path)
        raise


def audit(
    profile: str,
    build_dir_argument: str,
    nm_argument: str | None = None,
) -> dict:
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

    description_snapshot = snapshot_artifact(
        build_dir, build_dir / "project_description.json", "projectDescription"
    )
    description = json.loads(description_snapshot.data.decode("utf-8"))
    require(description.get("target") == "esp32s3", "unexpected build target")
    require(description.get("project_name") == "xiaozhi", "unexpected project name")
    require(Path(description.get("project_path", "")).resolve() == repo,
            "build project path does not match repository")
    require(Path(description.get("build_dir", "")).resolve() == build_dir,
            "project description build directory mismatch")

    artifact_paths = resolve_artifacts(build_dir, description)
    artifact_paths.pop("projectDescription")
    artifacts = snapshot_artifacts(build_dir, artifact_paths)
    artifacts["projectDescription"] = description_snapshot
    validate_artifact_chronology(artifacts)
    validate_artifact_freshness(artifacts, commit_timestamp)
    sdkconfig = parse_sdkconfig_data(artifacts["sdkconfig"].data)
    config_checks = audit_profile_configuration(profile, sdkconfig)
    auxiliary_snapshots: list[ArtifactSnapshot] = []
    defaults = defaults_manifest(
        repo, build_dir, description, profile, auxiliary_snapshots)
    nm_snapshot = resolve_nm(nm_argument)
    auxiliary_snapshots.append(nm_snapshot)
    elf_nm_output = run_nm_on_snapshot(nm_snapshot, artifacts["elf"], repo)
    archive_nm_output = run_nm_on_snapshot(
        nm_snapshot, artifacts["mainArchive"], repo)
    audit_symbols(profile, elf_nm_output, archive_nm_output)
    audit_literals(profile, artifacts)
    embedded_profile = audit_profile_literals(profile, artifacts)
    partition = app_partition_metrics(
        repo, sdkconfig, artifacts["bin"], auxiliary_snapshots)

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
                "path": str(snapshot.path.relative_to(build_dir)),
                "bytes": snapshot.size,
                "sha256": snapshot.sha256,
            }
            for label, snapshot in artifacts.items()
        },
        "partition": partition,
        "checks": manifest_checks(profile, config_checks, embedded_profile),
    }
    manifest_bytes = (json.dumps(manifest, indent=2, sort_keys=True) + "\n").encode("utf-8")
    def verify_inputs() -> None:
        for snapshot in (*artifacts.values(), *auxiliary_snapshots):
            verify_artifact_snapshot(snapshot)
        verify_source_state(repo, head)

    try:
        verify_inputs()
        publish_manifest_pair(
            manifest_path, sha_path, manifest_bytes, verify_inputs)
    except BaseException:
        remove_manifest_pair(manifest_path, sha_path)
        raise
    return manifest


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--profile", required=True, choices=("production", "hil"))
    parser.add_argument("--build-dir", required=True)
    parser.add_argument(
        "--nm",
        help="trusted explicit path to xtensa-esp32s3-elf-nm",
    )
    arguments = parser.parse_args()
    try:
        manifest = audit(arguments.profile, arguments.build_dir, arguments.nm)
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
