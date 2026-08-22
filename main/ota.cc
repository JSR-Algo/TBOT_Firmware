#include "ota.h"
#include "system_info.h"
#include "settings.h"
#include "assets/lang_config.h"
#include "firmware_version_policy.h"
#include "course_mode_local_endpoint_policy.h"

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <cJSON.h>
#include <esp_log.h>
#include <esp_partition.h>
#include <esp_ota_ops.h>
#include <esp_app_format.h>
#include <esp_system.h>
#include <esp_efuse.h>
#include <esp_efuse_table.h>
#include <esp_heap_caps.h>
#include <esp_timer.h>
#ifdef SOC_HMAC_SUPPORTED
#include <esp_hmac.h>
#endif

#include <cstring>
#include <cstdio>
#include <cctype>
#include <vector>
#include <algorithm>

#define TAG "Ota"

namespace {

class ScopedHttpClose {
public:
    explicit ScopedHttpClose(Http& http) : http_(http) {}
    ~ScopedHttpClose() { Close(); }

    void Close() {
        if (!closed_) {
            http_.Close();
            closed_ = true;
        }
    }

    ScopedHttpClose(const ScopedHttpClose&) = delete;
    ScopedHttpClose& operator=(const ScopedHttpClose&) = delete;

private:
    Http& http_;
    bool closed_ = false;
};

bool IsEphemeralEndpoint(const std::string& url) {
    return url.find(".trycloudflare.com/") != std::string::npos;
}

std::string ExtractUrlHost(const std::string& url) {
    const size_t scheme_end = url.find("://");
    if (scheme_end == std::string::npos) {
        return {};
    }
    const size_t host_start = scheme_end + 3;
    const size_t host_end = url.find_first_of(":/", host_start);
    std::string host = url.substr(host_start, host_end - host_start);
    std::transform(host.begin(), host.end(), host.begin(),
                   [](unsigned char value) { return std::tolower(value); });
    return host;
}

bool IsPrivateOrLocalHost(const std::string& host) {
    if (host == "localhost" || host.ends_with(".local")) {
        return true;
    }

    int first_octet = -1;
    int second_octet = -1;
    int third_octet = -1;
    int fourth_octet = -1;
    char trailing = '\0';
    if (std::sscanf(host.c_str(), "%d.%d.%d.%d%c", &first_octet, &second_octet,
                    &third_octet, &fourth_octet, &trailing) != 4) {
        return false;
    }
    const bool valid_ipv4 =
        first_octet >= 0 && first_octet <= 255 &&
        second_octet >= 0 && second_octet <= 255 &&
        third_octet >= 0 && third_octet <= 255 &&
        fourth_octet >= 0 && fourth_octet <= 255;
    if (!valid_ipv4) {
        return false;
    }
    return first_octet == 10 || first_octet == 127 ||
           (first_octet == 169 && second_octet == 254) ||
           (first_octet == 172 && second_octet >= 16 && second_octet <= 31) ||
           (first_octet == 192 && second_octet == 168);
}

bool IsStaleConfiguredEndpoint(const std::string& configured_url,
                               const std::string& canonical_url) {
    return configured_url != canonical_url &&
           (IsEphemeralEndpoint(configured_url) ||
            IsPrivateOrLocalHost(ExtractUrlHost(configured_url)));
}

int RemainingCheckTimeoutMs(int64_t deadline_us) {
    const int64_t remaining_us = deadline_us - esp_timer_get_time();
    if (remaining_us <= 0) {
        return 1;
    }
    return static_cast<int>(std::min<int64_t>(
        Ota::kHttpTimeoutMs, (remaining_us + 999) / 1000));
}

std::vector<std::string> BuildCheckVersionUrls(const std::string& configured_url) {
    std::vector<std::string> urls;
    const std::string canonical_url = CONFIG_OTA_URL;
    auto add_unique = [&urls](const std::string& url) {
        if (url.length() >= 10 && !IsEphemeralEndpoint(url) &&
            std::find(urls.begin(), urls.end(), url) == urls.end()) {
            urls.push_back(url);
        }
    };

    if (IsStaleConfiguredEndpoint(configured_url, canonical_url)) {
        add_unique(canonical_url);
        if (IsEphemeralEndpoint(configured_url)) {
            ESP_LOGW(TAG, "Ignoring stale ephemeral OTA URL from NVS");
        } else {
            add_unique(configured_url);
        }
    } else {
        add_unique(configured_url);
        add_unique(canonical_url);
    }
    return urls;
}

bool IsValidCheckVersionResponse(const cJSON* root,
                                 const std::string& current_version,
                                 bool* should_download) {
    if (!cJSON_IsObject(root)) {
        return false;
    }
    const cJSON* firmware = cJSON_GetObjectItem(root, "firmware");
    const cJSON* version = cJSON_GetObjectItem(firmware, "version");
    const cJSON* url = cJSON_GetObjectItem(firmware, "url");
    if (!cJSON_IsObject(firmware) || !cJSON_IsString(version) ||
        !cJSON_IsString(url) || should_download == nullptr) {
        return false;
    }
    const cJSON* force = cJSON_GetObjectItem(firmware, "force");
    const bool force_install = cJSON_IsNumber(force) && force->valueint == 1;
    const FirmwareResponseDecision decision = EvaluateFirmwareResponse(
        current_version, version->valuestring, url->valuestring, force_install);
    *should_download = decision.should_download;
    return decision.valid;
}

void PersistRecoveredOtaUrl(const std::string& url) {
    Settings settings("wifi", true);
    if (settings.GetString("ota_url") != url) {
        settings.SetString("ota_url", url);
        ESP_LOGW(TAG, "Recovered canonical OTA URL persisted to NVS");
    }
}

} // namespace


