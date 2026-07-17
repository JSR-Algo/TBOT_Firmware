#include "provisioning_status_reporter.h"

#include <cJSON.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "board.h"
#include "system_info.h"
#include "settings.h"

static const char* TAG = "ProvisioningReporter";

// Retry delays in milliseconds: 2 s, 4 s, 8 s
static const int kRetryDelaysMs[] = {2000, 4000, 8000};
static constexpr int kMaxAttempts = 3;

bool ProvisioningStatusReporter::Report(Status status,
                                        const std::string& token,
                                        const std::string& code,
                                        const std::string& reason) {
#ifndef CONFIG_TBOT_PROVISIONING_REPORT_ENABLED
    ESP_LOGW(TAG, "Provisioning reporting disabled at build time");
    return true;
#endif

    if (token.empty()) {
        ESP_LOGE(TAG, "Bootstrap token is empty, cannot report provisioning status");
        return false;
    }

    // Build JSON body
    cJSON* root = cJSON_CreateObject();
    const std::string device_id = Board::GetInstance().GetUuid();
    cJSON_AddStringToObject(root, "device_id", device_id.c_str());

    if (status == Status::DeviceAuthenticated) {
        cJSON_AddStringToObject(root, "status", "device_authenticated");
        cJSON_AddStringToObject(root, "code", code.c_str());
    } else {
        cJSON_AddStringToObject(root, "status", "failed");
        const std::string r = reason.empty() ? "wifi_connect_failed" : reason;
        cJSON_AddStringToObject(root, "reason", r.c_str());
    }

    char* raw = cJSON_PrintUnformatted(root);
    // cJSON_PrintUnformatted returns nullptr on allocation failure. Guard
    // before constructing std::string (strlen(nullptr) would crash), mirroring
    // BuildTbotClaimConfirmBody in claim_confirmation_reporter.cc. An empty body
    // would be a malformed request, so bail out cleanly instead.
    if (raw == nullptr) {
        ESP_LOGE(TAG, "Failed to serialize provisioning report body");
        cJSON_Delete(root);
        return false;
    }
    std::string body(raw);
    cJSON_free(raw);
    cJSON_Delete(root);

    // NVS-first override, mirroring Ota::GetCheckVersionUrl(): an operator can
    // seed a stable provisioning endpoint into NVS (wifi namespace, key
    // "provisioning_url") so a device survives a backend/tunnel host change
    // without a reflash. When NVS is empty we fall back to the compile-time
    // default, preserving existing behavior.
    Settings provisioning_settings("wifi", false);
    std::string url = provisioning_settings.GetString("provisioning_url");
    if (url.empty()) {
        url = CONFIG_PROVISIONING_STATUS_URL;
    }
    const std::string user_agent = SystemInfo::GetUserAgent();
    const std::string mac = SystemInfo::GetMacAddress();

    auto& board = Board::GetInstance();
    auto* network = board.GetNetwork();

    for (int attempt = 0; attempt < kMaxAttempts; ++attempt) {
        if (attempt > 0) {
            int delay_ms = kRetryDelaysMs[attempt - 1];
            ESP_LOGW(TAG, "Retrying provisioning report in %d ms (attempt %d/%d)",
                     delay_ms, attempt + 1, kMaxAttempts);
            vTaskDelay(pdMS_TO_TICKS(delay_ms));
        }

        auto http = network->CreateHttp(2);
        if (!http) {
            ESP_LOGE(TAG, "Failed to create HTTP client");
            continue;
        }

        http->SetHeader("Authorization", "Bearer " + token);
        http->SetHeader("Content-Type", "application/json");
        http->SetHeader("Device-Id", mac);
        http->SetHeader("User-Agent", user_agent);

        // SetContent takes std::string&&; copy body so we can retry
        std::string body_copy = body;
        http->SetContent(std::move(body_copy));

        // Never log the bootstrap token or the request body: the
        // device_authenticated body carries the provisioning code, and the token
        // is a credential. Log only non-secret routing fields plus the body
        // length for diagnostics.
        ESP_LOGI(TAG, "Reporting attempt=%d status=%d endpoint_configured=%d body_len=%u",
                 attempt + 1, static_cast<int>(status), static_cast<int>(!url.empty()),
                 static_cast<unsigned>(body.size()));

        if (!http->Open("POST", url)) {
            int err = http->GetLastError();
            ESP_LOGE(TAG, "HTTP open failed, error=0x%x", err);
            http->Close();
            continue;
        }

        int status_code = http->GetStatusCode();
        std::string resp_body;
        if (status_code < 200 || status_code >= 300) {
            resp_body = http->ReadAll();
        }
        http->Close();

        ESP_LOGI(TAG, "Report response attempt=%d status_code=%d", attempt + 1, status_code);

        if (status_code >= 200 && status_code < 300) {
            ESP_LOGI(TAG, "Provisioning status reported successfully (HTTP %d)", status_code);
            return true;
        }

        // Redact the response body to a length only: a misbehaving backend could
        // reflect the submitted request body (which carries the provisioning
        // code) in an error response, so never log it verbatim.
        ESP_LOGW(TAG, "Provisioning status report failed (HTTP %d) resp_len=%u, will retry",
                 status_code, static_cast<unsigned>(resp_body.size()));
    }

    ESP_LOGE(TAG, "All %d provisioning report attempts failed", kMaxAttempts);
    return false;
}
