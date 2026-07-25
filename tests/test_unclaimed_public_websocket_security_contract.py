from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def read(path: str) -> str:
    return (ROOT / path).read_text(encoding="utf-8")


def function_body(text: str, signature: str) -> str:
    start = text.index(signature)
    brace = text.index("{", start)
    depth = 0
    for index in range(brace, len(text)):
        char = text[index]
        if char == "{":
            depth += 1
        elif char == "}":
            depth -= 1
            if depth == 0:
                return text[brace:index]
    raise AssertionError(f"unterminated function {signature}")


def test_unclaimed_public_websocket_has_explicit_fail_closed_lesson_mode():
    header = read("main/protocols/websocket_protocol.h")
    source = read("main/protocols/websocket_protocol.cc")
    open_body = function_body(source, "bool WebsocketProtocol::OpenAudioChannel")

    assert "enum class WebsocketSessionMode" in header
    assert "kUnclaimedPublicLesson" in header
    assert "WebsocketSessionMode session_mode_" in header
    assert "bool IsAllowedUnclaimedPublicLessonMessage" in header
    assert "session_mode_ = token.empty()" in open_body
    assert "WebsocketSessionMode::kUnclaimedPublicLesson" in open_body
    assert "WebsocketSessionMode::kAuthenticatedRealtime" in open_body


def test_unclaimed_public_websocket_permits_only_exact_lesson_sync_mcp_call():
    source = read("main/protocols/websocket_protocol.cc")
    helper = function_body(source, "bool WebsocketProtocol::IsAllowedUnclaimedPublicLessonMessage")

    assert '"mcp"' in helper
    assert '"jsonrpc"' in helper
    assert '"2.0"' in helper
    assert '"method"' in helper
    assert '"tools/call"' in helper
    assert '"params"' in helper
    assert '"name"' in helper
    assert '"self.lesson_assets.sync_to_sd"' in helper
    assert '"arguments"' in helper
    assert "cJSON_IsNumber(id)" in helper
    for forbidden in (
        "self.reboot",
        "self.get_system_info",
        "self.lesson_assets.sync_sample_to_sd",
        "self.lesson_assets.evict_cache_key",
        "self.lesson_assets.hil",
    ):
        assert forbidden not in helper


def test_unclaimed_public_websocket_drops_forbidden_realtime_and_mcp_frames_before_app_dispatch():
    source = read("main/protocols/websocket_protocol.cc")
    on_data = source[
        source.index("websocket_->OnData") :
        source.index("websocket_->OnDisconnected", source.index("websocket_->OnData"))
    ]
    public_gate = on_data[
        on_data.index("if (session_mode_ == WebsocketSessionMode::kUnclaimedPublicLesson)") :
        on_data.index("if (strncmp(type->valuestring", on_data.index("if (session_mode_ == WebsocketSessionMode::kUnclaimedPublicLesson)"))
    ]

    assert "IsAllowedUnclaimedPublicLessonMessage(root)" in public_gate
    assert "return;" in public_gate
    assert "on_incoming_json_(root, callback_transport_epoch);" in public_gate
    assert public_gate.index("IsAllowedUnclaimedPublicLessonMessage(root)") < public_gate.index(
        "on_incoming_json_(root, callback_transport_epoch);"
    )
    for forbidden_type in ("tts", "system", "alert", "custom", "robot_action", "stt", "llm", "lesson_"):
        assert forbidden_type not in public_gate

    authenticated = on_data[
        on_data.index("if (on_incoming_json_ != nullptr)", on_data.index("if (session_mode_ == WebsocketSessionMode::kUnclaimedPublicLesson)")) :
    ]
    assert "on_incoming_json_(root, callback_transport_epoch);" in authenticated
