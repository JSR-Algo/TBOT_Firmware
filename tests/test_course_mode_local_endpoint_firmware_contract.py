import subprocess
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
FLAG = "CONFIG_TBOT_COURSE_MODE_LOCAL_ENDPOINT"
TASK07_OTA_URL = "http://192.168.0.117:8003/tbot/ota/"
TASK07_WEBSOCKET_URL = "ws://192.168.0.117:8000/tbot/v1/"


def read(path: str) -> str:
    return (ROOT / path).read_text(encoding="utf-8")


def function(source: str, signature: str, next_signature: str) -> str:
    return source[source.index(signature) : source.index(next_signature)]


def local_else_branch(source: str) -> str:
    return source.split("#else", 1)[1].split("#endif", 1)[0]


def test_task07_local_overlay_pins_only_the_authorized_lab_endpoints():
    overlay = read("sdkconfig.defaults.task07-local")

    assert f"{FLAG}=y" in overlay
    assert f'CONFIG_OTA_URL="{TASK07_OTA_URL}"' in overlay
    assert f'CONFIG_WEBSOCKET_URL="{TASK07_WEBSOCKET_URL}"' in overlay
    assert "esp.tjbot.vn" not in overlay


def preprocess_local(source: str, *defines: str) -> str:
    result = subprocess.run(
        [
            "clang++",
            "-E",
            "-P",
            "-x",
            "c++",
            f"-D{FLAG}=1",
            *(f"-D{define}=1" for define in defines),
            "-",
        ],
        input=source,
        text=True,
        capture_output=True,
        check=False,
    )
    assert result.returncode == 0, result.stderr
    return result.stdout


def test_local_ota_uses_only_validated_compiled_url_without_nvs_recovery():
    source = read("main/ota.cc")
    getter = function(source, "std::string Ota::GetCheckVersionUrl()", "std::unique_ptr<Http>")
    check = function(source, "esp_err_t Ota::CheckVersion()", "void Ota::MarkCurrentVersionValid()")

    assert f"#if {FLAG}" in getter
    assert "IsValidCourseModeOtaUrl(CONFIG_OTA_URL)" in getter
    assert "return CONFIG_OTA_URL;" in getter
    local_getter = getter.split("#else", 1)[0]
    assert "Settings" not in local_getter
    assert "ota_url" not in local_getter

    local_check = check.split("#else", 1)[0]
    assert "BuildCheckVersionUrls" not in local_check
    assert "PersistRecoveredOtaUrl" not in local_check
    assert "CONFIG_OTA_URL" in local_check
    assert "urls" not in local_check


def test_local_ota_response_is_ram_only_and_rejects_production_fields():
    header = read("main/ota.h")
    source = read("main/ota.cc")

    assert "GetTransientWebsocketUrl" in header
    assert "GetTransientWebsocketToken" in header
    assert "transient_websocket_url_" in header
    assert "transient_websocket_token_" in header
    assert "ParseCourseModeResponse" in source
    parser = function(source, "bool Ota::ParseCourseModeResponse", "std::unique_ptr<Http>")
    for prohibited in ("firmware", "mqtt", "api_url", "claim_reset", "activation", "server_time"):
        assert f'"{prohibited}"' in parser
    assert "factory_test_claimed" in parser
    assert "IsValidCourseModeWebsocketUrl" in parser
    assert "CONFIG_WEBSOCKET_URL" in parser
    assert parser.index("if (!cJSON_IsObject(websocket))") < parser.index(
        'cJSON_GetObjectItem(websocket, "url")'
    )
    assert "websocket_field_count != 2" in parser
    assert "Settings" not in parser
    assert "SetString" not in parser
    assert "SetInt" not in parser


