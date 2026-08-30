#include "claim_confirmation_reporter.h"

#include <cJSON.h>
#include <esp_log.h>

#include <cstdio>
#include <ctime>

#include "board.h"
#include "settings.h"
#include "system_info.h"

static const char* TAG = "ClaimConfirm";

bool ParsePendingTbotClaimFromDeviceConfigJson(const std::string& json,
                                               PendingTbotClaim& pending_claim) {
    pending_claim = PendingTbotClaim{};

    cJSON* root = cJSON_Parse(json.c_str());
    if (root == nullptr) {
        return false;
    }

    cJSON* claim = cJSON_GetObjectItem(root, "claim");
    if (claim == nullptr || cJSON_IsNull(claim)) {
        cJSON_Delete(root);
        return true;
    }
    if (!cJSON_IsObject(claim)) {
        cJSON_Delete(root);
        return false;
    }

    cJSON* claim_id = cJSON_GetObjectItem(claim, "claim_id");
    cJSON* status = cJSON_GetObjectItem(claim, "status");
    cJSON* message = cJSON_GetObjectItem(claim, "message");
    cJSON* expires_at = cJSON_GetObjectItem(claim, "expires_at");

    if (!cJSON_IsString(claim_id) || !cJSON_IsString(status) ||
        std::string(status->valuestring) != "WAITING_PHYSICAL_CONFIRM") {
        // M2: a non-empty status we did not recognize is almost certainly a
        // backend casing/value drift (e.g. "waiting_physical_confirm"). Surface
        // it so the dead-feature state is observable instead of silently false.
        if (cJSON_IsString(status) && status->valuestring[0] != '\0') {
            ESP_LOGW(TAG, "Unrecognized claim status '%s' (expected WAITING_PHYSICAL_CONFIRM)",
                     status->valuestring);
        }
        cJSON_Delete(root);
        return false;
    }

    pending_claim.active = true;
    pending_claim.claim_id = claim_id->valuestring;
    pending_claim.status = status->valuestring;
    if (cJSON_IsString(message)) {
        pending_claim.message = message->valuestring;
    }
    if (cJSON_IsString(expires_at)) {
        pending_claim.expires_at = expires_at->valuestring;
    }

    cJSON_Delete(root);
    return true;
}

