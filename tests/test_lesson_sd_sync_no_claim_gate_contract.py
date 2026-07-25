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
        "audio_service_.PrewarmWakeWord",
    ):
        assert claimed_call not in before_transport
        assert claimed_call in claimed_only


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
