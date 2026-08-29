"""Contracts for public lesson SD sync transport on unclaimed robots.

The ESP server fans out global lesson asset sync over the raw realtime session.
An unclaimed robot must therefore bring up only the minimum WebSocket/MCP path
needed to receive ``self.lesson_assets.sync_to_sd`` while keeping claimed-only
bootstrap, token, heartbeat, and provisioning behavior behind claim gates.
"""

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
                return text[brace : index + 1]
    raise AssertionError(f"unterminated function {signature}")


def block_after_marker(text: str, marker: str) -> str:
    start = text.index(marker)
    brace = text.index("{", start)
    depth = 0
    for index in range(brace, len(text)):
        char = text[index]
        if char == "{":
            depth += 1
        elif char == "}":
            depth -= 1
            if depth == 0:
                return text[brace : index + 1]
    raise AssertionError(f"unterminated block {marker}")


def test_unclaimed_saved_wifi_boot_uses_activation_transport_path_not_done_shortcut():
    source = read("main/application.cc")
    connected = function_body(source, "void Application::HandleNetworkConnectedEvent")
    unclaimed = block_after_marker(connected, "if (!IsDeviceClaimed())")

    assert "CompleteUnclaimedProtocolOnlyActivation();" in unclaimed
    assert "ActivationTask();" not in unclaimed
    assert "xEventGroupSetBits(event_group_, MAIN_EVENT_ACTIVATION_DONE)" not in unclaimed
    assert "InitializeProtocol();" not in unclaimed
    assert "CheckAssetsVersion();" not in unclaimed
    assert "CheckNewVersion();" not in unclaimed
    assert "RefreshWebsocketUrlFromConfigFetch();" not in unclaimed
    assert "audio_service_.PrewarmWakeWord" not in unclaimed


def test_unclaimed_blufi_promotion_uses_activation_transport_path_not_done_shortcut():
    source = read("main/application.cc")
    promoted = function_body(source, "void Application::PromoteFromWifiConfigAfterProvisioning")
    unclaimed = block_after_marker(promoted, "if (!IsDeviceClaimed())")

    assert "CompleteUnclaimedProtocolOnlyActivation();" in unclaimed
    assert "ActivationTask();" not in unclaimed
    assert "xEventGroupSetBits(event_group_, MAIN_EVENT_ACTIVATION_DONE)" not in unclaimed
    assert "InitializeProtocol();" not in unclaimed
    assert "CheckAssetsVersion();" not in unclaimed
    assert "CheckNewVersion();" not in unclaimed
    assert "RefreshWebsocketUrlFromConfigFetch();" not in unclaimed
    assert "audio_service_.PrewarmWakeWord" not in unclaimed


def test_unclaimed_protocol_only_activation_helper_excludes_assets_ota_wake_and_heartbeat():
    helper = function_body(
        read("main/application.cc"),
        "void Application::CompleteUnclaimedProtocolOnlyActivation"
    )

    assert "InitializeProtocol();" in helper
    assert "MAIN_EVENT_ACTIVATION_DONE" in helper
    assert "ota_->MarkCurrentVersionValid();" in helper
    for claimed_only_or_blocking in (
        "CheckAssetsVersion",
        "CheckNewVersion",
        "RefreshWebsocketUrlFromConfigFetch",
        "PrewarmWakeWord",
        "StartHeartbeat",
        "DispatchDeviceHeartbeat",
        "audio_service_.Start",
        "EnableWakeWordDetection",
    ):
        assert claimed_only_or_blocking not in helper


def test_unclaimed_activation_initializes_protocol_before_activation_done():
    activation = function_body(read("main/application.cc"), "void Application::ActivationTask")

    unclaimed = activation.index("if (!IsDeviceClaimed())")
    protocol_init = activation.index("InitializeProtocol();")
    activation_done = activation.index("MAIN_EVENT_ACTIVATION_DONE", protocol_init)

    assert unclaimed < protocol_init < activation_done
    assert "return;" not in activation[unclaimed:protocol_init]
    assert "CheckAssetsVersion();" in activation[unclaimed:protocol_init]


def test_unclaimed_activation_does_not_run_claimed_only_bootstrap_work():
    activation = function_body(read("main/application.cc"), "void Application::ActivationTask")

    unclaimed = activation.index("if (!IsDeviceClaimed())")
    protocol_init = activation.index("InitializeProtocol();")
    before_transport = block_after_marker(activation, "if (!IsDeviceClaimed())")
    claimed_only = activation[activation.index("} else {", unclaimed):protocol_init]

    for claimed_call in (
        "CheckNewVersion();",
        "RefreshWebsocketUrlFromConfigFetch();",
    ):
        assert claimed_call not in before_transport
        assert claimed_call in claimed_only
    assert "audio_service_.PrewarmWakeWord" not in activation


