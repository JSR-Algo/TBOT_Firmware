#!/usr/bin/env python3

import hashlib
import json
import os
from pathlib import Path
import re
import struct
import subprocess
import sys
import tempfile
from typing import Any, Dict, NoReturn


CHECK = "lessonPreviewSoftwareReadiness"
REQUIRED_ARGUMENTS = (
    "--contract",
    "--manager-repo",
    "--backend-repo",
    "--evidence-dir",
)


def report(value: Dict[str, Any], exit_code: int) -> NoReturn:
    sys.stdout.write(json.dumps(value, separators=(",", ":")) + "\n")
    raise SystemExit(exit_code)


def argument_value(name: str) -> str:
    try:
        return sys.argv[sys.argv.index(name) + 1]
    except (ValueError, IndexError):
        return ""


def require(condition: bool, message: str) -> None:
    if not condition:
        raise ValueError(message)


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def git_show(repo: Path, revision: str, source_path: str) -> str:
    result = subprocess.run(
        ["git", "-C", str(repo), "show", f"{revision}:{source_path}"],
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
    )
    require(
        result.returncode == 0,
        result.stderr.strip() or f"missing {revision}:{source_path}",
    )
    return result.stdout


def png_dimensions(path: Path) -> tuple[int, int]:
    header = path.read_bytes()[:24]
    require(len(header) == 24, "screenshot is too short to be a PNG")
    require(header[:8] == b"\x89PNG\r\n\x1a\n", "screenshot must be a PNG")
    require(header[12:16] == b"IHDR", "screenshot PNG is missing IHDR")
    return struct.unpack(">II", header[16:24])


