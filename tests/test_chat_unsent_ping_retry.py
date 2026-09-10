"""Exercise unadmitted ping retries through extracted production Application methods."""
import os
from pathlib import Path

import pytest

from test_chat_playout_intake import run_terminal_application


@pytest.mark.parametrize("sanitize", ["address"] + (["thread"] if os.environ.get("CHAT_APPLICATION_TSAN") == "1" else []))
@pytest.mark.parametrize("send_wake", [0, 1])
@pytest.mark.parametrize("case", ["Wake", "FullQueue", "Expiry", "Retirement", "Replacement", "Completions"])
def test_unsent_ping_retry(tmp_path, sanitize, send_wake, case):
    cases = (Path(__file__).parent / "native/chat_unsent_ping_retry_cases.inc").read_text()
    run_terminal_application(
        tmp_path, sanitize, send_wake,
        extra_tests=f"    TestUnsentPing{case}();\n",
        extra_definitions=cases,
    )