// Days since 1970-01-01 for a proleptic-Gregorian (y, m, d), Howard Hinnant's
// branchless civil algorithm. Self-contained so we depend on neither the TZ env
// nor the newlib timegm() extension — pure UTC arithmetic, correct by inspection.
static long DaysFromCivilUtc(int year, unsigned month, unsigned day) {
    year -= month <= 2;
    const long era = (year >= 0 ? year : year - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(year - era * 400);              // [0, 399]
    const unsigned doy = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;  // [0, 365]
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;                // [0, 146096]
    return era * 146097L + static_cast<long>(doe) - 719468L;
}

bool ParseIso8601UtcToEpoch(const std::string& iso_utc, time_t& out_epoch_seconds) {
    if (iso_utc.empty()) {
        return false;
    }

    int year = 0, month = 0, day = 0, hour = 0, minute = 0, second = 0;
    // Accept "YYYY-MM-DDTHH:MM:SS" with a 'T' or ' ' separator. Trailing
    // fractional seconds and the 'Z' suffix are ignored (UTC assumed).
    if (std::sscanf(iso_utc.c_str(), "%4d-%2d-%2dT%2d:%2d:%2d",
                    &year, &month, &day, &hour, &minute, &second) != 6 &&
        std::sscanf(iso_utc.c_str(), "%4d-%2d-%2d %2d:%2d:%2d",
                    &year, &month, &day, &hour, &minute, &second) != 6) {
        return false;
    }
    if (month < 1 || month > 12 || day < 1 || day > 31 ||
        hour < 0 || hour > 23 || minute < 0 || minute > 59 ||
        second < 0 || second > 60) {  // 60 allows a leap second
        return false;
    }

    const long days = DaysFromCivilUtc(year, static_cast<unsigned>(month),
                                       static_cast<unsigned>(day));
    out_epoch_seconds = static_cast<time_t>(days * 86400L +
                                            hour * 3600L + minute * 60L + second);
    return true;
}

bool IsPendingTbotClaimExpired(const PendingTbotClaim& claim, time_t now_epoch_seconds) {
    time_t expires_epoch = 0;
    if (!ParseIso8601UtcToEpoch(claim.expires_at, expires_epoch)) {
        // No parseable expiry -> never expire here; the bounded poll's window
        // cap is the backstop so we never strand on a stale claim.
        return false;
    }
    return now_epoch_seconds >= expires_epoch;
}

std::string BuildTbotClaimConfirmUrl(const std::string& api_base_url) {
    if (api_base_url.empty()) {
        return "";
    }

    std::string base = api_base_url;
    while (!base.empty() && base.back() == '/') {
        base.pop_back();
    }
    // Ensure the /v1 API prefix (see BuildTbotDeviceConfigUrl).
    if (base.find("/v1") == std::string::npos) {
        base += "/v1";
    }
    return base + "/claim/confirm";
}

std::string BuildTbotClaimConfirmBody(const std::string& claim_id,
                                      const std::string& device_id,
                                      const std::string& confirm_method) {
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "claim_id", claim_id.c_str());
    cJSON_AddStringToObject(root, "device_id", device_id.c_str());
    cJSON_AddStringToObject(root, "confirm_method", confirm_method.c_str());

    char* raw = cJSON_PrintUnformatted(root);
    std::string body(raw == nullptr ? "" : raw);
    if (raw != nullptr) {
        cJSON_free(raw);
    }
    cJSON_Delete(root);
    return body;
}

std::string UrlEncodeQueryParam(const std::string& value) {
    static const char* hex = "0123456789ABCDEF";
    std::string encoded;
    encoded.reserve(value.size());

    for (unsigned char c : value) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
            encoded.push_back(static_cast<char>(c));
            continue;
        }

        encoded.push_back('%');
        encoded.push_back(hex[(c >> 4) & 0x0f]);
        encoded.push_back(hex[c & 0x0f]);
    }

    return encoded;
}

std::string BuildTbotDeviceConfigUrl(const std::string& api_base_url,
                                     const std::string& device_id) {
    if (api_base_url.empty() || device_id.empty()) {
        return "";
    }

    std::string base = api_base_url;
    while (!base.empty() && base.back() == '/') {
        base.pop_back();
    }
    // Ensure the /v1 API prefix: the deployed OTA/bootstrap may hand a base
    // without it, but device/config + claim routes live under /v1.
    if (base.find("/v1") == std::string::npos) {
        base += "/v1";
    }
    return base + "/device/config?device_id=" + UrlEncodeQueryParam(device_id);
}

std::string BuildTbotConfigFetchUrl(const std::string& api_base_url) {
    if (api_base_url.empty()) {
        return "";
    }

    std::string base = api_base_url;
    while (!base.empty() && base.back() == '/') {
        base.pop_back();
    }
    if (base.find("/v1") == std::string::npos) {
        base += "/v1";
    }
    return base + "/config/fetch";
}

// The device_id the claim flow must use. The phone resolves the backend's
// serial-keyed device record and pushes that id to us over BLE custom-data
// (TLV tag 0x03 -> NVS websocket.claim_device_id). We MUST claim/confirm under
// that same id, otherwise the robot answers /device/config + /claim/confirm
// with its random Board UUID (board.cc GenerateUuid) while the phone created
// the claim attempt under the backend id -> the two never match and pairing
// hangs at "waiting for robot to authenticate". Fall back to the Board UUID
// only when the phone did not provide one (older app), preserving legacy
// behavior.
static std::string GetTbotClaimDeviceId() {
    Settings websocket_settings("websocket", false);
    const std::string pushed = websocket_settings.GetString("claim_device_id");
    if (!pushed.empty()) {
        return pushed;
    }
    return Board::GetInstance().GetUuid();
}