Ota::Ota() {
#ifdef ESP_EFUSE_BLOCK_USR_DATA
    // Read Serial Number from efuse user_data
    uint8_t serial_number[33] = {0};
    if (esp_efuse_read_field_blob(ESP_EFUSE_USER_DATA, serial_number, 32 * 8) == ESP_OK) {
        if (serial_number[0] == 0) {
            has_serial_number_ = false;
        } else {
            serial_number_ = std::string(reinterpret_cast<char*>(serial_number), 32);
            has_serial_number_ = true;
        }
    }
#endif
#if CONFIG_TBOT_COURSE_MODE_LOCAL_ENDPOINT
    // Keep the compiled lab route available when OTA response validation fails.
    if (IsValidCourseModeWebsocketUrl(CONFIG_WEBSOCKET_URL)) {
        transient_websocket_url_ = CONFIG_WEBSOCKET_URL;
    }
#endif
}

Ota::~Ota() {
}

std::string Ota::GetCheckVersionUrl() {
#if CONFIG_TBOT_COURSE_MODE_LOCAL_ENDPOINT
    if (!IsValidCourseModeOtaUrl(CONFIG_OTA_URL)) {
        return {};
    }
    return CONFIG_OTA_URL;
#else
    Settings settings("wifi", false);
    std::string url = settings.GetString("ota_url");
    if (url.empty()) {
        url = CONFIG_OTA_URL;
    }
    return url;
#endif
}

