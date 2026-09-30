#include "application_internal.h"

void Application::DispatchDeviceHeartbeat() {
#if CONFIG_TBOT_COURSE_MODE_LOCAL_ENDPOINT
    return;
#else
    const DeviceState device_state = GetDeviceState();
    if (device_state != kDeviceStateIdle && device_state != kDeviceStateListening &&
        device_state != kDeviceStateSpeaking) {
        return;
    }

    bool expected = false;
    if (!heartbeat_inflight_.compare_exchange_strong(expected, true)) {
        ESP_LOGD(TAG, "Heartbeat already in flight; skipping this tick");
        return;
    }

    Settings backend_settings("backend", false);
    const std::string api_url = backend_settings.GetString("api_url");
    if (api_url.empty()) {
        heartbeat_inflight_.store(false);
        return;
    }
    const std::string device_secret = backend_settings.GetString("device_secret");
    if (device_secret.empty()) {
        heartbeat_inflight_.store(false);
        return;
    }
    const std::string backend_device_id = backend_settings.GetString("device_id");
    if (backend_device_id.empty()) {
        ESP_LOGW(TAG, "Heartbeat skipped: missing backend device id");
        heartbeat_inflight_.store(false);
        return;
    }

    std::string base = api_url;
    while (!base.empty() && base.back() == '/') {
        base.pop_back();
    }
    if (base.find("/v1") == std::string::npos) {
        base += "/v1";
    }
    const std::string status_json = Board::GetInstance().GetDeviceStatusJson();

    auto* ctx = new HeartbeatContext{this, base + "/device/heartbeat", device_secret,
                                     BuildTbotHeartbeatBody(status_json, backend_device_id)};
    const NetworkWorkItem work{NetworkWorkKind::kHeartbeat, ctx};
    if (open_channel_queue == nullptr || xQueueSend(open_channel_queue, &work, 0) != pdTRUE) {
        ESP_LOGE(TAG, "heartbeat worker queue unavailable; retrying next tick");
        delete ctx;
        heartbeat_inflight_.store(false);
    } else {
        ESP_LOGI(TAG, "Heartbeat queued");
    }
#endif
}

void Application::HeartbeatTask(void* arg) {
    auto* ctx = static_cast<HeartbeatContext*>(arg);
    if (ctx == nullptr)
        return;
    auto* self = ctx->app;
    ESP_LOGI(TAG, "Heartbeat worker received request");
    const std::string url = ctx->url;
    const std::string device_secret = ctx->device_secret;
    std::string body = std::move(ctx->body);
    delete ctx;

    const int status_code = self->SendDeviceHeartbeat(url, device_secret, std::move(body));
    self->Schedule([self, status_code]() {
        self->heartbeat_inflight_.store(false);
        if (status_code == 401 || status_code == 403) {
            self->HandleHeartbeatAuthFailure(status_code);
        }
    });
}

int Application::SendDeviceHeartbeat(const std::string& url, const std::string& device_secret,
                                     std::string body) {
    auto* network = Board::GetInstance().GetNetwork();
    auto http = network->CreateHttp(2);
    if (!http) {
        ESP_LOGE(TAG, "Failed to create HTTP client for heartbeat");
        return 0;
    }
    http->SetTimeout(5000);
    http->SetHeader("X-Device-Token", device_secret);
    http->SetHeader("Content-Type", "application/json");
    http->SetHeader("Device-Id", SystemInfo::GetMacAddress());
    http->SetHeader("User-Agent", SystemInfo::GetUserAgent());
    http->SetContent(std::move(body));

    if (!http->Open("POST", url)) {
        ESP_LOGW(TAG, "Heartbeat HTTP open failed: 0x%x", http->GetLastError());
        http->Close();
        return 0;
    }
    const int status_code = http->GetStatusCode();
    http->Close();

    if (status_code < 200 || status_code >= 300) {
        ESP_LOGW(TAG, "Heartbeat failed (HTTP %d)", status_code);
        return status_code;
    }
    ESP_LOGI(TAG, "Heartbeat accepted (HTTP %d)", status_code);
    return status_code;
}