bool FetchPendingTbotClaimFromDeviceConfig(const std::string& api_base_url,
                                           const std::string& bootstrap_token,
                                           PendingTbotClaim& pending_claim,
                                           int* http_status_code) {
    pending_claim = PendingTbotClaim{};
    if (http_status_code) {
        *http_status_code = 0;
    }

    const std::string device_id = GetTbotClaimDeviceId();
    const std::string url = BuildTbotDeviceConfigUrl(api_base_url, device_id);
    if (url.empty()) {
        ESP_LOGW(TAG, "Cannot fetch device config: missing API URL or device id");
        return false;
    }

    auto* network = Board::GetInstance().GetNetwork();
    auto http = network->CreateHttp(2);
    if (!http) {
        ESP_LOGE(TAG, "Failed to create HTTP client for device config");
        return false;
    }

    // B1: cap the blocking Open() so a slow/unreachable backend cannot freeze the
    // caller (the claim poll runs on the priority-10 main Application task).
    http->SetTimeout(5000);

    if (!bootstrap_token.empty()) {
        http->SetHeader("Authorization", "Bearer " + bootstrap_token);
    }
    http->SetHeader("Device-Id", SystemInfo::GetMacAddress());
    http->SetHeader("User-Agent", SystemInfo::GetUserAgent());

    ESP_LOGI(TAG, "Fetching device config");
    if (!http->Open("GET", url)) {
        ESP_LOGE(TAG, "Device config HTTP open failed: 0x%x", http->GetLastError());
        http->Close();
        return false;
    }

    const int status_code = http->GetStatusCode();
    if (http_status_code) {
        *http_status_code = status_code;
    }
    const std::string response_body = http->ReadAll();
    http->Close();

    if (status_code < 200 || status_code >= 300) {
        // Never log the verbatim response body: a non-2xx/proxied backend can
        // reflect the /claim or /device/config JSON (which carries device_secret),
        // so log only the HTTP status + response length.
        ESP_LOGW(TAG, "Device config fetch failed (HTTP %d) resp_len=%u",
                 status_code, static_cast<unsigned>(response_body.size()));
        return false;
    }

    const bool parsed = ParsePendingTbotClaimFromDeviceConfigJson(response_body, pending_claim);
    ESP_LOGI(TAG, "Device config fetch result http=%d parsed=%d active=%d token=%d claim_present=%d",
             status_code, static_cast<int>(parsed), static_cast<int>(pending_claim.active),
             static_cast<int>(!bootstrap_token.empty()), static_cast<int>(!pending_claim.claim_id.empty()));
    return parsed;
}

bool FetchPendingTbotClaimFromDeviceConfig(const std::string& api_base_url,
                                           const std::string& bootstrap_token,
                                           PendingTbotClaim& pending_claim) {
    return FetchPendingTbotClaimFromDeviceConfig(api_base_url, bootstrap_token,
                                                 pending_claim, nullptr);
}

std::string BuildTbotDeviceBootstrapUrl() {
    // Derive {origin}/v1/device/bootstrap from the provisioning-status URL
    // {origin}/v1/device/provisioning/status (same backend surface). Pure string
    // surgery so we add no new Kconfig and never diverge from the host. Resolve
    // NVS-first (wifi/provisioning_url), mirroring provisioning_status_reporter, so
    // a backend/tunnel host change carries through here too without a reflash.
    Settings provisioning_settings("wifi", false);
    std::string status_url = provisioning_settings.GetString("provisioning_url");
    if (status_url.empty()) {
        status_url = CONFIG_PROVISIONING_STATUS_URL;
    }
    static const char* kSuffix = "provisioning/status";
    const std::size_t pos = status_url.rfind(kSuffix);
    if (pos == std::string::npos) {
        return "";
    }
    return status_url.substr(0, pos) + "bootstrap";
}