bool Ota::ParseCourseModeResponse(const cJSON* root) {
    transient_websocket_token_.clear();
    has_websocket_config_ = false;
    has_mqtt_config_ = false;
    has_new_version_ = false;
    has_activation_code_ = false;
    has_activation_challenge_ = false;
    has_server_time_ = false;

    if (!cJSON_IsObject(root) || cJSON_GetObjectItem(root, "firmware") != nullptr ||
        cJSON_GetObjectItem(root, "mqtt") != nullptr ||
        cJSON_GetObjectItem(root, "api_url") != nullptr ||
        cJSON_GetObjectItem(root, "claim_reset") != nullptr ||
        cJSON_GetObjectItem(root, "activation") != nullptr ||
        cJSON_GetObjectItem(root, "server_time") != nullptr ||
        cJSON_GetObjectItem(root, "factory_test_claimed") != nullptr) {
        return false;
    }
    const cJSON* item = nullptr;
    size_t root_field_count = 0;
    cJSON_ArrayForEach(item, root) {
        if (item->string == nullptr || std::strcmp(item->string, "websocket") != 0) return false;
        ++root_field_count;
    }
    if (root_field_count != 1) return false;

    const cJSON* websocket = cJSON_GetObjectItem(root, "websocket");
    if (!cJSON_IsObject(websocket)) return false;
    const cJSON* url = cJSON_GetObjectItem(websocket, "url");
    const cJSON* token = cJSON_GetObjectItem(websocket, "token");
    if (!cJSON_IsString(url) || !cJSON_IsString(token)) return false;
    size_t websocket_field_count = 0;
    cJSON_ArrayForEach(item, websocket) {
        if (item->string == nullptr ||
            (std::strcmp(item->string, "url") != 0 && std::strcmp(item->string, "token") != 0) ||
            std::strcmp(item->string, "factory_test_claimed") == 0) {
            return false;
        }
        ++websocket_field_count;
    }
    if (websocket_field_count != 2) return false;
    const std::string_view websocket_url(url->valuestring);
    const std::string_view websocket_token(token->valuestring);
    if (!IsValidCourseModeWebsocketUrl(websocket_url) ||
        websocket_url != CONFIG_WEBSOCKET_URL || websocket_token.empty() ||
        websocket_token.size() > 1024) {
        return false;
    }
    for (const unsigned char byte : websocket_token) {
        if (byte < 0x21 || byte > 0x7e) return false;
    }
    transient_websocket_url_.assign(websocket_url);
    transient_websocket_token_.assign(websocket_token);
    has_websocket_config_ = true;
    return true;
}

std::unique_ptr<Http> Ota::SetupHttp(int timeout_ms) {
    auto& board = Board::GetInstance();
    auto network = board.GetNetwork();
    auto http = network->CreateHttp(0);
    auto user_agent = SystemInfo::GetUserAgent();
    http->SetHeader("Activation-Version", has_serial_number_ ? "2" : "1");
    http->SetHeader("Device-Id", SystemInfo::GetMacAddress().c_str());
    http->SetHeader("Client-Id", board.GetUuid());
    if (has_serial_number_) {
        http->SetHeader("Serial-Number", serial_number_.c_str());
        ESP_LOGI(TAG, "Setup activation HTTP (serial_present=1)");
    }
    http->SetHeader("User-Agent", user_agent);
    http->SetHeader("Accept-Language", Lang::CODE);
    http->SetHeader("Content-Type", "application/json");
    http->SetTimeout(timeout_ms);

    return http;
}

/*
 * Specification: https://ccnphfhqs21z.feishu.cn/wiki/FjW6wZmisimNBBkov6OcmfvknVd
 */
