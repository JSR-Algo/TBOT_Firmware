#include "application_internal.h"

// ---------------------------------------------------------------------------
// Heartbeat (C5)
// ---------------------------------------------------------------------------

static std::string FirmwareVersionForHeartbeat() {
    const std::string user_agent = SystemInfo::GetUserAgent();
    const std::size_t slash = user_agent.rfind('/');
    if (slash == std::string::npos || slash + 1 >= user_agent.size()) {
        return user_agent;
    }
    return user_agent.substr(slash + 1);
}

static std::string CopyStringField(cJSON* object, const char* key, const char* fallback) {
    if (object == nullptr) {
        return fallback;
    }
    cJSON* value = cJSON_GetObjectItem(object, key);
    if (!cJSON_IsString(value) || value->valuestring == nullptr || value->valuestring[0] == '\0') {
        return fallback;
    }
    return value->valuestring;
}

static int ClampInt(int value, int min_value, int max_value) {
    if (value < min_value) {
        return min_value;
    }
    if (value > max_value) {
        return max_value;
    }
    return value;
}

static int ExtractWifiRssi(cJSON* status_root) {
    cJSON* network = status_root == nullptr ? nullptr : cJSON_GetObjectItem(status_root, "network");
    cJSON* rssi = network == nullptr ? nullptr : cJSON_GetObjectItem(network, "rssi");
    if (!cJSON_IsNumber(rssi)) {
        return -127;
    }
    return ClampInt(rssi->valueint, -127, 0);
}

static std::string ExtractWifiSsid(cJSON* status_root) {
    cJSON* network = status_root == nullptr ? nullptr : cJSON_GetObjectItem(status_root, "network");
    cJSON* ssid = network == nullptr ? nullptr : cJSON_GetObjectItem(network, "ssid");
    if (!cJSON_IsString(ssid) || ssid->valuestring == nullptr) {
        return "";
    }
    const std::size_t length = std::strlen(ssid->valuestring);
    if (length == 0 || length > 32) {
        return "";
    }
    return ssid->valuestring;
}

bool Application::ShouldKeepManagementHeartbeat() const {
    return IsDeviceClaimed() && !lesson_runtime_active_.load() &&
           GetDeviceState() == kDeviceStateIdle && !IsConnectSuccessPublicationSuppressed();
}

static std::string BuildTbotHeartbeatBody(const std::string& status_json,
                                          const std::string& device_id) {
    cJSON* status_root = cJSON_Parse(status_json.c_str());
    cJSON* root = cJSON_CreateObject();

    cJSON_AddStringToObject(root, "device_id", device_id.c_str());
    const std::string firmware_version = FirmwareVersionForHeartbeat();
    cJSON_AddStringToObject(root, "firmware_version", firmware_version.c_str());

    int battery_level = 0;
    bool charging = false;
    bool discharging = false;
    if (!Board::GetInstance().GetBatteryLevel(battery_level, charging, discharging)) {
        battery_level = 0;
    }
    battery_level = ClampInt(battery_level, 0, 100);
    cJSON_AddNumberToObject(root, "battery_level", battery_level);

    const int wifi_rssi = ExtractWifiRssi(status_root);
    cJSON* connectivity = cJSON_CreateObject();
    cJSON_AddStringToObject(connectivity, "connectivity_state", "online");
    cJSON_AddNumberToObject(connectivity, "wifi_rssi", wifi_rssi);
    const std::string wifi_ssid = ExtractWifiSsid(status_root);
    if (!wifi_ssid.empty()) {
        cJSON_AddStringToObject(connectivity, "wifi_ssid", wifi_ssid.c_str());
    }
    cJSON_AddItemToObject(root, "connectivity_metrics", connectivity);

    const std::string ble_state = CopyStringField(status_root, "ble_state", "off");
    const std::string ap_state = CopyStringField(status_root, "ap_state", "off");
    cJSON_AddStringToObject(root, "ble_state", ble_state.c_str());
    cJSON_AddStringToObject(root, "ap_state", ap_state.c_str());

    float temp = 0.0f;
    if (Board::GetInstance().GetTemperature(temp)) {
        cJSON_AddNumberToObject(root, "temp", temp);
    }

    char* raw = cJSON_PrintUnformatted(root);
    std::string body = raw == nullptr ? "{}" : raw;
    if (raw != nullptr) {
        cJSON_free(raw);
    }
    cJSON_Delete(root);
    if (status_root != nullptr) {
        cJSON_Delete(status_root);
    }
    return body;
}