def test_local_compiled_websocket_url_survives_failed_ota_response_handoff():
    ota = read("main/ota.cc")
    websocket = read("main/protocols/websocket_protocol.cc")

    constructor = function(ota, "Ota::Ota()", "Ota::~Ota()")
    assert f"#if {FLAG}" in constructor
    local_constructor = constructor.split(f"#if {FLAG}", 1)[1].split("#endif", 1)[0]
    assert "IsValidCourseModeWebsocketUrl(CONFIG_WEBSOCKET_URL)" in local_constructor
    assert "transient_websocket_url_ = CONFIG_WEBSOCKET_URL;" in local_constructor

    parser = function(ota, "bool Ota::ParseCourseModeResponse", "std::unique_ptr<Http>")
    assert "transient_websocket_url_.clear();" not in parser
    assert "transient_websocket_token_.clear();" in parser

    setter = function(
        websocket,
        "void WebsocketProtocol::SetTransientConfig",
        "void WebsocketProtocol::SetUnclaimedPublicLessonOnly",
    )
    local_setter = setter.split("#else", 1)[0]
    assert "IsValidCourseModeWebsocketUrl(url)" in local_setter
    assert "url != CONFIG_WEBSOCKET_URL" in local_setter
    assert "token.empty()" not in local_setter
    assert "transient_configured_ = true;" in local_setter


def test_local_websocket_requires_transient_config_and_never_reads_settings():
    header = read("main/protocols/websocket_protocol.h")
    source = read("main/protocols/websocket_protocol.cc")

    assert "SetTransientConfig" in header
    refresh = function(source, "void WebsocketProtocol::RefreshSettings()", "bool WebsocketProtocol::IsAllowed")
    local_refresh = refresh.split("#else", 1)[0]
    assert f"#if {FLAG}" in local_refresh
    assert "Settings settings" not in local_refresh
    assert "CONFIG_WEBSOCKET_URL" not in local_refresh
    assert "transient_configured_" in local_refresh
    assert "url_.clear()" in local_refresh

    open_channel = function(source, "bool WebsocketProtocol::OpenAudioChannel()", "std::string WebsocketProtocol::GetHelloMessage")
    assert "if (url.empty())" in open_channel
    assert "return false;" in open_channel

    start = function(source, "bool WebsocketProtocol::Start()", "bool WebsocketProtocol::SendAudio")
    assert f"#if {FLAG}" in start
    local_start = start.split("#else", 1)[0]
    assert "return OpenAudioChannel();" in local_start


def test_local_application_passes_ota_transient_config_without_endpoint_nvs():
    source = read("main/application.cc")
    initialize = function(source, "void Application::InitializeProtocol()", "bool Application::HandleRobotActionMessage")
    local_branch = local_else_branch(initialize)

    assert f"#if !{FLAG}" in initialize
    assert "GetTransientWebsocketUrl" in local_branch
    assert "GetTransientWebsocketToken" in local_branch
    assert "SetTransientConfig" in local_branch
    assert "Settings websocket_settings" not in local_branch
    assert "IsDeviceClaimed" not in local_branch
    assert "MqttProtocol" not in local_branch


def test_local_application_keeps_ota_ram_config_for_current_boot():
    source = read("main/application.cc")
    activation_done = function(
        source,
        "void Application::HandleActivationDoneEvent()",
        "void Application::RefreshPendingTbotClaim()",
    )

    assert f"#if !{FLAG}" in activation_done
    assert activation_done.index(f"#if !{FLAG}") < activation_done.index("ota_.reset();")

def test_local_application_bypasses_production_ownership_network_paths():
    source = read("main/application.cc")
    required_guards = {
        "void Application::RefreshPendingTbotClaim()": "void Application::CompleteUnclaimedProtocolOnlyActivation()",
        "void Application::MaybeDispatchDeferredCloudRelease()": "void Application::CloudReleaseTask(void* arg)",
        "void Application::StartClaimPoll()": "void Application::StopClaimPoll()",
        "void Application::StartHeartbeat()": "void Application::StopHeartbeat()",
        "void Application::DispatchDeviceHeartbeat()": "void Application::HeartbeatTask(void* arg)",
    }

    for signature, next_signature in required_guards.items():
        body = function(source, signature, next_signature)
        assert f"#if {FLAG}" in body, signature
        local_branch = body.split("#else", 1)[0]
        assert "return;" in local_branch, signature

    activation = function(source, "void Application::ActivationTask()", "void Application::CheckAssetsVersion()")
    assert f"#if !{FLAG}" in activation
    local_activation = local_else_branch(activation)
    assert "CheckNewVersion();" in local_activation
    assert "RefreshWebsocketUrlFromConfigFetch" not in local_activation
    assert "CheckAssetsVersion" not in local_activation