esp_err_t Ota::CheckVersion() {
#if CONFIG_TBOT_COURSE_MODE_LOCAL_ENDPOINT
    auto& board = Board::GetInstance();
    const auto app_desc = esp_app_get_description();
    current_version_ = app_desc->version;
    if (!IsValidCourseModeOtaUrl(CONFIG_OTA_URL) ||
        !IsValidCourseModeWebsocketUrl(CONFIG_WEBSOCKET_URL)) {
        ESP_LOGE(TAG, "Invalid compiled course-mode local endpoint");
        return ESP_ERR_INVALID_ARG;
    }

    auto http = SetupHttp();
    std::string data = board.GetSystemInfoJson();
    http->SetContent(std::move(data));
    if (!http->Open("POST", CONFIG_OTA_URL)) {
        return http->GetLastError();
    }
    ScopedHttpClose close_http(*http);
    if (http->GetStatusCode() != 200) return ESP_ERR_INVALID_RESPONSE;
    const std::string response_body = http->ReadAll();
    close_http.Close();
    cJSON* root = cJSON_Parse(response_body.c_str());
    const bool valid = ParseCourseModeResponse(root);
    cJSON_Delete(root);
    return valid ? ESP_OK : ESP_ERR_INVALID_RESPONSE;
#else
    auto& board = Board::GetInstance();
    auto app_desc = esp_app_get_description();

    // Check if there is a new firmware version available
    current_version_ = app_desc->version;
    ESP_LOGI(TAG, "Current version: %s", current_version_.c_str());

    std::string configured_url = GetCheckVersionUrl();
    auto urls = BuildCheckVersionUrls(configured_url);
    if (urls.empty()) {
        ESP_LOGE(TAG, "Check version URL is not properly set");
        return ESP_ERR_INVALID_ARG;
    }

    std::string data = board.GetSystemInfoJson();
    std::string method = data.length() > 0 ? "POST" : "GET";
    const int64_t check_deadline_us =
        esp_timer_get_time() + static_cast<int64_t>(Ota::kHttpTimeoutMs) * 1000;

    int last_error = ESP_FAIL;
    std::string successful_url;
    cJSON* root = nullptr;
    bool selected_should_download = false;
    for (const auto& url : urls) {
        if (RemainingCheckTimeoutMs(check_deadline_us) <= 1 &&
            esp_timer_get_time() >= check_deadline_us) {
            last_error = ESP_ERR_TIMEOUT;
            break;
        }

        auto http = SetupHttp(RemainingCheckTimeoutMs(check_deadline_us));
        std::string body_copy = data;
        http->SetContent(std::move(body_copy));
        if (!http->Open(method, url)) {
            last_error = http->GetLastError();
            ESP_LOGE(TAG, "Failed to open OTA HTTP connection, code=0x%x", last_error);
            continue;
        }
        ScopedHttpClose close_http(*http);

        http->SetTimeout(RemainingCheckTimeoutMs(check_deadline_us));
        const int status_code = http->GetStatusCode();
        if (status_code != 200) {
            last_error = status_code;
            ESP_LOGE(TAG, "Failed to check version, status code: %d", status_code);
            continue;
        }

        http->SetTimeout(RemainingCheckTimeoutMs(check_deadline_us));
        std::string response_body = http->ReadAll();
        close_http.Close();

        cJSON* candidate_root = cJSON_Parse(response_body.c_str());
        bool candidate_should_download = false;
        if (!IsValidCheckVersionResponse(candidate_root, current_version_,
                                         &candidate_should_download)) {
            last_error = ESP_ERR_INVALID_RESPONSE;
            ESP_LOGE(TAG, "Invalid OTA check response");
            cJSON_Delete(candidate_root);
            continue;
        }

        root = candidate_root;
        selected_should_download = candidate_should_download;
        successful_url = url;
        break;
    }

    if (root == nullptr) {
        return last_error;
    }
    if (successful_url != configured_url) {
        PersistRecoveredOtaUrl(successful_url);
    }

    has_activation_code_ = false;
    has_activation_challenge_ = false;
    cJSON *activation = cJSON_GetObjectItem(root, "activation");
    if (cJSON_IsObject(activation)) {
        cJSON* message = cJSON_GetObjectItem(activation, "message");
        if (cJSON_IsString(message)) {
            activation_message_ = message->valuestring;
        }
        cJSON* code = cJSON_GetObjectItem(activation, "code");
        if (cJSON_IsString(code)) {
            activation_code_ = code->valuestring;
            has_activation_code_ = true;
        }
        cJSON* challenge = cJSON_GetObjectItem(activation, "challenge");
        if (cJSON_IsString(challenge)) {
            activation_challenge_ = challenge->valuestring;
            has_activation_challenge_ = true;
        }
        cJSON* timeout_ms = cJSON_GetObjectItem(activation, "timeout_ms");
        if (cJSON_IsNumber(timeout_ms)) {
            activation_timeout_ms_ = timeout_ms->valueint;
        }
    }

    has_mqtt_config_ = false;
    cJSON *mqtt = cJSON_GetObjectItem(root, "mqtt");
    if (cJSON_IsObject(mqtt)) {
        Settings settings("mqtt", true);
        cJSON *item = NULL;
        cJSON_ArrayForEach(item, mqtt) {
            if (cJSON_IsString(item)) {
                if (settings.GetString(item->string) != item->valuestring) {
                    settings.SetString(item->string, item->valuestring);
                }
            } else if (cJSON_IsNumber(item)) {
                if (settings.GetInt(item->string) != item->valueint) {
                    settings.SetInt(item->string, item->valueint);
                }
            }
        }
        has_mqtt_config_ = true;
    } else {
        ESP_LOGI(TAG, "No mqtt section found !");
    }

    has_websocket_config_ = false;
    cJSON *websocket = cJSON_GetObjectItem(root, "websocket");
    bool factory_test_claimed_seen = false;
    int factory_test_claimed_value = 0;
    if (cJSON_IsObject(websocket)) {
        Settings settings("websocket", true);
        cJSON *item = NULL;
        cJSON_ArrayForEach(item, websocket) {
            if (cJSON_IsString(item)) {
                if (std::strcmp(item->string, "token") == 0) {
                    ESP_LOGI(TAG, "Received websocket token: empty=%d",
                             item->valuestring[0] == '\0');
                }
                if (settings.GetString(item->string) != item->valuestring) {
                    settings.SetString(item->string, item->valuestring);
                }
            } else if (cJSON_IsNumber(item)) {
                if (std::strcmp(item->string, "factory_test_claimed") == 0) {
                    factory_test_claimed_seen = true;
                    factory_test_claimed_value = item->valueint != 0 ? 1 : 0;
                } else {
                    ESP_LOGW(TAG, "Ignoring unsupported websocket numeric field: %s", item->string);
                }
            }
        }
        {
            Settings claim_state("tbot_claim", true);
            claim_state.SetInt("factory_test",
                               factory_test_claimed_seen ? factory_test_claimed_value : 0);
        }
        has_websocket_config_ = true;
    } else {
        Settings claim_state("tbot_claim", true);
        claim_state.SetInt("factory_test", 0);
        ESP_LOGI(TAG, "No websocket section found!");
    }

    cJSON *api_url = cJSON_GetObjectItem(root, "api_url");
    if (cJSON_IsString(api_url) && api_url->valuestring[0] != '\0') {
        Settings settings("backend", true);
        if (settings.GetString("api_url") != api_url->valuestring) {
            settings.SetString("api_url", api_url->valuestring);
        }
    }

    cJSON *claim_reset = cJSON_GetObjectItem(root, "claim_reset");
    if (cJSON_IsObject(claim_reset)) {
        cJSON *local_claim = cJSON_GetObjectItem(claim_reset, "local_claim");
        cJSON *nonce = cJSON_GetObjectItem(claim_reset, "nonce");
        const bool should_reset_local_claim =
            (cJSON_IsNumber(local_claim) && local_claim->valueint != 0) || cJSON_IsTrue(local_claim);
        if (should_reset_local_claim && cJSON_IsString(nonce) && nonce->valuestring[0] != '\0') {
            const std::string reset_nonce = nonce->valuestring;
            Settings reset_state("tbot_reset", true);
            if (reset_state.GetString("claim_reset_nonce") != reset_nonce) {
                ESP_LOGW(TAG, "OTA claim reset requested; clearing local ownership state and rebooting");
                {
                    Settings claim_state("tbot_claim", true);
                    claim_state.SetInt("confirmed", 0);
                    claim_state.SetInt("factory_test", 0);
                }
                {
                    Settings backend_settings("backend", true);
                    backend_settings.SetString("device_id", "");
                    backend_settings.SetString("device_secret", "");
                    backend_settings.SetInt("release_pending", 0);
                }
                {
                    Settings websocket_settings("websocket", true);
                    websocket_settings.SetString("bootstrap_token", "");
                    websocket_settings.SetString("token", "");
                    websocket_settings.SetString("url", "");
                    websocket_settings.SetString("claim_device_id", "");
                }
                reset_state.SetString("claim_reset_nonce", reset_nonce);
                cJSON_Delete(root);
                vTaskDelay(pdMS_TO_TICKS(500));
                esp_restart();
                return ESP_OK;
            }
        }
    }

    has_server_time_ = false;
    cJSON *server_time = cJSON_GetObjectItem(root, "server_time");
    if (cJSON_IsObject(server_time)) {
        cJSON *timestamp = cJSON_GetObjectItem(server_time, "timestamp");
        cJSON *timezone_offset = cJSON_GetObjectItem(server_time, "timezone_offset");

        if (cJSON_IsNumber(timestamp)) {
            // 设置系统时间
            struct timeval tv;
            double ts = timestamp->valuedouble;

            // 如果有时区偏移，计算本地时间
            if (cJSON_IsNumber(timezone_offset)) {
                ts += (timezone_offset->valueint * 60 * 1000); // 转换分钟为毫秒
            }

            tv.tv_sec = (time_t)(ts / 1000);  // 转换毫秒为秒
            tv.tv_usec = (suseconds_t)((long long)ts % 1000) * 1000;  // 剩余的毫秒转换为微秒
            settimeofday(&tv, NULL);
            has_server_time_ = true;
        }
    } else {
        ESP_LOGW(TAG, "No server_time section found!");
    }

    has_new_version_ = false;
    cJSON *firmware = cJSON_GetObjectItem(root, "firmware");
    if (cJSON_IsObject(firmware)) {
        cJSON *version = cJSON_GetObjectItem(firmware, "version");
        if (cJSON_IsString(version)) {
            firmware_version_ = version->valuestring;
        }
        cJSON *url = cJSON_GetObjectItem(firmware, "url");
        if (cJSON_IsString(url)) {
            firmware_url_ = url->valuestring;
        }

        if (cJSON_IsString(version) && cJSON_IsString(url)) {
            has_new_version_ = selected_should_download;
            if (has_new_version_) {
                ESP_LOGI(TAG, "New version available: %s", firmware_version_.c_str());
            } else {
                ESP_LOGI(TAG, "Current is the latest version");
            }
        }
    } else {
        ESP_LOGW(TAG, "No firmware section found!");
    }

    cJSON_Delete(root);
    return ESP_OK;
#endif
}

