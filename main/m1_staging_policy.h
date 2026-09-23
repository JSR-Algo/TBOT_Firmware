#pragma once

#ifdef ESP_PLATFORM
#include "sdkconfig.h"
#endif
#include <string>

// A separate attended staging profile; ordinary persisted identity is never migrated.
namespace M1Staging {
static constexpr const char* kApi = "https://m0-api.tjbot.vn/v1";
static constexpr const char* kOta = "https://m0-esp.tjbot.vn/tbot/ota/";
static constexpr const char* kWebsocket = "wss://m0-esp.tjbot.vn/tbot/v1/";
static constexpr const char* kProvisioning = "https://m0-api.tjbot.vn/v1/device/provisioning/status";

static inline bool ApiAllowed([[maybe_unused]] const std::string& url) {
#if CONFIG_TBOT_M1_STAGING
    return url == kApi || url == std::string(kApi) + "/";
#else
    return true;
#endif
}

static inline bool WebsocketAllowed([[maybe_unused]] const std::string& url) {
#if CONFIG_TBOT_M1_STAGING
    return url == kWebsocket;
#else
    return true;
#endif
}

static inline std::string StorageNamespace(const std::string& name) {
#if CONFIG_TBOT_M1_STAGING
    if (name == "backend") return "m1_backend";
    if (name == "websocket") return "m1_websocket";
    if (name == "tbot_claim") return "m1_claim";
    if (name == "tbot_reset") return "m1_reset";
    if (name == "mqtt") return "m1_mqtt";
    if (name == "lesson_select") return "m1_select";
    if (name == "lesson_course") return "m1_course";
#endif
    return name;
}

static inline const char* PinnedSetting([[maybe_unused]] const std::string& ns,
                                      [[maybe_unused]] const std::string& key) {
#if CONFIG_TBOT_M1_STAGING
    if (ns == "wifi" && key == "ota_url") return kOta;
    if (ns == "wifi" && key == "provisioning_url") return kProvisioning;
    if (ns == "backend" && key == "api_url") return kApi;
    if (ns == "websocket" && key == "url") return kWebsocket;
#endif
    return nullptr;
}
}
