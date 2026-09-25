"""Capability-allocated claim workers must release stacks and C++ locals."""
from pathlib import Path

import pytest

from test_protocol_work_lifetime import method


ROOT = Path(__file__).resolve().parents[1]


@pytest.mark.parametrize("task", ["ClaimFetchTask", "ClaimConfirmationTask", "CloudReleaseTask"])
def test_caps_task_releases_locals_before_matching_delete(task):
    source = (ROOT / "main/application.cc").read_text()
    assert f"xTaskCreateWithCaps(&Application::{task}," in source
    body = method(source, f"void Application::{task}")
    assert "vTaskDelete(nullptr)" not in body
    deletion = body.index("vTaskDeleteWithCaps(nullptr);")
    outer = body.index("{")
    # Worker locals belong to an inner scope, already closed before self-delete.
    inner = body.index("{", outer + 1)
    assert body[outer + 1:inner].strip() == ""
    depth = 1
    for end in range(inner + 1, deletion):
        depth += (body[end] == "{") - (body[end] == "}")
        if depth == 0:
            break
    assert depth == 0
    assert body[end + 1:deletion].strip() == ""