void Ota::MarkCurrentVersionValid() {
    auto partition = esp_ota_get_running_partition();
    if (strcmp(partition->label, "factory") == 0) {
        ESP_LOGI(TAG, "Running from factory partition, skipping");
        return;
    }

    ESP_LOGI(TAG, "Running partition: %s", partition->label);
    esp_ota_img_states_t state;
    if (esp_ota_get_state_partition(partition, &state) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get state of partition");
        return;
    }

    if (state == ESP_OTA_IMG_PENDING_VERIFY) {
        ESP_LOGI(TAG, "Marking firmware as valid");
        esp_ota_mark_app_valid_cancel_rollback();
    }
}

bool Ota::Upgrade(const std::string& firmware_url, std::function<void(int progress, size_t speed)> callback) {
    ESP_LOGI(TAG, "Upgrading firmware from authenticated URL");
    esp_ota_handle_t update_handle = 0;
    auto update_partition = esp_ota_get_next_update_partition(NULL);
    if (update_partition == NULL) {
        ESP_LOGE(TAG, "Failed to get update partition");
        return false;
    }

    ESP_LOGI(TAG, "Writing to partition %s at offset 0x%lx", update_partition->label, update_partition->address);
    bool image_header_checked = false;
    std::string image_header;

    auto network = Board::GetInstance().GetNetwork();
    auto http = network->CreateHttp(0);
    if (!http->Open("GET", firmware_url)) {
        ESP_LOGE(TAG, "Failed to open HTTP connection");
        return false;
    }

    if (http->GetStatusCode() != 200) {
        ESP_LOGE(TAG, "Failed to get firmware, status code: %d", http->GetStatusCode());
        return false;
    }

    size_t content_length = http->GetBodyLength();
    if (content_length == 0) {
        ESP_LOGE(TAG, "Failed to get content length");
        http->Close();
        return false;
    }

    if (content_length > update_partition->size) {
        ESP_LOGE(TAG, "Firmware image too large: content_length=%zu partition_size=%lu partition=%s",
                 content_length, static_cast<unsigned long>(update_partition->size), update_partition->label);
        http->Close();
        return false;
    }

    constexpr size_t PAGE_SIZE = 4096;
    char* buffer = (char*)heap_caps_malloc(PAGE_SIZE, MALLOC_CAP_INTERNAL);
    if (buffer == nullptr) {
        ESP_LOGE(TAG, "Failed to allocate buffer");
        return false;
    }

    size_t buffer_offset = 0;  // Current data size in buffer
    size_t total_read = 0, recent_read = 0;
    auto last_calc_time = esp_timer_get_time();
    while (true) {
        int ret = http->Read(buffer + buffer_offset, PAGE_SIZE - buffer_offset);
        if (ret < 0) {
            ESP_LOGE(TAG, "Failed to read HTTP data: %s", esp_err_to_name(ret));
            heap_caps_free(buffer);
            return false;
        }

        // Calculate speed and progress every second
        recent_read += ret;
        total_read += ret;
        buffer_offset += ret;
        if (esp_timer_get_time() - last_calc_time >= 1000000 || ret == 0) {
            size_t progress = total_read * 100 / content_length;
            ESP_LOGI(TAG, "Progress: %u%% (%u/%u), Speed: %uB/s", progress, total_read, content_length, recent_read);
            if (callback) {
                callback(progress, recent_read);
            }
            last_calc_time = esp_timer_get_time();
            recent_read = 0;
        }

        if (!image_header_checked) {
            image_header.append(buffer, buffer_offset);
            if (image_header.size() >= sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t) + sizeof(esp_app_desc_t)) {
                esp_app_desc_t new_app_info;
                memcpy(&new_app_info, image_header.data() + sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t), sizeof(esp_app_desc_t));

                ESP_LOGI(TAG, "New firmware image version: %s", new_app_info.version);

                esp_err_t begin_err = esp_ota_begin(update_partition, OTA_WITH_SEQUENTIAL_WRITES, &update_handle);
                if (begin_err != ESP_OK) {
                    esp_ota_abort(update_handle);
                    ESP_LOGE(TAG, "Failed to begin OTA: %s", esp_err_to_name(begin_err));
                    heap_caps_free(buffer);
                    http->Close();
                    return false;
                }

                image_header_checked = true;
                std::string().swap(image_header);
            }
        }

        // Write to flash when buffer is full (4KB) or it's the last chunk
        bool is_last_chunk = (ret == 0);
        if (buffer_offset == PAGE_SIZE || (is_last_chunk && buffer_offset > 0)) {
            auto err = esp_ota_write(update_handle, buffer, buffer_offset);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "Failed to write OTA data: %s", esp_err_to_name(err));
                esp_ota_abort(update_handle);
                heap_caps_free(buffer);
                return false;
            }

            buffer_offset = 0;
        }

        if (is_last_chunk) {
            break;
        }
    }

    if (total_read != content_length) {
        ESP_LOGE(TAG, "Firmware download size mismatch: total_read=%zu content_length=%zu",
                 total_read, content_length);
        if (update_handle != 0) {
            esp_ota_abort(update_handle);
        }
        http->Close();
        heap_caps_free(buffer);
        return false;
    }

    ESP_LOGI(TAG, "Firmware download complete: total_read=%zu content_length=%zu",
             total_read, content_length);
    http->Close();
    heap_caps_free(buffer);

    esp_err_t err = esp_ota_end(update_handle);
    if (err != ESP_OK) {
        if (err == ESP_ERR_OTA_VALIDATE_FAILED) {
            ESP_LOGE(TAG, "Image validation failed, image is corrupted");
        } else {
            ESP_LOGE(TAG, "Failed to end OTA: %s", esp_err_to_name(err));
        }
        return false;
    }

    err = esp_ota_set_boot_partition(update_partition);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set boot partition: %s", esp_err_to_name(err));
        return false;
    }

    ESP_LOGI(TAG, "Firmware upgrade successful");
    return true;
}

