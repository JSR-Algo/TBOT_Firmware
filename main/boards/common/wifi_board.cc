#include "wifi_board.h"

#include "display.h"
#include "application.h"
#include "system_info.h"
#include "settings.h"
#include "assets/lang_config.h"

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_network.h>
#include <esp_log.h>
#include <utility>

#include <font_awesome.h>
#include <wifi_manager.h>
#include <wifi_station.h>
#include <ssid_manager.h>
#include "afsk_demod.h"
#ifdef CONFIG_USE_ESP_BLUFI_WIFI_PROVISIONING
#include "blufi.h"
#endif
#ifdef CONFIG_TBOT_PROVISIONING_REPORT_ENABLED
#include "provisioning_status_reporter.h"
#endif

static const char *TAG = "WifiBoard";

// Connection timeout in seconds
static constexpr int CONNECT_TIMEOUT_SEC = 60;

WifiBoard::WifiBoard() {
    // Create connection timeout timer
    esp_timer_create_args_t timer_args = {
        .callback = OnWifiConnectTimeout,
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "wifi_connect_timer",
        .skip_unhandled_events = true
    };
    esp_timer_create(&timer_args, &connect_timer_);
}

WifiBoard::~WifiBoard() {
    if (connect_timer_) {
        esp_timer_stop(connect_timer_);
        esp_timer_delete(connect_timer_);
    }
    if (ap_setup_timer_) {
        esp_timer_stop(ap_setup_timer_);
        esp_timer_delete(ap_setup_timer_);
        ap_setup_timer_ = nullptr;
    }
}

std::string WifiBoard::GetBoardType() {
    return "wifi";
}

void WifiBoard::StartNetwork() {
    auto& wifi_manager = WifiManager::GetInstance();

    // Initialize WiFi manager
    WifiManagerConfig config;
    config.ssid_prefix = "TBot";
    config.language = Lang::CODE;
    wifi_manager.Initialize(config);

    // Set unified event callback - forward to NetworkEvent with SSID data
    wifi_manager.SetEventCallback([this](WifiEvent event, const std::string& data) {
        switch (event) {
            case WifiEvent::Scanning:
                OnNetworkEvent(NetworkEvent::Scanning);
                break;
            case WifiEvent::Connecting:
                OnNetworkEvent(NetworkEvent::Connecting, data);
                break;
            case WifiEvent::Connected:
                OnNetworkEvent(NetworkEvent::Connected, data);
                break;
            case WifiEvent::Disconnected:
                OnNetworkEvent(NetworkEvent::Disconnected);
                break;
            case WifiEvent::ConfigModeEnter:
                OnNetworkEvent(NetworkEvent::WifiConfigModeEnter);
                break;
            case WifiEvent::ConfigModeExit:
                OnNetworkEvent(NetworkEvent::WifiConfigModeExit);
                break;
        }
    });

    if (in_config_mode_) {
        ESP_LOGI(TAG, "StartNetwork skipped auto-connect because config mode is already active");
        return;
    }

    // Try to connect or enter config mode
    TryWifiConnect();
}

void WifiBoard::TryWifiConnect() {
    auto& ssid_manager = SsidManager::GetInstance();
    bool have_ssid = !ssid_manager.GetSsidList().empty();

    if (have_ssid) {
        auto& app = Application::GetInstance();
        app.EnsureBleAdvertisingForUnclaimedSavedWifi();
        // Start connection attempt with timeout
        ESP_LOGI(TAG, "Starting WiFi connection attempt");
        esp_timer_start_once(connect_timer_, CONNECT_TIMEOUT_SEC * 1000000ULL);
        WifiManager::GetInstance().StartStation();
    } else {
        // No SSID configured, enter config mode
        // Wait for the board version to be shown
        vTaskDelay(pdMS_TO_TICKS(1500));
        StartWifiConfigMode();
    }
}

