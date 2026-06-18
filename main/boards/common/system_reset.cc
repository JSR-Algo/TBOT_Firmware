#include "system_reset.h"

#include <esp_log.h>
#include <nvs_flash.h>
#include <driver/gpio.h>
#include <esp_partition.h>
#include <esp_system.h>
#include <freertos/FreeRTOS.h>

#include <string>
#include <utility>

#include "board.h"
#include "settings.h"
#include "system_info.h"


#define TAG "SystemReset"


SystemReset::SystemReset(gpio_num_t reset_nvs_pin, gpio_num_t reset_factory_pin) : reset_nvs_pin_(reset_nvs_pin), reset_factory_pin_(reset_factory_pin) {
    // Configure GPIO1, GPIO2 as INPUT, reset NVS flash if the button is pressed
    gpio_config_t io_conf;
    io_conf.intr_type = GPIO_INTR_DISABLE;
    io_conf.mode = GPIO_MODE_INPUT;
    io_conf.pin_bit_mask = (1ULL << reset_nvs_pin_) | (1ULL << reset_factory_pin_);
    io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io_conf.pull_up_en = GPIO_PULLUP_ENABLE;
    gpio_config(&io_conf);
}


void SystemReset::CheckButtons() {
    if (gpio_get_level(reset_factory_pin_) == 0) {
        ESP_LOGI(TAG, "Button is pressed, reset to factory");
        if (ReleaseCloudOwnership()) {
            ResetNvsFlash();
            ResetToFactory();
        } else {
            ESP_LOGW(TAG, "Cloud ownership release failed; keeping NVS credentials so reset can be retried");
            RestartInSeconds(3);
        }
    }

    if (gpio_get_level(reset_nvs_pin_) == 0) {
        ESP_LOGI(TAG, "Button is pressed, reset NVS flash");
        ResetNvsFlash();
    }
}

static std::string UrlEncodeQueryParam(const std::string& value) {
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

static std::string BuildFactoryResetUrl(const std::string& api_url, const std::string& device_id) {
    if (api_url.empty() || device_id.empty()) {
        return "";
    }
    std::string base = api_url;
    while (!base.empty() && base.back() == '/') {
        base.pop_back();
    }
    if (base.find("/v1") == std::string::npos) {
        base += "/v1";
    }
    return base + "/devices/" + UrlEncodeQueryParam(device_id) + "/factory-reset";
}

bool SystemReset::ReleaseCloudOwnership() {
    Settings backend_settings("backend", false);
    const std::string api_url = backend_settings.GetString("api_url");
    const std::string device_id = backend_settings.GetString("device_id");
    const std::string device_secret = backend_settings.GetString("device_secret");

    if (api_url.empty() || device_secret.empty() || device_id.empty()) {
        ESP_LOGI(TAG, "No cloud ownership credentials present; local factory reset can continue");
        return true;
    }

    const std::string url = BuildFactoryResetUrl(api_url, device_id);
    if (url.empty()) {
        ESP_LOGW(TAG, "Cannot release cloud ownership: missing API URL or device id");
        return false;
    }

    auto* network = Board::GetInstance().GetNetwork();
    auto http = network->CreateHttp(2);
    if (!http) {
        ESP_LOGE(TAG, "Failed to create HTTP client for cloud factory reset");
        return false;
    }

    http->SetTimeout(5000);
    http->SetHeader("X-Device-Token", device_secret);
    http->SetHeader("X-Device-Id", device_id);
    http->SetHeader("Content-Type", "application/json");
    http->SetHeader("Device-Id", SystemInfo::GetMacAddress());
    http->SetHeader("User-Agent", SystemInfo::GetUserAgent());
    std::string body = "{}";
    http->SetContent(std::move(body));

    ESP_LOGI(TAG, "Releasing cloud ownership before local factory reset");
    if (!http->Open("POST", url)) {
        ESP_LOGW(TAG, "Cloud factory reset HTTP open failed: 0x%x", http->GetLastError());
        http->Close();
        return false;
    }

    const int status_code = http->GetStatusCode();
    http->Close();
    if (status_code == 401) {
        ESP_LOGW(TAG, "Cloud factory reset rejected stale device credentials (HTTP 401); clearing local release marker");
        Settings writable_backend_settings("backend", true);
        writable_backend_settings.SetString("device_secret", "");
        writable_backend_settings.SetInt("release_pending", 0);
        Settings claim_state("tbot_claim", true);
        claim_state.SetInt("confirmed", 0);
        return true;
    }
    if (status_code < 200 || status_code >= 300) {
        ESP_LOGW(TAG, "Cloud factory reset failed (HTTP %d)", status_code);
        return false;
    }

    ESP_LOGI(TAG, "Cloud ownership released (HTTP %d)", status_code);
    return true;
}

void SystemReset::ResetNvsFlash() {
    ESP_LOGI(TAG, "Resetting NVS flash");
    esp_err_t ret = nvs_flash_erase();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to erase NVS flash");
    }
    ret = nvs_flash_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize NVS flash");
    }
}

void SystemReset::ResetToFactory() {
    ESP_LOGI(TAG, "Resetting to factory");
    // Erase otadata partition
    const esp_partition_t* partition = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_OTA, NULL);
    if (partition == NULL) {
        ESP_LOGE(TAG, "Failed to find otadata partition");
        return;
    }
    esp_partition_erase_range(partition, 0, partition->size);
    ESP_LOGI(TAG, "Erased otadata partition");

    // Reboot in 3 seconds
    RestartInSeconds(3);
}

void SystemReset::RestartInSeconds(int seconds) {
    for (int i = seconds; i > 0; i--) {
        ESP_LOGI(TAG, "Resetting in %d seconds", i);
        vTaskDelay(1000 / portTICK_PERIOD_MS);
    }
    esp_restart();
}
