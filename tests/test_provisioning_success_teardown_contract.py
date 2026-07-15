from pathlib import Path
import re


ROOT = Path(__file__).resolve().parents[1]


def read(path: str) -> str:
    return (ROOT / path).read_text(encoding="utf-8")


def function_body(source: str, signature: str) -> str:
    start = source.index(signature)
    brace = source.index("{", start)
    depth = 0
    for index in range(brace, len(source)):
        if source[index] == "{":
            depth += 1
        elif source[index] == "}":
            depth -= 1
            if depth == 0:
                return source[brace:index + 1]
    raise AssertionError(signature)


def test_one_helper_owns_cancel_deinit_and_conditional_rearm():
    source = read("main/boards/common/blufi.cpp")
    header = read("main/boards/common/blufi.h")
    body = function_body(source, "bool Blufi::CompleteSuccessfulProvisioningTeardown")
    assert "bool CompleteSuccessfulProvisioningTeardown(const char* reason," in header
    assert "ProvisioningToken provisioning_token);" in header
    claim = body.index("provisioning_session_.Claim(provisioning_token)")
    cancel = body.index("CancelBleSetupTimeout()")
    deinit = body.index("deinit()", cancel)
    rearm = body.index("EndWifiProvisioningAndRearm(", deinit)
    assert claim < cancel < deinit < rearm
    assert "provisioning_token" in body[rearm:rearm + 120]
    assert "if (deinit_error != ESP_OK)" in body
    assert body.index("if (deinit_error != ESP_OK)") < rearm
    assert "reason" in body
    assert "if (!rearmed)" in body
    assert body.index("if (!rearmed)") < body.index("ConsumeSuccess")


def test_provisioning_token_is_plain_generation_state_for_low_memory_paths():
    controller = read("main/audio/wake_word_lifecycle_controller.h")
    token = controller[controller.index("struct ProvisioningToken"):]
    token = token[:token.index("};")]
    assert "uint64_t generation" in token
    assert "string" not in token
    assert "shared_ptr" not in token
    binding = read("main/audio/provisioning_session_binding.h")
    assert "shared_ptr" not in binding
    assert "new " not in binding


def test_wifi_begin_token_is_bound_to_the_exact_blufi_setup_session():
    audio_h = read("main/audio/audio_service.h")
    wifi = read("main/boards/common/wifi_board.cc")
    blufi_h = read("main/boards/common/blufi.h")
    start = function_body(wifi, "void WifiBoard::StartWifiConfigMode")
    assert "WifiProvisioningToken BeginWifiProvisioning();" in audio_h
    reserve = start.index("TryReserveProvisioningSession()")
    begin = start.index("BeginWifiProvisioning()", reserve)
    commit = start.index("provisioning_reservation.Commit(provisioning_token)", begin)
    init = start.index("blufi.init()", commit)
    assert reserve < begin < commit < init
    assert "ProvisioningReservation TryReserveProvisioningSession();" in blufi_h
    assert "ProvisioningSessionBinding provisioning_session_" in blufi_h
    assert "if (!provisioning_reservation)" in start


def test_every_success_owner_passes_an_explicit_originating_token():
    sources = read("main/boards/common/blufi.cpp") + read("main/boards/common/wifi_board.cc") + read("main/application.cc")
    reasons = (
        "connected_wifi_token_handoff",
        "authenticated_report_ble_release",
        "wifi_credentials_connected",
        "provisioned_ble_disconnect",
        "claim_confirmed",
    )
    for reason in reasons:
        assert re.search(
            rf'CompleteSuccessfulProvisioningTeardown\(\s*"{reason}"\s*,\s*[^)]+\)',
            sources,
        ), reason
    assert "CaptureProvisioningSession()" in sources


def test_wifi_connect_worker_captures_session_before_any_delayed_work():
    blufi = read("main/boards/common/blufi.cpp")
    body = function_body(blufi, "void Blufi::StartStationConnectFromCredentials")
    capture = body.index("CaptureProvisioningSession()")
    delay = body.index("vTaskDelay(pdMS_TO_TICKS(500))")
    task = body.index("xTaskCreate(", delay)
    assert capture < delay < task


