"""Resolve shared robot-monorepo files from source checkouts and Git worktrees."""

import os
import subprocess
from pathlib import Path
from typing import Optional


def _looks_like_robot_root(path: Path) -> bool:
    return (path / "esp32-server").is_dir() and (path / "docs").is_dir()


def _discover_git_common_dir(start: Path) -> Optional[Path]:
    try:
        value = subprocess.run(
            ["git", "rev-parse", "--git-common-dir"],
            cwd=str(start),
            check=True,
            capture_output=True,
            text=True,
        ).stdout.strip()
    except (OSError, subprocess.CalledProcessError):
        return None
    common_dir = Path(value)
    if not common_dir.is_absolute():
        common_dir = start / common_dir
    return common_dir.resolve()


def _candidates(start: Path, git_common_dir: Optional[Path]):
    env_root = os.environ.get("TBOT_ROBOT_ROOT")
    if env_root:
        yield "TBOT_ROBOT_ROOT", Path(env_root).expanduser().resolve()

    start = start.resolve()
    for path in (start, *start.parents):
        if path.name == "TBOT-Firmware":
            yield "source checkout", path.parent

    common_dir = git_common_dir or _discover_git_common_dir(start)
    if common_dir is not None:
        common_dir = common_dir.resolve()
        firmware_root = common_dir.parent if common_dir.name == ".git" else common_dir
        yield "git common dir", firmware_root.parent


def resolve_robot_root(start: Path, git_common_dir: Optional[Path] = None) -> Path:
    searched = []
    for source, candidate in _candidates(Path(start), git_common_dir):
        searched.append(f"{source}: {candidate}")
        if _looks_like_robot_root(candidate):
            return candidate
    detail = "\n  ".join(searched) if searched else "no candidates discovered"
    raise FileNotFoundError(
        "Unable to locate the TBOT robot monorepo root. Set TBOT_ROBOT_ROOT or "
        f"run inside its source checkout. Searched:\n  {detail}"
    )


def resolve_robot_path(relative: str, start: Optional[Path] = None) -> Path:
    relative_path = Path(relative)
    if relative_path.is_absolute() or ".." in relative_path.parts:
        raise ValueError(f"Expected a relative path contained by robot root: {relative}")
    checkout = Path(start) if start is not None else Path(__file__).resolve().parent
    root = resolve_robot_root(checkout)
    path = (root / relative_path).resolve()
    try:
        path.relative_to(root.resolve())
    except ValueError as exc:
        raise ValueError(f"Expected a relative path contained by robot root: {relative}") from exc
    if not path.exists():
        raise FileNotFoundError(
        f"Resolved TBOT robot root to {root}, but required path is missing: {path}. "
        "Set TBOT_ROBOT_ROOT to the checkout containing this dependency."
        )
    return path