def test_local_claim_refresh_guard_does_not_remove_other_method_definitions():
    source = read("main/application.cc")
    refresh_start = source.index("void Application::RefreshPendingTbotClaim()")
    apply_start = source.index("void Application::ApplyPendingTbotClaimFetchResult", refresh_start)
    refresh = source[refresh_start:apply_start]

    assert f"#if {FLAG}" in refresh
    assert "#else" not in refresh
    assert refresh.index("#endif") < refresh.index("MaybeDispatchDeferredCloudRelease();")
    for signature in (
        "void Application::ApplyPendingTbotClaimFetchResult",
        "bool Application::ConfirmPendingTbotClaim",
        "bool Application::ApplyPendingTbotClaimConfirmationResult",
        "bool Application::DispatchPendingTbotClaimConfirmation",
        "void Application::ClaimConfirmationTask",
        "void Application::SchedulePendingTbotClaimRefresh",
        "void Application::DispatchPendingTbotClaimRefreshForSetupGeneration",
        "void Application::PromoteFromWifiConfigAfterProvisioning",
    ):
        assert signature in source[apply_start:]
        assert signature in preprocess_local(source[refresh_start:source.index(
            "void Application::CompleteUnclaimedProtocolOnlyActivation()", apply_start
        )])


def test_local_blufi_ignores_claim_custom_data_without_nvs_or_network_scheduling():
    source = read("main/boards/common/blufi.cpp")
    event = source[
        source.index("case ESP_BLUFI_EVENT_RECV_CUSTOM_DATA") :
        source.index("default:", source.index("case ESP_BLUFI_EVENT_RECV_CUSTOM_DATA"))
    ]

    assert f"#if {FLAG}" in event
    local_branch = event.split("#else", 1)[0]
    production_branch = event.split("#else", 1)[1]
    assert "SecureClearCustomDataSnapshot(snapshot)" in local_branch
    assert "bootstrap_token_.clear()" in local_branch
    assert "provisioning_code_.clear()" in local_branch
    assert "Settings" not in local_branch
    assert "SetString" not in local_branch
    assert "Application::GetInstance().Schedule" not in local_branch
    assert "ScheduleClaimRefreshAfterTokenHandoff" not in local_branch
    assert 'SetString(\n                                "claim_device_id"' in production_branch
    assert 'SetString(\n                                "bootstrap_token"' in production_branch


def test_local_blufi_suppresses_claim_and_provisioning_network_schedulers():
    source = read("main/boards/common/blufi.cpp")
    delayed = function(
        source,
        "void Blufi::ScheduleClaimRefreshAfterTokenHandoff()",
        "void Blufi::TryReportProvisioningAuthenticated",
    )
    report = function(
        source,
        "void Blufi::TryReportProvisioningAuthenticated",
        "void Blufi::StartStationConnectFromCredentials",
    )
    clear = function(
        source,
        "void Blufi::ClearProvisioningSecrets(bool preserve_claim_token)",
        "void Blufi::_event_callback_trampoline",
    )

    for body in (delayed, report):
        assert f"#if {FLAG}" in body
        local_branch = body.split("#else", 1)[0]
        assert "return;" in local_branch
        assert "Application::GetInstance().Schedule" not in local_branch
        assert "SchedulePendingTbotClaimRefresh" not in local_branch
        assert "ProvisioningStatusReporter::Report" not in local_branch
        assert "Settings" not in local_branch

    assert f"#if {FLAG}" in clear
    clear_local = clear.split("#else", 1)[0]
    assert "bootstrap_token_.clear()" in clear_local
    assert "provisioning_code_.clear()" in clear_local
    settings_idx = clear.index("Settings websocket_settings")
    assert f"#if !{FLAG}" in clear[:settings_idx]

    connected = source.index('ESP_LOGI(BLUFI_TAG, "connected to WiFi")')
    wifi_success_start = source.index(f"#if !{FLAG}", connected)
    wifi_success_end = source.index("} else {", wifi_success_start)
    wifi_success = source[wifi_success_start:wifi_success_end]
    assert f"#if !{FLAG}" in wifi_success
    assert "SchedulePendingTbotClaimRefresh" in wifi_success
    blufi_methods = source[source.index("void Blufi::ScheduleClaimRefreshAfterTokenHandoff()") :]
    local_source = preprocess_local(blufi_methods, "CONFIG_TBOT_PROVISIONING_REPORT_ENABLED")
    assert "ProvisioningStatusReporter::Report" not in local_source
    assert "SchedulePendingTbotClaimRefresh" not in local_source
    assert '"claim_device_id"' not in local_source
    assert '"bootstrap_token"' not in local_source


