from pathlib import Path
import os

import pytest

from test_chat_playout_intake import run_terminal_application


ROOT = Path(__file__).resolve().parents[1]


def interrupt_fixture(fixture, _app, _header):
    # Keep existing fixture behavior for its older scenarios. These scenarios
    # model the reset/preparation acknowledgements separately from wire delivery.
    original = "uint32_t RequestChatAudioCleanup(uint32_t,bool,bool,bool,bool=true,bool=false,ChatWakePolicy=ChatWakePolicy::Explicit) { ++cleanups;audio_service_.current=false;return 1; }"
    assert original in fixture
    fixture = fixture.replace(original, """
    bool exercise_interrupt_cleanup=false;
    unsigned interrupt_resets=0;
    uint32_t interrupt_capture=0;
    uint32_t RequestChatAudioCleanup(uint32_t,bool reset,bool,bool,bool=true,bool=false,ChatWakePolicy=ChatWakePolicy::Explicit) {
        ++cleanups;audio_service_.current=false;
        if (!exercise_interrupt_cleanup) return 1;
        chat_audio_prepared_=0;
        if (reset) { ++interrupt_resets;chat_audio_reset_serial_=++audio_service_.reset_token; }
        return ++interrupt_capture;
    }
""")
    return fixture


@pytest.mark.parametrize("send_wake", [0, 1])
@pytest.mark.parametrize("sanitize", ["address"] + (["thread"] if os.environ.get("CHAT_APPLICATION_TSAN") == "1" else []))
def test_actual_owned_interrupt_recovery(tmp_path, send_wake, sanitize):
    run_terminal_application(
        tmp_path,
        sanitize,
        send_wake,
        extra_tests="    TestOwnedInterruptRecovery();\n",
        extra_definitions=(ROOT / "tests/native/chat_interrupt_recovery_test.cc").read_text(),
        fixture_transform=interrupt_fixture,
    )
