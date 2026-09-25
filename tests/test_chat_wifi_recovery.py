"""A radio drop must preserve the existing selected-chat recovery intent."""
import pytest

from test_chat_playout_intake import run_terminal_application
from test_chat_recovery import recovery_fixture
from test_protocol_work_lifetime import method


@pytest.mark.parametrize("sanitize", ["address", "thread"])
@pytest.mark.parametrize("case", [0, 1, 2])
def test_network_drop_uses_chat_source_recovery(tmp_path, sanitize, case):
    def network_fixture(fixture, source, header):
        fixture = recovery_fixture(fixture, source, header)
        fixture = fixture.replace("void StartPassiveLessonWebsocket();",
                                  "void StartPassiveLessonWebsocket();void HandleNetworkDisconnectedEvent();")
        fixture = fixture.replace("void SetEmotion(const char*) {}",
                                  "void SetEmotion(const char*) {}void UpdateStatusBar(bool){}")
        fixture = fixture.replace("void Stop() {}", "void Stop() {}void PlaySound(const char*){assert(false);}")
        fixture = fixture.replace("app.state=kDeviceStateIdle;app.online_intent_=true;",
                                  "app.state=kDeviceStateSpeaking;app.online_intent_=true;")
        fixture = fixture.replace("    app.HandleChatSourceFailure();", r'''
    app.HandleNetworkDisconnectedEvent();
    assert(app.online_intent_ && "radio loss is not user-requested close");
    assert(app.chat_recovery_.kind==Application::ChatRecoveryIntent::Kind::Background);
    assert(!app.microphone_uplink_authorized_ && !app.tts_audio_accepting_);
    const auto cleanups=app.cleanups;
    app.HandleNetworkDisconnectedEvent();app.PollChatProtocolSignals();
    assert(app.cleanups==cleanups && "duplicate radio/transport failure must not restart cleanup");
''', 1)
        return (f"#define RECOVERY_CASE {case}\n#define RECOVERY_WAKE 0\n" + fixture + "\n" +
                method(source, "void Application::HandleNetworkDisconnectedEvent"))

    run_terminal_application(tmp_path, sanitize, 1, fixture_transform=network_fixture)