std::string FetchBackendApiUrlFromBootstrap(const std::string& bootstrap_token,
                                            bool persist_result) {
    const std::string url = BuildTbotDeviceBootstrapUrl();
    if (url.empty()) {
        ESP_LOGW(TAG, "Cannot derive bootstrap URL from provisioning-status URL");
        return "";
    }

    auto* network = Board::GetInstance().GetNetwork();
    auto http = network->CreateHttp(2);
    if (!http) {
        ESP_LOGE(TAG, "Failed to create HTTP client for bootstrap api_url fallback");
        return "";
    }

    // B1: cap the blocking Open() (this runs on the main Application task).
    http->SetTimeout(5000);
    if (!bootstrap_token.empty()) {
        http->SetHeader("Authorization", "Bearer " + bootstrap_token);
    }
    http->SetHeader("Device-Id", SystemInfo::GetMacAddress());
    http->SetHeader("User-Agent", SystemInfo::GetUserAgent());

    ESP_LOGI(TAG, "Fetching backend api_url fallback");
    if (!http->Open("GET", url)) {
        ESP_LOGW(TAG, "Bootstrap api_url fetch HTTP open failed: 0x%x", http->GetLastError());
        http->Close();
        return "";
    }

    const int status_code = http->GetStatusCode();
    const std::string response_body = http->ReadAll();
    http->Close();

    if (status_code < 200 || status_code >= 300) {
        ESP_LOGW(TAG, "Bootstrap api_url fetch failed (HTTP %d)", status_code);
        return "";
    }

    cJSON* root = cJSON_Parse(response_body.c_str());
    if (root == nullptr) {
        ESP_LOGW(TAG, "Bootstrap response is not valid JSON");
        return "";
    }
    cJSON* api_url = cJSON_GetObjectItem(root, "api_url");
    std::string result;
    if (cJSON_IsString(api_url) && api_url->valuestring[0] != '\0') {
        result = api_url->valuestring;
    }
    cJSON_Delete(root);

    if (result.empty()) {
        ESP_LOGW(TAG, "Bootstrap response did not carry a non-empty api_url");
        return "";
    }

    // Persist so the next RefreshPendingTbotClaim / heartbeat reads it straight
    // from Settings("backend"), exactly like the OTA-provided value.
    if (persist_result) {
        Settings settings("backend", true);
        if (settings.GetString("api_url") != result) {
            settings.SetString("api_url", result);
        }
    }
    ESP_LOGI(TAG, "Backend api_url recovered from bootstrap fallback");
    return result;
}

bool RefreshWebsocketUrlFromConfigFetch() {
    Settings backend_settings("backend", false);
    const std::string api_url = backend_settings.GetString("api_url");
    const std::string device_id = backend_settings.GetString("device_id");
    const std::string device_secret = backend_settings.GetString("device_secret");
    if (api_url.empty() || device_id.empty() || device_secret.empty()) {
        ESP_LOGI(TAG, "Skipping config-fetch websocket refresh: claimed backend credentials incomplete");
        return false;
    }

    const std::string url = BuildTbotConfigFetchUrl(api_url);
    if (url.empty()) {
        ESP_LOGW(TAG, "Cannot fetch runtime config: backend api_url is empty");
        return false;
    }

    auto* network = Board::GetInstance().GetNetwork();
    auto http = network->CreateHttp(2);
    if (!http) {
        ESP_LOGE(TAG, "Failed to create HTTP client for config fetch");
        return false;
    }

    http->SetTimeout(5000);
    http->SetHeader("X-Device-Id", device_id);
    http->SetHeader("X-Device-Token", device_secret);
    http->SetHeader("User-Agent", SystemInfo::GetUserAgent());

    ESP_LOGI(TAG, "Fetching runtime config");
    if (!http->Open("GET", url)) {
        ESP_LOGW(TAG, "Config fetch HTTP open failed: 0x%x", http->GetLastError());
        http->Close();
        return false;
    }

    const int status_code = http->GetStatusCode();
    const std::string response_body = http->ReadAll();
    http->Close();

    if (status_code < 200 || status_code >= 300) {
        ESP_LOGW(TAG, "Config fetch failed (HTTP %d) resp_len=%u",
                 status_code, static_cast<unsigned>(response_body.size()));
        return false;
    }

    cJSON* root = cJSON_Parse(response_body.c_str());
    if (root == nullptr) {
        ESP_LOGW(TAG, "Config fetch response is not valid JSON");
        return false;
    }

    cJSON* config_blob = cJSON_GetObjectItem(root, "configBlob");
    cJSON* websocket = cJSON_IsObject(config_blob) ? cJSON_GetObjectItem(config_blob, "websocket") : nullptr;
    cJSON* ws_url = cJSON_IsObject(websocket) ? cJSON_GetObjectItem(websocket, "url") : nullptr;
    if (!cJSON_IsString(ws_url) || ws_url->valuestring[0] == '\0') {
        cJSON_Delete(root);
        ESP_LOGW(TAG, "Config fetch response did not carry configBlob.websocket.url");
        return false;
    }

    Settings websocket_settings("websocket", true);
    if (websocket_settings.GetString("url") != ws_url->valuestring) {
        websocket_settings.SetString("url", ws_url->valuestring);
    }
    cJSON_Delete(root);
    ESP_LOGI(TAG, "Websocket URL refreshed from config fetch");
    return true;
}