void Application::StartHeartbeat() {
#if CONFIG_TBOT_COURSE_MODE_LOCAL_ENDPOINT
    return;
#else
    if (heartbeat_active_) {
        return;
    }
    if (open_channel_queue == nullptr || open_channel_task == nullptr) {
        ESP_LOGE(TAG, "Persistent network worker unavailable for heartbeat");
        return;
    }
    if (heartbeat_timer_ == nullptr) {
        esp_timer_create_args_t args = {
            .callback =
                [](void* arg) {
                    Application* app = static_cast<Application*>(arg);
                    app->Schedule([app]() { app->DispatchDeviceHeartbeat(); });
                },
            .arg = this,
            .dispatch_method = ESP_TIMER_TASK,
            .name = "heartbeat",
            .skip_unhandled_events = true,
        };
        if (esp_timer_create(&args, &heartbeat_timer_) != ESP_OK) {
            ESP_LOGE(TAG, "Failed to create heartbeat timer");
            heartbeat_timer_ = nullptr;
            return;
        }
    }
    heartbeat_active_ = true;
    esp_timer_start_periodic(heartbeat_timer_, kHeartbeatIntervalUs);
    ESP_LOGI(TAG, "Heartbeat started (every %lus)",
             static_cast<unsigned long>(kHeartbeatIntervalUs / 1000000ULL));
#endif
}

void Application::StopHeartbeat() {
    if (heartbeat_timer_ != nullptr && heartbeat_active_) {
        esp_timer_stop(heartbeat_timer_);
    }
    heartbeat_active_ = false;
}

void Application::HandleHeartbeatAuthFailure(int status_code) {
    if (lesson_runtime_active_.load()) {
        ESP_LOGW(TAG, "Heartbeat auth failed (HTTP %d) during lesson; deferring claim recovery",
                 status_code);
        StopHeartbeat();
        deferred_heartbeat_auth_failure_status_.store(status_code);
        return;
    }
    ESP_LOGW(TAG, "Heartbeat auth failed (HTTP %d); entering remote-unpair WiFi setup",
             status_code);
    StopHeartbeat();
    StopClaimPoll();
    CloseAudioChannelByIntent();

    auto display = Board::GetInstance().GetDisplay();
    if (display) {
        display->SetStatus(Lang::Strings::INITIALIZING);
        display->SetChatMessage("system", "");
    }

    {
        Settings backend_settings("backend", true);
        backend_settings.SetString("device_id", "");
        backend_settings.SetString("device_secret", "");
        backend_settings.SetInt("release_pending", 0);
    }
    {
        Settings claim_state("tbot_claim", true);
        claim_state.SetInt("confirmed", 0);
        claim_state.SetInt("factory_test", 0);
    }
    {
        Settings websocket_settings("websocket", true);
        websocket_settings.SetString("bootstrap_token", "");
        websocket_settings.SetString("token", "");
        websocket_settings.SetString("url", "");
        websocket_settings.SetInt("claim_ambiguous", 0);
        websocket_settings.EraseKey("claim_device_id");
    }

    pending_tbot_claim_ = PendingTbotClaim{};
    pending_tbot_claim_api_url_.clear();
    SecureClearString(pending_tbot_claim_token_);
    claim_confirmation_ambiguous_ = false;
    claim_substate_ = TbotClaimSubstate::AvailableStandby;
    backend_offline_.store(false);

    const auto wifi_clear_result = SsidManager::GetInstance().ForceClearAndCancelTransaction();
    if (wifi_clear_result != SsidMutationResult::kApplied) {
        ESP_LOGE(TAG, "Heartbeat auth recovery could not clear saved WiFi");
        return;
    }
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
}