void WifiBoard::OnNetworkEvent(NetworkEvent event, const std::string& data) {
    switch (event) {
        case NetworkEvent::Connected:
            // Stop timeout timer
            esp_timer_stop(connect_timer_);
            // Provisioning succeeded → AP (if it was open) must stop. Cancel the
            // AP hard-timeout so a stale callback cannot post a redundant
            // StopConfigAp after we have already moved on.
            CancelApSetupTimeout();
#if defined(CONFIG_USE_ESP_BLUFI_WIFI_PROVISIONING) && defined(CONFIG_TBOT_PROVISIONING_REPORT_ENABLED)
            {
                auto& blufi = Blufi::GetInstance();
                const std::string& token = blufi.GetBootstrapToken();
                const std::string& code  = blufi.GetProvisioningCode();
                // Report() now lives in blufi.cpp success branch (single owner).
                // Do not call Report() here to avoid duplicate POST + prior-reconnect race.
                ESP_LOGI(TAG,
                         "NetworkEvent::Connected token_empty=%d code_empty=%d — report owned by BluFi success branch",
                         (int)token.empty(), (int)code.empty());
                // Cancel the BLE hard-timeout before releasing BLE resources so
                // the timer cannot post a redundant teardown after deinit().
                blufi.CancelBleSetupTimeout();
                // Release BLE resources
                blufi.deinit();
            }
#elif defined(CONFIG_USE_ESP_BLUFI_WIFI_PROVISIONING)
            {
                auto& blufi = Blufi::GetInstance();
                // Cancel BLE hard-timeout before deinit to prevent a stale
                // timer callback from posting a redundant teardown.
                blufi.CancelBleSetupTimeout();
                // make sure blufi resources has been released
                blufi.deinit();
            }
#endif
            in_config_mode_ = false;
            ESP_LOGI(TAG, "Connected to WiFi: %s", data.c_str());
            break;
        case NetworkEvent::Scanning:
            ESP_LOGI(TAG, "WiFi scanning");
            break;
        case NetworkEvent::Connecting:
            ESP_LOGI(TAG, "WiFi connecting to %s", data.c_str());
            break;
        case NetworkEvent::Disconnected:
            ESP_LOGW(TAG, "WiFi disconnected");
            break;
        case NetworkEvent::WifiConfigModeEnter:
            ESP_LOGI(TAG, "WiFi config mode entered");
            in_config_mode_ = true;
            break;
        case NetworkEvent::WifiConfigModeExit:
            ESP_LOGI(TAG, "WiFi config mode exited");
            in_config_mode_ = false;
            // Leaving config mode (credentials received) is a successful exit of
            // AP setup — cancel the hard-timeout before attempting connection.
            CancelApSetupTimeout();
            // Try to connect with the new credentials
            TryWifiConnect();
            break;
        default:
            break;
    }

    // Notify external callback if set
    if (network_event_callback_) {
        network_event_callback_(event, data);
    }
}

void WifiBoard::SetNetworkEventCallback(NetworkEventCallback callback) {
    network_event_callback_ = std::move(callback);
}

void WifiBoard::OnWifiConnectTimeout(void* arg) {
    auto* board = static_cast<WifiBoard*>(arg);
    ESP_LOGW(TAG, "WiFi connection timeout, entering config mode");

    WifiManager::GetInstance().StopStation();
    board->StartWifiConfigMode();
}

// ---------------------------------------------------------------------------
// AP-setup hard-timeout safety gate (mirrors the BLE gate in blufi.cpp).
// SoftAP must not run forever: §"AP must not run forever" / "AP setup timeout
// default: 10 minutes". The timer callback runs in the esp_timer task; tearing
// down the AP there would race the Wi-Fi/event tasks, so we only latch the flag
// and post StopConfigAp() to the Application task.
// ---------------------------------------------------------------------------

void WifiBoard::OnApSetupTimeout(void* arg) {
    auto* board = static_cast<WifiBoard*>(arg);
    ESP_LOGW(TAG, "AP setup TIMEOUT -> StopConfigAp posted to Application task");
    board->ap_timed_out_ = true;

    Application::GetInstance().Schedule([board]() {
        ESP_LOGW(TAG, "AP setup TIMEOUT teardown executing on Application task");
        WifiManager::GetInstance().StopConfigAp();
        board->in_config_mode_ = false;
    });
}