def test_local_blufi_wifi_success_promotes_activation_without_claim_refresh():
    blufi = read("main/boards/common/blufi.cpp")
    application_header = read("main/application.h")
    application = read("main/application.cc")
    connected = blufi.index('ESP_LOGI(BLUFI_TAG, "connected to WiFi")')
    local_start = blufi.index("#else", blufi.index(f"#if !{FLAG}", connected))
    local_end = blufi.index("#endif", local_start)
    local_success = blufi[local_start:local_end]

    assert "CompleteSuccessfulProvisioningTeardown" in local_success
    generation_check = local_success.index(
        "generation != self->setup_generation_.load()"
    )
    unlock_index = local_success.index("continuation_lock.unlock();", generation_check)
    teardown_index = local_success.index("CompleteSuccessfulProvisioningTeardown")
    completion_index = local_success.index("completion_recorded", teardown_index)
    completion_guard = local_success.index("if (!completion_recorded)", completion_index)
    acknowledge_index = local_success.index(
        "AcknowledgeSuccessfullyCompleted", completion_guard
    )
    assert "PromoteCourseModeFromWifiConfigAfterProvisioning" in local_success
    promotion_index = local_success.index("PromoteCourseModeFromWifiConfigAfterProvisioning")
    assert (
        generation_check
        < unlock_index
        < teardown_index
        < completion_index
        < completion_guard
        < acknowledge_index
        < promotion_index
    )
    assert "SchedulePendingTbotClaimRefresh" not in local_success
    assert "TryReportProvisioningAuthenticated" not in local_success
    assert "Settings" not in local_success

    assert "void PromoteCourseModeFromWifiConfigAfterProvisioning();" in application_header
    promotion = function(
        application,
        "void Application::PromoteCourseModeFromWifiConfigAfterProvisioning()",
        "void Application::CompleteUnclaimedProtocolOnlyActivation()",
    )
    assert f"#if {FLAG}" in promotion
    local_promotion = promotion.split("#else", 1)[0]
    assert "PromoteFromWifiConfigAfterProvisioning();" in local_promotion
    assert "RefreshPendingTbotClaim" not in local_promotion
    assert "Settings" not in local_promotion


def test_production_paths_remain_in_default_off_else_branches():
    ota = read("main/ota.cc")
    application = read("main/application.cc")
    websocket = read("main/protocols/websocket_protocol.cc")

    assert f"#if {FLAG}" in ota and "#else" in ota
    assert 'Settings settings("wifi", false)' in ota
    assert "BuildCheckVersionUrls" in ota
    assert "PersistRecoveredOtaUrl" in ota
    assert f"#if {FLAG}" in websocket and "#else" in websocket
    assert 'Settings settings("websocket", false)' in websocket
    assert f"#if {FLAG}" in application and "#else" in application
    assert "RefreshWebsocketUrlFromConfigFetch();" in application
    assert "StartHeartbeat();" in application
    assert "DispatchDeviceHeartbeat();" in application