def test_each_other_delayed_owner_captures_before_scheduling_or_http():
    blufi = read("main/boards/common/blufi.cpp")
    app = read("main/application.cc")

    handoff = function_body(blufi, "void Blufi::ScheduleClaimRefreshAfterTokenHandoff")
    assert handoff.index("CaptureProvisioningSession()") < handoff.index("xTaskCreate(")

    report = function_body(blufi, "void Blufi::TryReportProvisioningAuthenticated")
    capture = report.index("CaptureProvisioningSession()")
    assert capture < report.index("Application::GetInstance().Schedule(", capture)

    disconnect = blufi[blufi.index("case ESP_BLUFI_EVENT_BLE_DISCONNECT:"):]
    disconnect = disconnect[:disconnect.index("case ESP_BLUFI_EVENT_SET_WIFI_OPMODE:")]
    capture = disconnect.index("CaptureProvisioningSession()")
    assert capture < disconnect.index("Application::GetInstance().Schedule(", capture)

    confirm = function_body(app, "bool Application::ConfirmPendingTbotClaim")
    assert confirm.index("CaptureProvisioningSession()") < confirm.index(
        "ClaimConfirmationReporter::Confirm"
    )


def test_network_connected_is_not_a_teardown_or_rearm_owner():
    wifi = read("main/boards/common/wifi_board.cc")
    connected = wifi[wifi.index("case NetworkEvent::Connected:"):]
    connected = connected[:connected.index("case NetworkEvent::Scanning:")]
    for forbidden in (
        "CaptureProvisioningSession",
        "CompleteSuccessfulProvisioningTeardown",
        "deinit()",
        "EndWifiProvisioningAndRearm",
    ):
        assert forbidden not in connected


def test_duplicate_success_callers_cannot_double_delete_the_timeout_timer():
    source = read("main/boards/common/blufi.cpp")
    header = read("main/boards/common/blufi.h")
    start = function_body(source, "void Blufi::StartBleSetupTimeout")
    cancel = function_body(source, "void Blufi::CancelBleSetupTimeout")
    assert "std::mutex ble_setup_timer_mutex_" in header
    assert "ble_setup_timer_mutex_" in start
    assert "ble_setup_timer_mutex_" in cancel


def test_every_approved_success_owner_uses_the_central_helper():
    blufi = read("main/boards/common/blufi.cpp")
    wifi = read("main/boards/common/wifi_board.cc")
    app = read("main/application.cc")
    for reason in (
        "connected_wifi_token_handoff",
        "authenticated_report_ble_release",
        "wifi_credentials_connected",
        "provisioned_ble_disconnect",
    ):
        assert "CompleteSuccessfulProvisioningTeardown" in blufi
        assert f'"{reason}"' in blufi
    assert "CompleteSuccessfulProvisioningTeardown" not in function_body(
        wifi, "case NetworkEvent::Connected:"
    )
    assert '"claim_confirmed", provisioning_token' in app


def test_timeout_failure_preconfirm_and_manual_teardown_never_rearm():
    blufi = read("main/boards/common/blufi.cpp")
    app = read("main/application.cc")
    timeout = function_body(blufi, "void Blufi::_ble_setup_timeout_cb")
    stop = function_body(app, "void Application::StopBleAdvertising")
    apply = function_body(app, "void Application::ApplyPendingTbotClaimFetchResult")
    confirm = function_body(app, "bool Application::ConfirmPendingTbotClaim")
    preconfirm = apply[:apply.index("ConfirmPendingTbotClaim")]
    assert "deinit();" in timeout
    assert "CompleteSuccessfulProvisioningTeardown" not in timeout
    assert "blufi.deinit();" in stop
    assert "CompleteSuccessfulProvisioningTeardown" not in stop
    assert "StopBleAdvertising();" in preconfirm
    assert "CompleteSuccessfulProvisioningTeardown" not in preconfirm
    confirmed_tail = confirm[confirm.index("if (!confirmed)"):]
    assert '"claim_confirmed", provisioning_token' in confirmed_tail
    failed_wifi = blufi[blufi.index("Failed to connect to WiFi via esp-wifi-connect"):]
    failed_wifi = failed_wifi[:failed_wifi.index("vTaskDelete(nullptr)")]
    assert "CompleteSuccessfulProvisioningTeardown" not in failed_wifi