void WifiBoard::StartApSetupTimeout(int seconds) {
    if (ap_setup_timer_ != nullptr) {
        // Already armed — stop and delete so we can re-create cleanly.
        esp_timer_stop(ap_setup_timer_);
        esp_timer_delete(ap_setup_timer_);
        ap_setup_timer_ = nullptr;
    }
    ap_timed_out_ = false;

    esp_timer_create_args_t args = {
        .callback = OnApSetupTimeout,
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "ap_setup_timer",
        .skip_unhandled_events = true,
    };
    esp_err_t err = esp_timer_create(&args, &ap_setup_timer_);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create AP setup timer: %s", esp_err_to_name(err));
        ap_setup_timer_ = nullptr;
        return;
    }
    err = esp_timer_start_once(ap_setup_timer_, static_cast<uint64_t>(seconds) * 1000000ULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start AP setup timer: %s", esp_err_to_name(err));
        esp_timer_delete(ap_setup_timer_);
        ap_setup_timer_ = nullptr;
        return;
    }
    ESP_LOGI(TAG, "AP setup timer armed %ds", seconds);
}

void WifiBoard::CancelApSetupTimeout() {
    if (ap_setup_timer_ == nullptr) {
        return;  // Never armed or already cancelled — no-op.
    }
    esp_timer_stop(ap_setup_timer_);
    esp_timer_delete(ap_setup_timer_);
    ap_setup_timer_ = nullptr;
    ESP_LOGI(TAG, "AP setup cancelled (provisioned)");
}

const char* WifiBoard::GetApStateString() const {
    // AP timeout tears the SoftAP down, so the backend-safe radio state is off.
    // The explicit AP_SETUP_TIMEOUT connection state carries the timeout detail.
    if (in_config_mode_ && WifiManager::GetInstance().IsConfigMode()) {
        return "active";
    }
    return "off";
}

void WifiBoard::StartWifiConfigMode() {
    in_config_mode_ = true;
    // Transition to wifi configuring state
    Application::GetInstance().SetDeviceState(kDeviceStateWifiConfiguring);
#ifdef CONFIG_USE_HOTSPOT_WIFI_PROVISIONING
    auto& wifi_manager = WifiManager::GetInstance();

    wifi_manager.StartConfigAp();

    // Arm the AP-setup hard-timeout safety gate immediately after opening the
    // SoftAP so it cannot run forever if provisioning never completes. Teardown
    // is posted to the Application task inside the timer callback (never in
    // callback context) to avoid WDT/race conditions — mirrors the BLE gate.
    StartApSetupTimeout(CONFIG_AP_SETUP_TIMEOUT_SEC);

    // Show config prompt after a short delay
    Application::GetInstance().Schedule([&wifi_manager]() {
        std::string hint = Lang::Strings::CONNECT_TO_HOTSPOT;
        hint += wifi_manager.GetApSsid();
        hint += Lang::Strings::ACCESS_VIA_BROWSER;
        hint += wifi_manager.GetApWebUrl();

        Application::GetInstance().Alert(Lang::Strings::WIFI_CONFIG_MODE, hint.c_str(), "gear", Lang::Sounds::OGG_WIFICONFIG);
    });
#elif CONFIG_USE_ESP_BLUFI_WIFI_PROVISIONING
    auto &blufi = Blufi::GetInstance();
    Application::GetInstance().GetAudioService().ReleaseWakeWordResourcesForWifiConfig();
    // initialize esp-blufi protocol.
    // Guard against double-init: the Application now also brings BLE up while the
    // robot is unclaimed in claimable standby (so the app can discover it over
    // BLE). If we entered config mode from that state the BLE stack is already
    // advertising; calling init() again would leak the controller/host. A prior
    // hard-timeout is equivalent to off for an explicit BOOT setup entry: the
    // stack was torn down and must be initialized again so the phone can scan it.
    const auto ble_state = blufi.GetBleState();
    if (ble_state == Blufi::BleState::kOff ||
        ble_state == Blufi::BleState::kTimeout) {
        blufi.init();
    }
    // Arm the hard-timeout safety gate immediately after init so that BLE
    // advertising cannot run forever if provisioning never completes.
    // Teardown is posted to the Application task inside the timer callback
    // (never in callback context) to avoid WDT/race conditions (§9). Re-arming
    // is safe even if BLE was already up (StartBleSetupTimeout recreates the
    // timer cleanly), and it correctly switches the timeout to this explicit
    // setup window.
    blufi.StartBleSetupTimeout(CONFIG_BLE_SETUP_TIMEOUT_SEC);
#endif
#if CONFIG_USE_ACOUSTIC_WIFI_PROVISIONING
    // Start acoustic provisioning task
    auto codec = Board::GetInstance().GetAudioCodec();
    int channel = codec ? codec->input_channels() : 1;
    ESP_LOGI(TAG, "Starting acoustic WiFi provisioning, channels: %d", channel);

    xTaskCreate([](void* arg) {
        auto ch = reinterpret_cast<intptr_t>(arg);
        auto& app = Application::GetInstance();
        auto& wifi = WifiManager::GetInstance();
        auto disp = Board::GetInstance().GetDisplay();
        audio_wifi_config::ReceiveWifiCredentialsFromAudio(&app, &wifi, disp, ch);
        vTaskDelete(NULL);
    }, "acoustic_wifi", 4096, reinterpret_cast<void*>(channel), 2, NULL);
#endif
}