static bool ProcessTbotClaimConfirmationResponse(const std::string& json, bool persist,
                                                 bool require_api_url = false) {
    cJSON* root = cJSON_Parse(json.c_str());
    if (root == nullptr) {
        return false;
    }

    cJSON* device_id = cJSON_GetObjectItem(root, "device_id");
    cJSON* device_secret = cJSON_GetObjectItem(root, "device_secret");
    cJSON* ws_url = cJSON_GetObjectItem(root, "ws_url");
    cJSON* api_url = cJSON_GetObjectItem(root, "api_url");
    const auto is_nonempty_string = [](const cJSON* item) {
        return cJSON_IsString(item) && item->valuestring != nullptr &&
               item->valuestring[0] != '\0';
    };
    if (!is_nonempty_string(device_id) || !is_nonempty_string(device_secret) ||
        !is_nonempty_string(ws_url) || (require_api_url && !is_nonempty_string(api_url))) {
        cJSON_Delete(root);
        return false;
    }

    if (persist) {
        Settings websocket_settings("websocket", true);
        websocket_settings.SetString("url", ws_url->valuestring);

        Settings backend_settings("backend", true);
        backend_settings.SetString("device_id", device_id->valuestring);
        backend_settings.SetString("device_secret", device_secret->valuestring);
        if (is_nonempty_string(api_url)) {
            backend_settings.SetString("api_url", api_url->valuestring);
        }

        // Mark the device claimed with a DEDICATED flag. websocket "token" alone is
        // NOT a reliable claimed-signal: the OTA CheckVersion response also writes
        // websocket.token (a realtime-WS auth token) on every boot.
        Settings claim_state("tbot_claim", true);
        claim_state.SetInt("confirmed", 1);
    }

    cJSON_Delete(root);
    return true;
}

bool PersistTbotClaimConfirmationResponse(const std::string& json) {
    return ProcessTbotClaimConfirmationResponse(json, true);
}

bool PersistProvisioningCredentialResponse(const std::string& json) {
    return ProcessTbotClaimConfirmationResponse(json, true, true);
}

static std::string ExtractTbotClaimErrorSummary(const std::string& response_body) {
    cJSON* root = cJSON_Parse(response_body.c_str());
    if (root == nullptr) {
        return "";
    }

    if (cJSON_GetObjectItem(root, "device_secret") != nullptr) {
        cJSON_Delete(root);
        return " code=REDACTED_SECRET_BODY";
    }

    std::string summary;
    cJSON* code = cJSON_GetObjectItem(root, "code");
    if (cJSON_IsString(code) && code->valuestring != nullptr && code->valuestring[0] != '\0') {
        summary += " code=";
        summary += code->valuestring;
    }

    cJSON* message = cJSON_GetObjectItem(root, "message");
    if (cJSON_IsString(message) && message->valuestring != nullptr && message->valuestring[0] != '\0') {
        std::string safe_message = message->valuestring;
        for (char& ch : safe_message) {
            if (ch == '\r' || ch == '\n') {
                ch = ' ';
            }
        }
        if (safe_message.size() > 160) {
            safe_message.resize(160);
        }
        summary += " message=";
        summary += safe_message;
    }

    cJSON_Delete(root);
    return summary;
}