void Application::EnterRepairPairingMode(ChatRequestContext context) {
    if (!IsChatRequestCurrent(context))
        return;
#if CONFIG_TBOT_COURSE_MODE_LOCAL_ENDPOINT
    ESP_LOGW(TAG, "Course-mode local endpoint blocks repair/reset networking");
    return;
#else
    Schedule([this, context]() {
        if (!IsChatRequestCurrent(context))
            return;
        if (lesson_runtime_active_.load()) {
            ESP_LOGW(TAG, "lesson re-pair ignored during lesson");
            return;
        }
        ESP_LOGW(TAG, "BOOT re-pair: forgetting current claim so a new parent phone can connect");
        StopHeartbeat();
        CloseAudioChannelByIntent();
        auto display = Board::GetInstance().GetDisplay();
        display->SetStatus(Lang::Strings::INITIALIZING);
        display->SetChatMessage("system", "");

        bool had_cloud_secret = false;
        {
            Settings backend_settings("backend", false);
            had_cloud_secret = !backend_settings.GetString("device_secret").empty() &&
                               !backend_settings.GetString("device_id").empty() &&
                               !backend_settings.GetString("api_url").empty();
        }
        bool released = !had_cloud_secret;
        if (had_cloud_secret) {
            released = SystemReset::ReleaseCloudOwnership();
            ESP_LOGW(TAG, "BOOT re-pair cloud ownership release: %s",
                     released ? "OK (backend freed for re-claim)"
                              : "FAILED (offline/auth?) -> deferred retry when online");
        } else {
            ESP_LOGW(TAG,
                     "BOOT re-pair: no cloud credentials to release (treating as already free)");
        }

        {
            Settings backend_settings("backend", true);
            if (released) {
                backend_settings.SetString("device_id", "");
                backend_settings.SetString("device_secret", "");
                backend_settings.SetInt("release_pending", 0);
            } else if (had_cloud_secret) {
                backend_settings.SetInt("release_pending", 1);
            }
        }
        {
            Settings claim_state("tbot_claim", true);
            claim_state.SetInt("confirmed", 0);
            claim_state.SetInt("factory_test", 0);
        }
        {
            Settings websocket_settings("websocket", true);
            websocket_settings.SetString("bootstrap_token", "");
            websocket_settings.SetString("token", "");
            websocket_settings.SetInt("claim_ambiguous", 0);
            websocket_settings.SetString("url", "");
            websocket_settings.EraseKey("claim_device_id");
        }

        pending_tbot_claim_ = PendingTbotClaim{};
        pending_tbot_claim_api_url_.clear();
        SecureClearString(pending_tbot_claim_token_);
        claim_confirmation_ambiguous_ = false;
        claim_substate_ = TbotClaimSubstate::AvailableStandby;
        backend_offline_.store(false);
        RenderClaimSubstate(claim_substate_);

        const auto wifi_clear_result = SsidManager::GetInstance().ForceClearAndCancelTransaction();
        if (wifi_clear_result != SsidMutationResult::kApplied) {
            ESP_LOGE(TAG, "BOOT re-pair could not clear saved WiFi");
            return;
        }
        ESP_LOGW(TAG,
                 "BOOT re-pair: Wi-Fi forgotten; rebooting into Wi-Fi setup for a new network");
        vTaskDelay(pdMS_TO_TICKS(1500));
        esp_restart();
    });
#endif
}

namespace {
struct CloudReleaseContext {
    Application* app;
    std::string api_url;
    std::string device_id;
    std::string device_secret;
};
}  // namespace

