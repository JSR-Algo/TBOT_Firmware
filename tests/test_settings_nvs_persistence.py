"""Source-level regression tests: Settings NVS erases must persist.

Static assertions over main/settings.cc (no device), following the convention in
tests/test_blufi_provisioning_stability.py (ROOT, read(), substring/regex).

Bug context (fixed 2026-06-10): the destructor only commits when
`read_write_ && dirty_`. SetString/SetInt/SetBool set dirty_=true, but
EraseKey/EraseAll did not -> an erase was staged in the NVS handle yet never
nvs_commit()'ed, so it did NOT persist across a power cycle. This silently
defeated US-005 ClearProvisioningSecrets()'s NVS bootstrap_token erase (and the
claim epic's terminal-outcome token erases at rest). These tests lock the fix.
"""

import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def read(path: str) -> str:
    return (ROOT / path).read_text(encoding="utf-8")


def _method_body(src: str, signature: str) -> str:
    start = src.index(signature)
    end = src.index("\n}", start)
    return src[start:end]


def test_destructor_commits_only_when_read_write_and_dirty():
    settings = read("main/settings.cc")
    dtor = _method_body(settings, "Settings::~Settings()")
    assert "read_write_ && dirty_" in dtor
    assert "nvs_commit(nvs_handle_)" in dtor


def test_erase_key_marks_dirty_so_the_erase_persists():
    settings = read("main/settings.cc")
    body = _method_body(settings, "void Settings::EraseKey(")
    assert "nvs_erase_key(nvs_handle_" in body
    # The erase must mark the handle dirty so the destructor commits it.
    assert "dirty_ = true;" in body, (
        "EraseKey must set dirty_=true, else the destructor skips nvs_commit() "
        "and the removal never persists across reboot"
    )
    # And only inside the read_write_ branch (not when read-only).
    rw_idx = body.index("if (read_write_)")
    else_idx = body.index("} else {")
    assert rw_idx < body.index("dirty_ = true;") < else_idx


def test_erase_all_marks_dirty_so_the_wipe_persists():
    settings = read("main/settings.cc")
    body = _method_body(settings, "void Settings::EraseAll()")
    assert "nvs_erase_all(nvs_handle_)" in body
    assert "dirty_ = true;" in body, (
        "EraseAll must set dirty_=true so the destructor commits the wipe"
    )


def test_all_mutators_consistently_mark_dirty():
    # Every read_write mutator (Set*/Erase*) must mark dirty_ so its NVS write is
    # committed; otherwise the change is silently dropped at scope exit.
    settings = read("main/settings.cc")
    for sig in (
        "void Settings::SetString(",
        "void Settings::SetInt(",
        "void Settings::SetBool(",
        "void Settings::EraseKey(",
        "void Settings::EraseAll()",
    ):
        body = _method_body(settings, sig)
        assert "dirty_ = true;" in body, f"{sig} does not mark dirty_"