def write_atomic(path: Path, content: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    handle = tempfile.NamedTemporaryFile(
        mode="w",
        encoding="utf-8",
        dir=str(path.parent),
        prefix=f".{path.name}.",
        delete=False,
    )
    temporary = Path(handle.name)
    try:
        with handle:
            handle.write(content)
            handle.flush()
            os.fsync(handle.fileno())
        os.replace(temporary, path)
    finally:
        temporary.unlink(missing_ok=True)


def main() -> None:
    values = {name: argument_value(name) for name in REQUIRED_ARGUMENTS}
    if any(not value for value in values.values()):
        report(
            {
                "check": CHECK,
                "status": "ERROR",
                "reason": (
                    "required arguments: --contract, --manager-repo, --backend-repo, "
                    "--evidence-dir"
                ),
            },
            2,
        )

    try:
        contract = json.loads(Path(values["--contract"]).read_text(encoding="utf-8"))
        manager_repo = Path(values["--manager-repo"]).resolve()
        backend_repo = Path(values["--backend-repo"]).resolve()
        evidence_dir = Path(values["--evidence-dir"]).resolve()

        require(contract.get("schemaVersion") == 1, "unsupported contract schemaVersion")
        require(contract.get("contract") == CHECK, "contract name mismatch")
        revisions = contract["revisions"]
        manager_commit = revisions["manager"]
        backend_commit = revisions["backend"]

        compose_source = git_show(
            manager_repo,
            manager_commit,
            contract["managerComposePath"],
        )
        for token in contract["managerComposeRequiredTokens"]:
            require(token in compose_source, f"manager Compose missing required token: {token}")

        stale_origins = contract["staleAssetOrigins"]
        for stale_origin in stale_origins:
            require(
                stale_origin not in compose_source,
                f"manager Compose contains stale origin: {stale_origin}",
            )

        backend_source = "\n".join(
            (
                git_show(backend_repo, backend_commit, contract["backendFixturePath"]),
                git_show(backend_repo, backend_commit, contract["backendSeederPath"]),
            )
        )
        for token in contract["backendRequiredTokens"]:
            require(
                token in backend_source,
                f"backend fixture/bootstrap missing token: {token}",
            )

        evidence = contract["evidence"]
        preview_path = evidence_dir / evidence["previewJson"]
        screenshot_path = evidence_dir / evidence["screenshot"]
        manifest_path = evidence_dir / evidence["manifest"]
        for path in (preview_path, screenshot_path, manifest_path):
            require(path.is_file(), f"missing evidence artifact: {path.name}")
        require(
            sha256(preview_path) == evidence["previewJsonSha256"],
            "preview JSON SHA-256 mismatch",
        )
        require(
            sha256(screenshot_path) == evidence["screenshotSha256"],
            "screenshot SHA-256 mismatch",
        )
        require(
            sha256(manifest_path) == evidence["manifestSha256"],
            "manifest SHA-256 mismatch",
        )

        preview = json.loads(preview_path.read_text(encoding="utf-8"))
        published = json.loads(manifest_path.read_text(encoding="utf-8"))
        manifest = published["manifest"]
        expected = contract["expected"]

        require(preview["status"] == "PASS", "preview evidence status must be PASS")
        require(
            preview["source"]["managerCommit"] == manager_commit,
            "preview manager revision mismatch",
        )
        require(
            preview["source"]["backendCommit"] == backend_commit,
            "preview backend revision mismatch",
        )
        require(preview["fixture"]["courseId"] == expected["courseId"], "preview course mismatch")
        require(preview["fixture"]["lessonId"] == expected["lessonId"], "preview lesson mismatch")
        require(preview["fixture"]["lessonVersion"] == expected["lessonVersion"], "preview lesson version mismatch")
        require(
            preview["fixture"]["manifestChecksum"] == expected["manifestChecksum"],
            "preview manifest checksum mismatch",
        )
        require(preview["previewWordText"] == expected["word"], "preview teaching word mismatch")
        require(
            preview["previewPathOutcome"] == expected["pathOutcome"],
            "preview path outcome mismatch",
        )
        require(bool(preview["previewMotionTimeline"]), "preview motion timeline must be non-empty")
        require(bool(preview["previewLayerRects"]), "preview layer rectangles must be non-empty")
        for name, rect in preview["previewLayerRects"].items():
            require(len(rect) == 4 and rect[2] > 0 and rect[3] > 0, f"invalid preview rect: {name}")

        expected_dimensions = (expected["width"], expected["height"])
        require(png_dimensions(screenshot_path) == expected_dimensions, "screenshot dimensions mismatch")
        require(
            (preview["screenshot"]["width"], preview["screenshot"]["height"]) == expected_dimensions,
            "recorded screenshot dimensions mismatch",
        )
        require(
            preview["screenshot"]["sha256"] == evidence["screenshotSha256"],
            "recorded screenshot SHA mismatch",
        )

        require(published["status"] == "published", "manifest artifact must be published")
        require(published["checksum"] == expected["manifestChecksum"], "published checksum mismatch")
        require(manifest["courseId"] == expected["courseId"], "manifest course mismatch")
        require(manifest["lessonId"] == expected["lessonId"], "manifest lesson mismatch")
        require(manifest["lessonVersion"] == expected["lessonVersion"], "manifest version mismatch")

        asset_origin = expected["assetOrigin"].rstrip("/")
        require(asset_origin not in stale_origins, "expected asset origin is stale")
        require(
            preview["environment"]["assetOrigin"].rstrip("/") == asset_origin,
            "preview asset origin mismatch",
        )
        require(
            all(
                code == 200
                for code in preview["environment"]["assetRequests"].values()
            ),
            "preview asset request failed",
        )
        require(
            preview["environment"]["browserConsoleErrors"] == 0,
            "browser console errors were recorded",
        )

        assets = manifest["assets"]
        require(bool(assets), "manifest assets must be non-empty")
        require(len(assets) == expected["assetCount"], "manifest asset count mismatch")
        for asset in assets:
            digest = asset["sha256"]
            require(bool(re.fullmatch(r"[0-9a-f]{64}", digest)), f"invalid asset SHA-256: {digest}")
            require(
                asset["path"] == f"lesson-assets/{digest}",
                f"asset path is not content-addressed: {asset['id']}",
            )
            require(
                asset["url"] == f"{asset_origin}/{asset['path']}",
                f"asset URL origin mismatch: {asset['id']}",
            )

        local_bootstrap = contract["localBootstrap"]
        require(
            local_bootstrap["LESSON_SAMPLE_ENABLED"] == "false",
            "local sample mode must be disabled",
        )
        local_origin = local_bootstrap["LESSON_ASSET_ORIGIN_BASE"].rstrip("/")
        local_public = local_bootstrap["LESSON_ASSET_PUBLIC_BASE_URL"].rstrip("/")
        require(
            local_origin != local_public,
            "local origin and runtime public base must be distinct",
        )
        require(local_origin not in stale_origins, "local bootstrap uses a stale asset origin")
        require(local_public not in stale_origins, "local bootstrap uses a stale public base")

        env_content = "".join(
            f"{key}={local_bootstrap[key]}\n" for key in sorted(local_bootstrap)
        )
        env_hash = hashlib.sha256(env_content.encode("utf-8")).hexdigest()
        env_output = argument_value("--compose-env-out")
        if env_output:
            write_atomic(Path(env_output).resolve(), env_content)

        report(
            {
                "check": CHECK,
                "status": "PASS",
                "managerCommit": manager_commit,
                "backendCommit": backend_commit,
                "manifestChecksum": expected["manifestChecksum"],
                "assetCount": len(assets),
                "preview": {
                    "width": expected["width"],
                    "height": expected["height"],
                    "word": expected["word"],
                },
                "composeEnvSha256": env_hash,
            },
            0,
        )
    except Exception as error:
        report({"check": CHECK, "status": "FAIL", "reason": str(error)}, 1)


if __name__ == "__main__":
    main()
