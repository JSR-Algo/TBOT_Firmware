from pathlib import Path
import os
import subprocess
import sys

import pytest

from repo_paths import resolve_robot_path, resolve_robot_root


def _make_robot_tree(root: Path) -> None:
    (root / "TBOT-Firmware").mkdir(parents=True)
    (root / "esp32-server").mkdir()
    (root / "docs").mkdir()


def test_resolver_prefers_explicit_robot_root(monkeypatch, tmp_path):
    robot = tmp_path / "explicit-robot"
    _make_robot_tree(robot)
    monkeypatch.setenv("TBOT_ROBOT_ROOT", str(robot))

    assert resolve_robot_root(tmp_path / "unrelated") == robot


def test_resolver_finds_normal_source_checkout(monkeypatch, tmp_path):
    monkeypatch.delenv("TBOT_ROBOT_ROOT", raising=False)
    robot = tmp_path / "robot"
    _make_robot_tree(robot)

    assert resolve_robot_root(robot / "TBOT-Firmware") == robot


def test_resolver_uses_git_common_dir_for_linked_worktree(monkeypatch, tmp_path):
    monkeypatch.delenv("TBOT_ROBOT_ROOT", raising=False)
    robot = tmp_path / "source" / "robot"
    _make_robot_tree(robot)
    common_dir = robot / "TBOT-Firmware" / ".git"
    common_dir.mkdir()

    linked = tmp_path / "worktrees" / "TBOT-Firmware" / "feature"
    linked.mkdir(parents=True)
    assert resolve_robot_root(linked, git_common_dir=common_dir) == robot


def test_resolve_robot_path_reports_all_candidates(monkeypatch, tmp_path):
    robot = tmp_path / "robot"
    _make_robot_tree(robot)
    monkeypatch.setenv("TBOT_ROBOT_ROOT", str(robot))

    with pytest.raises(FileNotFoundError, match="required path is missing") as exc:
        resolve_robot_path("esp32-server/missing.py", start=tmp_path / "checkout")

    assert "esp32-server/missing.py" in str(exc.value)


@pytest.mark.parametrize("unsafe", ["/etc/passwd", "../outside", "docs/../../outside"])
def test_resolve_robot_path_rejects_absolute_and_parent_escape(monkeypatch, tmp_path, unsafe):
    robot = tmp_path / "robot"
    _make_robot_tree(robot)
    monkeypatch.setenv("TBOT_ROBOT_ROOT", str(robot))

    with pytest.raises(ValueError, match="relative path contained by robot root"):
        resolve_robot_path(unsafe, start=robot / "TBOT-Firmware")


def test_missing_path_still_fails_under_python_optimized_mode(tmp_path):
    robot = tmp_path / "robot"
    _make_robot_tree(robot)
    script = "from repo_paths import resolve_robot_path; resolve_robot_path('docs/missing')"
    result = subprocess.run(
        [sys.executable, "-O", "-c", script],
        cwd=Path(__file__).resolve().parents[1],
        env={**os.environ, "TBOT_ROBOT_ROOT": str(robot)},
        capture_output=True,
        text=True,
    )
    assert result.returncode != 0
    assert "FileNotFoundError" in result.stderr
