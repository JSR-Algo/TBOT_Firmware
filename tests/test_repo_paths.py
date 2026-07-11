from pathlib import Path

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

    with pytest.raises(AssertionError, match="required path is missing") as exc:
        resolve_robot_path("esp32-server/missing.py", start=tmp_path / "checkout")

    assert "esp32-server/missing.py" in str(exc.value)