bool Ota::StartUpgrade(std::function<void(int progress, size_t speed)> callback) {
    return Upgrade(firmware_url_, callback);
}


std::string Ota::GetActivationPayload() {
    if (!has_serial_number_) {
        return "{}";
    }

    std::string hmac_hex;
#ifdef SOC_HMAC_SUPPORTED
    uint8_t hmac_result[32]; // SHA-256 输出为32字节

    // 使用Key0计算HMAC
    esp_err_t ret = esp_hmac_calculate(HMAC_KEY0, (uint8_t*)activation_challenge_.data(), activation_challenge_.size(), hmac_result);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "HMAC calculation failed: %s", esp_err_to_name(ret));
        return "{}";
    }

    for (size_t i = 0; i < sizeof(hmac_result); i++) {
        char buffer[3];
        sprintf(buffer, "%02x", hmac_result[i]);
        hmac_hex += buffer;
    }
#endif

    cJSON *payload = cJSON_CreateObject();
    cJSON_AddStringToObject(payload, "algorithm", "hmac-sha256");
    cJSON_AddStringToObject(payload, "serial_number", serial_number_.c_str());
    cJSON_AddStringToObject(payload, "challenge", activation_challenge_.c_str());
    cJSON_AddStringToObject(payload, "hmac", hmac_hex.c_str());
    auto json_str = cJSON_PrintUnformatted(payload);
    std::string json(json_str);
    cJSON_free(json_str);
    cJSON_Delete(payload);

    ESP_LOGI(TAG, "Activation payload prepared (serial_len=%u challenge_len=%u hmac_len=%u)",
             static_cast<unsigned>(serial_number_.size()),
             static_cast<unsigned>(activation_challenge_.size()),
             static_cast<unsigned>(hmac_hex.size()));
    return json;
}

esp_err_t Ota::Activate() {
    if (!has_activation_challenge_) {
        ESP_LOGW(TAG, "No activation challenge found");
        return ESP_FAIL;
    }

    std::string url = GetCheckVersionUrl();
    if (url.back() != '/') {
        url += "/activate";
    } else {
        url += "activate";
    }

    auto http = SetupHttp();

    std::string data = GetActivationPayload();
    http->SetContent(std::move(data));

    if (!http->Open("POST", url)) {
        ESP_LOGE(TAG, "Failed to open HTTP connection");
        return ESP_FAIL;
    }

    auto status_code = http->GetStatusCode();
    if (status_code == 202) {
        return ESP_ERR_TIMEOUT;
    }
    if (status_code != 200) {
        const std::string response_body = http->ReadAll();
        ESP_LOGE(TAG, "Failed to activate, code=%d body_len=%u", status_code,
                 static_cast<unsigned>(response_body.size()));
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Activation successful");
    return ESP_OK;
}