ClaimConfirmationResult ClaimConfirmationReporter::Confirm(
    const PendingTbotClaim& claim,
    const std::string& api_base_url,
    const std::string& bootstrap_token,
    std::string* deferred_success_response) {
    if (!claim.active || claim.claim_id.empty() || bootstrap_token.empty()) {
        ESP_LOGW(TAG, "Cannot confirm claim: active=%d claim_empty=%d token_empty=%d",
                 static_cast<int>(claim.active), static_cast<int>(claim.claim_id.empty()),
                 static_cast<int>(bootstrap_token.empty()));
        return ClaimConfirmationResult::TerminalFailure;
    }

    const std::string url = BuildTbotClaimConfirmUrl(api_base_url);
    if (url.empty()) {
        ESP_LOGE(TAG, "Cannot confirm claim: API base URL is empty");
        return ClaimConfirmationResult::RetryableFailure;
    }

    const std::string device_id = GetTbotClaimDeviceId();
    std::string body = BuildTbotClaimConfirmBody(claim.claim_id, device_id, "button_press");

    auto* network = Board::GetInstance().GetNetwork();
    auto http = network->CreateHttp(2);
    if (!http) {
        ESP_LOGE(TAG, "Failed to create HTTP client for claim confirmation");
        return ClaimConfirmationResult::RetryableFailure;
    }

    // B1: cap the blocking Open() so confirmation cannot freeze the main task.
    http->SetTimeout(5000);

    http->SetHeader("Authorization", "Bearer " + bootstrap_token);
    http->SetHeader("Content-Type", "application/json");
    http->SetHeader("User-Agent", SystemInfo::GetUserAgent());
    http->SetContent(std::move(body));

    ESP_LOGI(TAG, "Confirming claim");
    if (!http->Open("POST", url)) {
        ESP_LOGE(TAG, "Claim confirmation HTTP open failed: 0x%x", http->GetLastError());
        http->Close();
        return ClaimConfirmationResult::RetryableFailure;
    }

    const int status_code = http->GetStatusCode();
    const std::string response_body = http->ReadAll();
    http->Close();

    if (status_code >= 200 && status_code < 300) {
        const bool valid_response = ProcessTbotClaimConfirmationResponse(
            response_body, deferred_success_response == nullptr);
        if (!valid_response) {
            ESP_LOGE(TAG, "Claim returned 2xx but response credentials were invalid");
            return ClaimConfirmationResult::AmbiguousSuccess;
        }
        if (deferred_success_response != nullptr) {
            *deferred_success_response = response_body;
        }
        ESP_LOGI(TAG, "Claim confirmed successfully (HTTP %d)", status_code);
        return ClaimConfirmationResult::Confirmed;
    }

    // Never log the verbatim response body: the /claim/confirm success response
    // carries device_secret. For non-2xx diagnostics, expose only redacted
    // backend code/message fields so field debugging can identify the branch.
    const std::string error_summary = ExtractTbotClaimErrorSummary(response_body);
    ESP_LOGW(TAG, "Claim confirmation failed (HTTP %d) resp_len=%u%s",
             status_code, static_cast<unsigned>(response_body.size()), error_summary.c_str());

    if (status_code == 408 || status_code == 425 || status_code == 429 ||
        status_code >= 500 || status_code < 400) {
        return ClaimConfirmationResult::RetryableFailure;
    }
    return ClaimConfirmationResult::TerminalFailure;
}