def test_websocket_protocol_opens_passive_raw_session_without_claim_heartbeat():
    initialize = function_body(read("main/application.cc"), "void Application::InitializeProtocol")
    websocket_branch = initialize[
        initialize.index("if (is_websocket_protocol)") :
        initialize.index("} else {\n        protocol_->Start();", initialize.index("if (is_websocket_protocol)"))
    ]

    assert websocket_branch.count("StartPassiveLessonWebsocket();") == 2
    assert "Unclaimed device: opening passive WebSocket for public lesson sync" in websocket_branch
    unclaimed_log = websocket_branch.index(
        "Unclaimed device: opening passive WebSocket for public lesson sync"
    )
    unclaimed_call = websocket_branch.index("StartPassiveLessonWebsocket();", unclaimed_log)
    assert websocket_branch.rindex("if (IsDeviceClaimed())", 0, unclaimed_log) < unclaimed_log
    assert "StartHeartbeat" not in websocket_branch[unclaimed_log:unclaimed_call]
    assert "DispatchDeviceHeartbeat" not in websocket_branch[unclaimed_log:unclaimed_call]


def test_unclaimed_passive_websocket_success_does_not_rearm_wake_word_or_deferred_wake():
    source = read("main/application.cc")
    opened = source[source.index("protocol_->OnAudioChannelOpened") : source.index("protocol_->OnAudioChannelClosed")]
    passive_opened = opened[opened.index("if (passive_ws_intent_.load())") : opened.index("} else {")]

    assert "EnableWakeWordDetection(true)" not in passive_opened

    open_task = function_body(source, "void Application::OpenChannelTask")
    passive_success = open_task[
        open_task.index("if (passive_preconnect)") :
        open_task.index("} else if (wake_word_invoke)", open_task.index("if (passive_preconnect)"))
    ]

    assert "self->IsDeviceClaimed()" in passive_success
    for wake_action in (
        "self->FinishWakeWordInvoke(deferred_wake_word);",
        "self->audio_service_.EnableWakeWordDetection(true);",
    ):
        assert wake_action in passive_success
        assert passive_success.index("self->IsDeviceClaimed()") < passive_success.index(wake_action)


def test_unclaimed_protocol_selection_can_use_persisted_or_compile_time_websocket_url_without_ota():
    initialize = function_body(read("main/application.cc"), "void Application::InitializeProtocol")
    selection = initialize[:initialize.index("protocol_generation_.fetch_add")]

    assert 'Settings websocket_settings("websocket", false);' in selection
    assert 'websocket_settings.GetString("url", CONFIG_WEBSOCKET_URL)' in selection
    assert "has_configured_websocket_url" in selection
    assert "ota_->HasWebsocketConfig() || has_configured_websocket_url" in selection
    assert "std::make_unique<WebsocketProtocol>()" in selection
    assert "SetUnclaimedPublicLessonOnly(!IsDeviceClaimed())" in selection
    assert selection.index("has_configured_websocket_url") < selection.index(
        "std::make_unique<WebsocketProtocol>()"
    )
    assert selection.index("std::make_unique<WebsocketProtocol>()") < selection.index(
        "SetUnclaimedPublicLessonOnly(!IsDeviceClaimed())"
    )

    for token_gate in (
        "bootstrap_token",
        "device_token",
        "provisioning_token",
        "backend_uuid",
        "claim_token",
        "claim_device_id",
    ):
        assert token_gate not in selection
    for selection_line in (
        line for line in selection.splitlines()
        if line.strip().startswith(("if (", "} else if ("))
    ):
        assert "IsDeviceClaimed" not in selection_line


def test_sync_to_sd_registration_and_dispatch_are_not_claim_or_token_gated():
    source = read("main/mcp_server.cc")
    sync_start = source.index('AddUserOnlyTool("self.lesson_assets.sync_to_sd"')
    sync_end = source.index('\n\n    AddUserOnlyTool("self.assets.set_download_url"', sync_start)
    sync_body = source[sync_start:sync_end]

    forbidden = (
        "IsDeviceClaimed",
        "bootstrap_token",
        "device_token",
        "provisioning_token",
        "backend_uuid",
        "claim_token",
        "claim_device_id",
        "claim_confirmed",
    )
    for token_gate in forbidden:
        assert token_gate not in sync_body

    dispatch = function_body(source, "void McpServer::ParseMessage(const cJSON* json)")
    mcp_call = dispatch[dispatch.index('method_str == "tools/call"') :]
    for token_gate in forbidden:
        assert token_gate not in mcp_call