void WifiBoard::EnterWifiConfigMode() {
    ESP_LOGI(TAG, "EnterWifiConfigMode called");
    GetDisplay()->ShowNotification(Lang::Strings::ENTERING_WIFI_CONFIG_MODE);

    auto& app = Application::GetInstance();
    auto state = app.GetDeviceState();

    if (state == kDeviceStateWifiConfiguring) {
        esp_timer_stop(connect_timer_);
        WifiManager::GetInstance().StopStation();
        StartWifiConfigMode();
        return;
    }

    if (state == kDeviceStateSpeaking || state == kDeviceStateListening ||
        state == kDeviceStateIdle || state == kDeviceStateActivating ||
        state == kDeviceStateConnecting) {
        // Reset protocol (close audio channel, reset protocol)
        Application::GetInstance().ResetProtocol();

        xTaskCreate([](void* arg) {
            auto* board = static_cast<WifiBoard*>(arg);

            // Wait for 1 second to allow speaking to finish gracefully
            vTaskDelay(pdMS_TO_TICKS(1000));

            // Stop any ongoing connection attempt
            esp_timer_stop(board->connect_timer_);
            WifiManager::GetInstance().StopStation();

            // Enter config mode
            board->StartWifiConfigMode();

            vTaskDelete(NULL);
        }, "wifi_cfg_delay", 4096, this, 2, NULL);
        return;
    }

    if (state != kDeviceStateStarting) {
        ESP_LOGE(TAG, "EnterWifiConfigMode called but device state is not allowed for WiFi config: %d", state);
        return;
    }

    // Stop any ongoing connection attempt
    esp_timer_stop(connect_timer_);
    WifiManager::GetInstance().StopStation();

    StartWifiConfigMode();
}

bool WifiBoard::IsInWifiConfigMode() const {
    return WifiManager::GetInstance().IsConfigMode();
}

NetworkInterface* WifiBoard::GetNetwork() {
    static EspNetwork network;
    return &network;
}

const char* WifiBoard::GetNetworkStateIcon() {
    auto& wifi = WifiManager::GetInstance();

    if (wifi.IsConfigMode()) {
        return FONT_AWESOME_WIFI;
    }
    if (!wifi.IsConnected()) {
        return FONT_AWESOME_WIFI_SLASH;
    }

    int rssi = wifi.GetRssi();
    if (rssi >= -65) {
        return FONT_AWESOME_WIFI;
    } else if (rssi >= -75) {
        return FONT_AWESOME_WIFI_FAIR;
    }
    return FONT_AWESOME_WIFI_WEAK;
}