void Application::MaybeDispatchDeferredCloudRelease() {
#if CONFIG_TBOT_COURSE_MODE_LOCAL_ENDPOINT
    return;
#else
    Settings backend_settings("backend", false);
    if (backend_settings.GetInt("release_pending", 0) == 0) {
        return;
    }
    const std::string api_url = backend_settings.GetString("api_url");
    const std::string device_id = backend_settings.GetString("device_id");
    std::string device_secret = backend_settings.GetString("device_secret");
    SecureStringScope device_secret_scope(device_secret);
    if (api_url.empty() || device_id.empty() || device_secret.empty()) {
        Settings writable("backend", true);
        writable.SetInt("release_pending", 0);
        return;
    }

    const DeviceState state = GetDeviceState();
    if (state == kDeviceStateConnecting || state == kDeviceStateListening ||
        state == kDeviceStateSpeaking || connect_in_flight_.load()) {
        return;
    }
    bool expected = false;
    if (!cloud_release_inflight_.compare_exchange_strong(expected, true)) {
        return;
    }

    auto* ctx = new (std::nothrow) CloudReleaseContext{this, api_url, device_id, device_secret};
    if (ctx == nullptr) {
        ESP_LOGE(TAG, "cloud_release context allocation failed; retrying next refresh");
        cloud_release_inflight_.store(false);
        return;
    }
    if (xTaskCreateWithCaps(&Application::CloudReleaseTask, "cloud_release", 6144, ctx,
                            tskIDLE_PRIORITY + 1, nullptr,
                            MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) != pdPASS) {
        SecureClearString(ctx->device_secret);
        delete ctx;
        ESP_LOGE(TAG, "cloud_release task create failed; retrying next refresh");
        cloud_release_inflight_.store(false);
    }
#endif
}

void Application::CloudReleaseTask(void* arg) {
    {
        auto* ctx = static_cast<CloudReleaseContext*>(arg);
        Application* self = ctx->app;

        const bool released =
            SystemReset::ReleaseCloudOwnership(ctx->api_url, ctx->device_id, ctx->device_secret);
        std::string api_url = std::move(ctx->api_url);
        std::string device_id = std::move(ctx->device_id);
        std::string device_secret = std::move(ctx->device_secret);
        delete ctx;

        self->Schedule([self, released, api_url = std::move(api_url),
                        device_id = std::move(device_id),
                        device_secret = std::move(device_secret)]() mutable {
            self->cloud_release_inflight_.store(false);
            Settings current_settings("backend", false);
            const bool credentials_unchanged =
                current_settings.GetString("api_url") == api_url &&
                current_settings.GetString("device_id") == device_id &&
                current_settings.GetString("device_secret") == device_secret;
            if (!credentials_unchanged) {
                Settings backend_settings("backend", true);
                backend_settings.SetInt("release_pending", 0);
                ESP_LOGI(TAG, "Deferred cloud release completed against superseded credentials");
                std::fill(device_secret.begin(), device_secret.end(), '\0');
                return;
            }
            if (!released) {
                ESP_LOGW(TAG,
                         "Deferred cloud ownership release failed; will retry on next refresh");
                std::fill(device_secret.begin(), device_secret.end(), '\0');
                return;
            }
            Settings backend_settings("backend", true);
            backend_settings.SetInt("release_pending", 0);
            backend_settings.SetString("device_id", "");
            backend_settings.SetString("device_secret", "");
            std::fill(device_secret.begin(), device_secret.end(), '\0');
            ESP_LOGI(TAG,
                     "Deferred cloud ownership released; robot is free for a new parent to claim");
            self->RefreshPendingTbotClaim();
        });
    }
    vTaskDeleteWithCaps(nullptr);
}

namespace {
struct HeartbeatContext {
    Application* app;
    std::string url;
    std::string device_secret;
    std::string body;
};
}  // namespace