std::string WifiBoard::GetBoardJson() {
    auto& wifi = WifiManager::GetInstance();
    std::string json = R"({"type":")" + std::string(BOARD_TYPE) + R"(",)";
    json += R"("name":")" + std::string(BOARD_NAME) + R"(",)";

    if (!wifi.IsConfigMode()) {
        json += R"("ssid":")" + wifi.GetSsid() + R"(",)";
        json += R"("rssi":)" + std::to_string(wifi.GetRssi()) + R"(,)";
        json += R"("channel":)" + std::to_string(wifi.GetChannel()) + R"(,)";
        json += R"("ip":")" + wifi.GetIpAddress() + R"(",)";
    }

    json += R"("mac":")" + SystemInfo::GetMacAddress() + R"("})";
    return json;
}

void WifiBoard::SetPowerSaveLevel(PowerSaveLevel level) {
    WifiPowerSaveLevel wifi_level;
    switch (level) {
        case PowerSaveLevel::LOW_POWER:
            wifi_level = WifiPowerSaveLevel::LOW_POWER;
            break;
        case PowerSaveLevel::BALANCED:
            wifi_level = WifiPowerSaveLevel::BALANCED;
            break;
        case PowerSaveLevel::PERFORMANCE:
        default:
            wifi_level = WifiPowerSaveLevel::PERFORMANCE;
            break;
    }
    WifiManager::GetInstance().SetPowerSaveLevel(wifi_level);
}

std::string WifiBoard::GetDeviceStatusJson() {
    auto& board = Board::GetInstance();
    auto root = cJSON_CreateObject();

    // Audio speaker
    auto audio_speaker = cJSON_CreateObject();
    if (auto codec = board.GetAudioCodec()) {
        cJSON_AddNumberToObject(audio_speaker, "volume", codec->output_volume());
    }
    cJSON_AddItemToObject(root, "audio_speaker", audio_speaker);

    // Screen
    auto screen = cJSON_CreateObject();
    if (auto backlight = board.GetBacklight()) {
        cJSON_AddNumberToObject(screen, "brightness", backlight->brightness());
    }
    if (auto display = board.GetDisplay(); display && display->height() > 64) {
        if (auto theme = display->GetTheme()) {
            cJSON_AddStringToObject(screen, "theme", theme->name().c_str());
        }
    }
    cJSON_AddItemToObject(root, "screen", screen);

    // Battery
    int level = 0;
    bool charging = false, discharging = false;
    if (board.GetBatteryLevel(level, charging, discharging)) {
        auto battery = cJSON_CreateObject();
        cJSON_AddNumberToObject(battery, "level", level);
        cJSON_AddBoolToObject(battery, "charging", charging);
        cJSON_AddItemToObject(root, "battery", battery);
    }

    // Network
    auto& wifi = WifiManager::GetInstance();
    auto network = cJSON_CreateObject();
    cJSON_AddStringToObject(network, "type", "wifi");
    cJSON_AddStringToObject(network, "ssid", wifi.GetSsid().c_str());
    int rssi = wifi.GetRssi();
    const char* signal = rssi >= -60 ? "strong" : (rssi >= -70 ? "medium" : "weak");
    cJSON_AddNumberToObject(network, "rssi", rssi);
    cJSON_AddStringToObject(network, "signal", signal);
    cJSON_AddItemToObject(root, "network", network);

#ifdef CONFIG_USE_ESP_BLUFI_WIFI_PROVISIONING
    cJSON_AddStringToObject(root, "ble_state", Blufi::GetInstance().GetBleStateString());
#else
    cJSON_AddStringToObject(root, "ble_state", "off");
#endif
    cJSON_AddStringToObject(root, "ap_state", GetApStateString());

    // Chip temperature
    float temp = 0.0f;
    if (board.GetTemperature(temp)) {
        auto chip = cJSON_CreateObject();
        cJSON_AddNumberToObject(chip, "temperature", temp);
        cJSON_AddItemToObject(root, "chip", chip);
    }

    auto str = cJSON_PrintUnformatted(root);
    std::string result(str);
    cJSON_free(str);
    cJSON_Delete(root);
    return result;
}
