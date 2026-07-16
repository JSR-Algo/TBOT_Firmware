/*
 * MCP Server Implementation
 * Reference: https://modelcontextprotocol.io/specification/2024-11-05
 */

#include "mcp_server.h"
#include <esp_log.h>
#include <esp_app_desc.h>
#include <esp_heap_caps.h>
#include <algorithm>
#include <cctype>
#include <cstring>
#include <cstdio>
#include <cerrno>
#include <memory>
#include <initializer_list>
#include <vector>
#include <sys/stat.h>
#include <esp_pthread.h>
#include <esp_task_wdt.h>

#include "application.h"
#include "display.h"
#include "oled_display.h"
#include "board.h"
#include "settings.h"
#include "lvgl_theme.h"
#include "lvgl_display.h"
#include "lesson_asset_cache_evict.h"
#include "lesson_asset_storage_coordinator.h"
#include "lesson_asset_download_raii.h"
#include "lesson_asset_download_staging.h"
#include "lesson_asset_http_transfer.h"
#include "lesson_asset_sample_url_policy.h"
#include "lesson_asset_sync_path_policy.h"
#include <lesson_asset_sync_attestation.h>
#if CONFIG_TBOT_HIL_STORAGE_FAULTS
#include "lesson_storage_hil_mcp_tools.h"
#include "lesson_storage_hil_controller.h"
#endif

#define TAG "MCP"

namespace {
constexpr size_t kMcpToolsListMaxPayloadBytes = 2500;
constexpr size_t kLessonAssetSyncMaxAssets = 64;
constexpr const char* kLessonAssetPackRoot = "/sdcard/tbot/lesson-assets/";
constexpr const char* kSampleLessonAssetDir = "/sdcard/tbot/lesson-assets/sample-barn";
struct SampleLessonAsset {
    const char* file;
    const char* sha256;
};
constexpr SampleLessonAsset kSampleLessonAssets[] = {
    {"barn-round-field-poster.jpg", "d5cdaba9f9086ef56a5f41c5fddf2e32b91ecfe141cc346f3221c7b221a3a357"},
    {"barn.png", "bf3d88d17867f02872e3e6aff31b5d4d0a94977a5efa4214b7e831122938511b"},
    {"bright-teach.png", "576d86a75686f6eab606295529593da14b01554e21e0601c8f29aedbc1ba4965"},
    {"bright-listening.png", "572a61f140eca17968a85f61704967d03a1a3311222335e32b94b1ab370e2419"},
    {"bright-thinking.png", "3d3d3c3a7c6993ad2346e11cfee72a15750d0b5f35642b8d949902a8745352c8"},
    {"bright-celebrate.png", "8392fb31c53030147d27fbd96c5b2dd1a4e5c33efd35f8727bee6dabda62605d"},
};

bool DownloadLessonAssetToFile(
    const LessonAssetMutationLease& mutation,
    const char* cache_key,
    bool has_declared_size,
    size_t declared_size,
    const std::string& url,
    const std::string& dest_path,
    size_t& bytes_out
);
void EnsureDirOrThrow(const LessonAssetMutationLease& mutation, const char* path);

void RequireLessonAssetMutationLease(const LessonAssetMutationLease& mutation) {
    if (!mutation) {
        throw std::runtime_error("lesson asset mutation lease required");
    }
}

[[noreturn]] void ThrowLessonAssetMutationRefusal(LessonAssetReservationCode code) {
    if (code == LessonAssetReservationCode::kLessonSessionActive) {
        throw std::runtime_error("lesson asset session active");
    }
    throw std::runtime_error("lesson asset storage busy");
}

bool EnsureDir(const LessonAssetMutationLease& mutation, const char* path) {
    RequireLessonAssetMutationLease(mutation);
    if (path == nullptr || path[0] == '\0') return false;
    errno = 0;
    if (mkdir(path, 0755) == 0 || errno == EEXIST) return true;
    return false;
}

void EnsureLessonAssetParentDirs(
    const LessonAssetMutationLease& mutation,
    const std::string& file_path
) {
    RequireLessonAssetMutationLease(mutation);
    EnsureDirOrThrow(mutation, "/sdcard/tbot");
    EnsureDirOrThrow(mutation, "/sdcard/tbot/lesson-assets");
    size_t pos = strlen(kLessonAssetPackRoot);
    while ((pos = file_path.find('/', pos)) != std::string::npos) {
        EnsureDirOrThrow(mutation, file_path.substr(0, pos).c_str());
        ++pos;
    }
}

const char* JsonStringField(const cJSON* object, const char* key) {
    const cJSON* value = cJSON_GetObjectItem(object, key);
    return cJSON_IsString(value) ? value->valuestring : nullptr;
}

struct ValidatedLessonAsset {
    const char* key;
    const char* path;
    const char* url;
    const char* sha256;
    const char* destination;
    bool has_declared_size;
    size_t declared_size;
};

template <size_t N>
bool HasOnlyAllowedJsonFields(
    const cJSON* object,
    const char* const (&allowed)[N]
) {
    if (!cJSON_IsObject(object)) {
        return false;
    }
    size_t seen[N] = {};
    const cJSON* field = nullptr;
    cJSON_ArrayForEach(field, object) {
        if (field->string == nullptr) {
            return false;
        }
        size_t match = N;
        for (size_t index = 0; index < N; ++index) {
            if (std::strcmp(field->string, allowed[index]) == 0) {
                match = index;
                break;
            }
        }
        if (match == N || ++seen[match] != 1) {
            return false;
        }
    }
    return true;
}

bool IsBoundedMetadataString(const char* value, size_t max_bytes) {
    if (value == nullptr) {
        return false;
    }
    const size_t length = std::strlen(value);
    if (length == 0 || length > max_bytes) {
        return false;
    }
    for (size_t index = 0; index < length; ++index) {
        const unsigned char byte = static_cast<unsigned char>(value[index]);
        if (byte < 0x20 || byte == 0x7f) {
            return false;
        }
    }
    return true;
}

bool IsOptionalBoundedString(const cJSON* object, const char* field, size_t max_bytes) {
    const cJSON* value = cJSON_GetObjectItem(object, field);
    return value == nullptr ||
           (cJSON_IsString(value) && IsBoundedMetadataString(value->valuestring, max_bytes));
}

bool IsOptionalBoolean(const cJSON* object, const char* field) {
    const cJSON* value = cJSON_GetObjectItem(object, field);
    return value == nullptr || cJSON_IsBool(value);
}

bool IsOptionalExactSha256(const cJSON* object, const char* field) {
    const cJSON* value = cJSON_GetObjectItem(object, field);
    return value == nullptr ||
           (cJSON_IsString(value) && IsExactLowerLessonAssetSha256(value->valuestring));
}

bool IsOptionalNonNegativeInteger(const cJSON* object, const char* field) {
    const cJSON* value = cJSON_GetObjectItem(object, field);
    return value == nullptr ||
           (cJSON_IsNumber(value) && value->valuedouble >= 0 &&
            value->valuedouble == static_cast<double>(value->valueint));
}

bool CacheKeyMatchesManifestChecksum(
    const char* cache_key,
    const char* manifest_checksum
) {
    // This proves exact wire echo consistency; firmware does not recompute the manifest hash.
    const size_t cache_key_length = std::strlen(cache_key);
    const size_t checksum_length = std::strlen(manifest_checksum);
    return checksum_length == kLessonAssetSyncSha256HexBytes &&
           cache_key_length > checksum_length &&
           cache_key[cache_key_length - checksum_length - 1] == '-' &&
           std::memcmp(
               cache_key + cache_key_length - checksum_length,
               manifest_checksum,
               checksum_length) == 0;
}

std::vector<ValidatedLessonAsset> ValidateLessonAssetSyncPackOrThrow(
    const cJSON* pack,
    const char*& cache_key_out
) {
    static constexpr const char* kPackFields[] = {
        "assignmentVersion", "lessonId", "lessonVersion", "manifestChecksum",
        "cacheKey", "localRoot", "ready", "assets",
    };
    static constexpr const char* kAssetFields[] = {
        "key", "path", "url", "sha256", "sourceSha256", "size", "mediaType",
        "critical", "layer", "role", "state", "checksumOk", "localPath",
    };
    const char* cache_key = JsonStringField(pack, "cacheKey");
    const char* manifest_checksum = JsonStringField(pack, "manifestChecksum");
    const char* local_root = JsonStringField(pack, "localRoot");
    const cJSON* assets = cJSON_GetObjectItem(pack, "assets");
    if (!HasOnlyAllowedJsonFields(pack, kPackFields) || cache_key == nullptr ||
        manifest_checksum == nullptr || local_root == nullptr ||
        !IsCanonicalLessonAssetSyncCacheKey(cache_key) ||
        !IsExactLowerLessonAssetSha256(manifest_checksum) ||
        !CacheKeyMatchesManifestChecksum(cache_key, manifest_checksum) ||
        !cJSON_IsArray(assets) ||
        !IsOptionalNonNegativeInteger(pack, "assignmentVersion") ||
        !IsOptionalNonNegativeInteger(pack, "lessonVersion") ||
        !IsOptionalBoundedString(pack, "lessonId", kLessonAssetIdentityMaxBytes) ||
        !IsOptionalBoolean(pack, "ready")) {
        throw std::runtime_error("lesson asset sync request invalid");
    }

    const std::string expected_root = std::string(kLessonAssetPackRoot) + cache_key;
    if (expected_root != local_root) {
        throw std::runtime_error("lesson asset sync request invalid");
    }

    const int asset_count = cJSON_GetArraySize(assets);
    if (asset_count <= 0 || static_cast<size_t>(asset_count) > kLessonAssetSyncMaxAssets) {
        throw std::runtime_error("lesson asset sync request invalid");
    }

    cache_key_out = cache_key;
    std::vector<ValidatedLessonAsset> validated;
    validated.reserve(static_cast<size_t>(asset_count));
    const cJSON* asset = nullptr;
    cJSON_ArrayForEach(asset, assets) {
        const char* key = JsonStringField(asset, "key");
        const char* path = JsonStringField(asset, "path");
        const char* url = JsonStringField(asset, "url");
        const char* sha256 = JsonStringField(asset, "sha256");
        const char* local_path = JsonStringField(asset, "localPath");
        const cJSON* declared_size = cJSON_GetObjectItem(asset, "size");
        if (!HasOnlyAllowedJsonFields(asset, kAssetFields) ||
            !IsBoundedMetadataString(key, kLessonAssetSyncKeyMaxBytes) ||
            !IsBoundedMetadataString(path, kLessonAssetSyncMetadataPathMaxBytes) ||
            url == nullptr || !IsAllowedLessonAssetSyncUrl(url) ||
            sha256 == nullptr || !IsExactLowerLessonAssetSha256(sha256) ||
            !IsOptionalExactSha256(asset, "sourceSha256") ||
            local_path == nullptr ||
            !IsOptionalNonNegativeInteger(asset, "size") ||
            !IsOptionalBoolean(asset, "critical") ||
            !IsOptionalBoolean(asset, "checksumOk") ||
            !IsOptionalBoundedString(asset, "mediaType", 128) ||
            !IsOptionalBoundedString(asset, "layer", 128) ||
            !IsOptionalBoundedString(asset, "role", 128) ||
            !IsOptionalBoundedString(asset, "state", 128)) {
            throw std::runtime_error("lesson asset sync request invalid");
        }
        const auto path_result = ValidateLessonAssetSyncPath(cache_key, local_path, key);
        if (path_result.code != LessonAssetSyncPathCode::kValid) {
            throw std::runtime_error("lesson asset sync request invalid");
        }
        for (const auto& existing : validated) {
            if (LessonAssetSyncDestinationsCollide(
                    existing.destination, path_result.destination)) {
                throw std::runtime_error("lesson asset sync request invalid");
            }
        }
        validated.push_back({
            key,
            path,
            url,
            sha256,
            local_path,
            declared_size != nullptr,
            declared_size == nullptr
                ? 0
                : static_cast<size_t>(declared_size->valuedouble),
        });
    }
    return validated;
}

void DownloadLessonAssetToVerifiedFile(
    const LessonAssetMutationLease& mutation,
    const char* cache_key,
    bool has_declared_size,
    size_t declared_size,
    const std::string& url,
    const std::string& dest_path,
    const std::string& sha256,
    size_t& bytes_out
) {
    RequireLessonAssetMutationLease(mutation);
    EnsureLessonAssetParentDirs(mutation, dest_path);
    LessonAssetDownloadStagingFile staging(dest_path);
    DownloadLessonAssetToFile(
        mutation,
        cache_key,
        has_declared_size,
        declared_size,
        url,
        staging.path(),
        bytes_out);
    CommitVerifiedLessonAssetDownload(staging, cache_key, dest_path, sha256);
}

void EnsureDirOrThrow(const LessonAssetMutationLease& mutation, const char* path) {
    RequireLessonAssetMutationLease(mutation);
    if (EnsureDir(mutation, path)) return;
    throw std::runtime_error("lesson asset storage write failed");
}

void EnsureSampleLessonAssetDir(const LessonAssetMutationLease& mutation) {
    RequireLessonAssetMutationLease(mutation);
    EnsureDirOrThrow(mutation, "/sdcard/tbot");
    EnsureDirOrThrow(mutation, "/sdcard/tbot/lesson-assets");
    EnsureDirOrThrow(mutation, kSampleLessonAssetDir);
}

bool DownloadLessonAssetToFile(
    const LessonAssetMutationLease& mutation,
    const char* cache_key,
    bool has_declared_size,
    size_t declared_size,
    const std::string& url,
    const std::string& dest_path,
    size_t& bytes_out
) {
    RequireLessonAssetMutationLease(mutation);
    bytes_out = 0;
    auto* network = Board::GetInstance().GetNetwork();
    if (network == nullptr) {
        throw std::runtime_error("network unavailable");
    }
    auto http = network->CreateHttp(1);
    if (!http) {
        throw std::runtime_error("failed to create HTTP client");
    }
    http->SetTimeout(2000);
    esp_task_wdt_reset();
    if (!http->Open("GET", url)) {
        throw std::runtime_error("failed to open URL: " + url);
    }
    ScopedHttpClose<Http> http_close(http.get());
    esp_task_wdt_reset();
    int status = http->GetStatusCode();
    if (status != 200) {
        esp_task_wdt_reset();
        throw std::runtime_error("unexpected status " + std::to_string(status) + " for " + url);
    }
    DownloadLessonAssetHttpBodyToFile(
        *http,
        cache_key,
        has_declared_size,
        declared_size,
        url,
        dest_path,
        bytes_out);
    return true;
}
}

McpServer::McpServer() {
}

McpServer::~McpServer() {
    for (auto tool : tools_) {
        delete tool;
    }
    tools_.clear();
}

void McpServer::AddCommonTools() {
    // *Important* To speed up the response time, we add the common tools to the beginning of
    // the tools list to utilize the prompt cache.
    // **重要** 为了提升响应速度，我们把常用的工具放在前面，利用 prompt cache 的特性。

    // Backup the original tools list and restore it after adding the common tools.
    auto original_tools = std::move(tools_);
    auto& board = Board::GetInstance();

    // Do not add custom tools here.
    // Custom tools must be added in the board's InitializeTools function.

    AddTool("self.get_device_status",
        "Provides the real-time information of the device, including the current status of the audio speaker, screen, battery, network, etc.\n"
        "Use this tool for: \n"
        "1. Answering questions about current condition (e.g. what is the current volume of the audio speaker?)\n"
        "2. As the first step to control the device (e.g. turn up / down the volume of the audio speaker, etc.)",
        PropertyList(),
        [&board](const PropertyList& properties) -> ReturnValue {
            return board.GetDeviceStatusJson();
        });

    AddTool("self.audio_speaker.set_volume", 
        "Set the volume of the audio speaker. If the current volume is unknown, you must call `self.get_device_status` tool first and then call this tool.",
        PropertyList({
            Property("volume", kPropertyTypeInteger, 0, 100)
        }), 
        [&board](const PropertyList& properties) -> ReturnValue {
            auto codec = board.GetAudioCodec();
            codec->SetOutputVolume(properties["volume"].value<int>());
            return true;
        });

    auto add_robot_arm_tool = [this](const std::string& name, const std::string& description,
                                    bool (Application::*handler)()) {
        AddTool(name, description, PropertyList(),
            [handler](const PropertyList& properties) -> ReturnValue {
                (void)properties;
                auto& app = Application::GetInstance();
                return (app.*handler)();
            });
    };
    add_robot_arm_tool("self.robot.left_arm_raise",
        "Raise the robot left arm by sending a UART command to the servant controller. Use when the user asks to raise the left hand or left arm, including Vietnamese phrases like nâng tay trái, giơ tay trái, or dơ tay trái.",
        &Application::SendLeftArmRaise);
    add_robot_arm_tool("self.robot.right_arm_raise",
        "Raise the robot right arm by sending a UART command to the servant controller. Use when the user asks to raise the right hand or right arm, including Vietnamese phrases like nâng tay phải, giơ tay phải, or dơ tay phải.",
        &Application::SendRightArmRaise);
    add_robot_arm_tool("self.robot.left_arm_lower",
        "Lower the robot left arm by sending a UART command to the servant controller. Use when the user asks to lower the left hand or left arm.",
        &Application::SendLeftArmLower);
    add_robot_arm_tool("self.robot.right_arm_lower",
        "Lower the robot right arm by sending a UART command to the servant controller. Use when the user asks to lower the right hand or right arm.",
        &Application::SendRightArmLower);
    add_robot_arm_tool("self.robot.both_arms_raise",
        "Raise both robot arms by sending UART commands to the servant controller. Use when the user asks to raise both hands or both arms, including Vietnamese phrases like nâng hai tay, giơ cả hai tay, or dơ cả hai tay.",
        &Application::SendBothArmsRaise);
    add_robot_arm_tool("self.robot.both_arms_lower",
        "Lower both robot arms by sending UART commands to the servant controller. Use when the user asks to lower both hands or both arms.",
        &Application::SendBothArmsLower);
    AddTool("self.robot.left_arm_set_percent",
        "Set the robot left arm position as a percentage by sending a UART command to the servant controller. Use when the user asks to nâng tay trái 50%, hạ tay trái 30%, đưa tay trái lên 75%, or set the left arm/hand to a percentage. 0% is fully lowered and 100% is fully raised.",
        PropertyList({
            Property("percent", kPropertyTypeInteger, 100, 0, 100)
        }),
        [](const PropertyList& properties) -> ReturnValue {
            auto& app = Application::GetInstance();
            return app.SendLeftArmSetPercent(properties["percent"].value<int>());
        });
    AddTool("self.robot.right_arm_set_percent",
        "Set the robot right arm position as a percentage by sending a UART command to the servant controller. Use when the user asks to nâng tay phải 50%, hạ tay phải 30%, đưa tay phải lên 75%, or set the right arm/hand to a percentage. 0% is fully lowered and 100% is fully raised.",
        PropertyList({
            Property("percent", kPropertyTypeInteger, 100, 0, 100)
        }),
        [](const PropertyList& properties) -> ReturnValue {
            auto& app = Application::GetInstance();
            return app.SendRightArmSetPercent(properties["percent"].value<int>());
        });
    AddTool("self.robot.both_arms_set_percent",
        "Set both robot arm positions as a percentage by sending UART commands to the servant controller. Use when the user asks to nâng hai tay 50%, hạ hai tay 30%, đưa cả hai tay lên 75%, or set both arms/hands to a percentage. 0% is fully lowered and 100% is fully raised.",
        PropertyList({
            Property("percent", kPropertyTypeInteger, 100, 0, 100)
        }),
        [](const PropertyList& properties) -> ReturnValue {
            auto& app = Application::GetInstance();
            return app.SendBothArmsSetPercent(properties["percent"].value<int>());
        });
    add_robot_arm_tool("self.robot.head_turn_left",
        "Turn the robot head left by sending a UART command to the servant controller. Use when the user asks to turn the head left, look left, or Vietnamese phrases like quay đầu trái, nhìn sang trái, or quay mặt sang trái.",
        &Application::SendHeadTurnLeft);
    add_robot_arm_tool("self.robot.head_turn_right",
        "Turn the robot head right by sending a UART command to the servant controller. Use when the user asks to turn the head right, look right, or Vietnamese phrases like quay đầu phải, nhìn sang phải, or quay mặt sang phải.",
        &Application::SendHeadTurnRight);
    add_robot_arm_tool("self.robot.head_center",
        "Center the robot head by sending a UART command to the servant controller. Use when the user asks to center the head, face forward, or Vietnamese phrases like đưa đầu về giữa, nhìn thẳng, or quay đầu về giữa.",
        &Application::SendHeadCenter);
    AddTool("self.robot.head_set_angle",
        "Set the robot head servo angle by sending a UART command to the servant controller. Use when the user asks to chỉnh góc quay đầu, quay đầu 120 độ, xoay đầu sang trái 45 độ, xoay đầu sang phải 135 độ, or any requested head angle. Angle is 0 to 180 degrees, with 90 degrees centered.",
        PropertyList({
            Property("angle", kPropertyTypeInteger, 90, 0, 180)
        }),
        [](const PropertyList& properties) -> ReturnValue {
            auto& app = Application::GetInstance();
            return app.SendHeadSetAngle(properties["angle"].value<int>());
        });
    AddTool("self.robot.head_set_percent",
        "Set the robot head position as a percentage by sending a UART command to the servant controller. Use when the user asks to quay đầu 50%, xoay đầu sang trái 50%, xoay đầu sang phải 75%, or set the head/face turn as a percentage. 0% is fully left, 50% is center, and 100% is fully right.",
        PropertyList({
            Property("percent", kPropertyTypeInteger, 50, 0, 100)
        }),
        [](const PropertyList& properties) -> ReturnValue {
            auto& app = Application::GetInstance();
            return app.SendHeadSetPercent(properties["percent"].value<int>());
        });
    
    auto backlight = board.GetBacklight();
    if (backlight) {
        AddTool("self.screen.set_brightness",
            "Set the brightness of the screen.",
            PropertyList({
                Property("brightness", kPropertyTypeInteger, 0, 100)
            }),
            [backlight](const PropertyList& properties) -> ReturnValue {
                uint8_t brightness = static_cast<uint8_t>(properties["brightness"].value<int>());
                backlight->SetBrightness(brightness, true);
                return true;
            });
    }

#ifdef HAVE_LVGL
    auto display = board.GetDisplay();
    if (display && display->GetTheme() != nullptr) {
        AddTool("self.screen.set_theme",
            "Set the theme of the screen. The theme can be `light` or `dark`.",
            PropertyList({
                Property("theme", kPropertyTypeString)
            }),
            [display](const PropertyList& properties) -> ReturnValue {
                auto theme_name = properties["theme"].value<std::string>();
                auto& theme_manager = LvglThemeManager::GetInstance();
                auto theme = theme_manager.GetTheme(theme_name);
                if (theme != nullptr) {
                    display->SetTheme(theme);
                    return true;
                }
                return false;
            });
    }

    auto camera = board.GetCamera();
    if (camera) {
        AddTool("self.camera.take_photo",
            "Always remember you have a camera. If the user asks you to see something, use this tool to take a photo and then explain it.\n"
            "Args:\n"
            "  `question`: The question that you want to ask about the photo.\n"
            "Return:\n"
            "  A JSON object that provides the photo information.",
            PropertyList({
                Property("question", kPropertyTypeString)
            }),
            [camera](const PropertyList& properties) -> ReturnValue {
                // Lower the priority to do the camera capture
                TaskPriorityReset priority_reset(1);

                if (!camera->Capture()) {
                    throw std::runtime_error("Failed to capture photo");
                }
                auto question = properties["question"].value<std::string>();
                return camera->Explain(question);
            });
    }
#endif

    // Restore the original tools list to the end of the tools list
    tools_.insert(tools_.end(), original_tools.begin(), original_tools.end());
}

void McpServer::AddUserOnlyTools() {
#if CONFIG_TBOT_HIL_STORAGE_FAULTS
    RegisterLessonStorageHilMcpTools(*this);
#endif
    // System tools
    AddUserOnlyTool("self.get_system_info",
        "Get the system information",
        PropertyList(),
        [this](const PropertyList& properties) -> ReturnValue {
            auto& board = Board::GetInstance();
            return board.GetSystemInfoJson();
        });

    AddUserOnlyTool("self.reboot", "Reboot the system",
        PropertyList(),
        [this](const PropertyList& properties) -> ReturnValue {
            auto& app = Application::GetInstance();
            app.Schedule([&app]() {
                ESP_LOGW(TAG, "User requested reboot");
                vTaskDelay(pdMS_TO_TICKS(1000));

                app.Reboot();
            });
            return true;
        });

    // Firmware upgrade
    AddUserOnlyTool("self.upgrade_firmware", "Upgrade firmware from a specific URL. This will download and install the firmware, then reboot the device.",
        PropertyList({
            Property("url", kPropertyTypeString, "The URL of the firmware binary file to download and install")
        }),
        [this](const PropertyList& properties) -> ReturnValue {
            auto url = properties["url"].value<std::string>();
            ESP_LOGI(TAG, "User requested firmware upgrade from URL: %s", url.c_str());
            
            auto& app = Application::GetInstance();
            app.Schedule([url, &app]() {
                bool success = app.UpgradeFirmware(url);
                if (!success) {
                    ESP_LOGE(TAG, "Firmware upgrade failed");
                }
            });
            
            return true;
        });

    // Display control
#ifdef HAVE_LVGL
    auto display = dynamic_cast<LvglDisplay*>(Board::GetInstance().GetDisplay());
    if (display) {
        AddUserOnlyTool("self.screen.get_info", "Information about the screen, including width, height, etc.",
            PropertyList(),
            [display](const PropertyList& properties) -> ReturnValue {
                cJSON *json = cJSON_CreateObject();
                cJSON_AddNumberToObject(json, "width", display->width());
                cJSON_AddNumberToObject(json, "height", display->height());
                if (dynamic_cast<OledDisplay*>(display)) {
                    cJSON_AddBoolToObject(json, "monochrome", true);
                } else {
                    cJSON_AddBoolToObject(json, "monochrome", false);
                }
                return json;
            });

#if CONFIG_LV_USE_SNAPSHOT
        AddUserOnlyTool("self.screen.snapshot", "Snapshot the screen and upload it to a specific URL",
            PropertyList({
                Property("url", kPropertyTypeString),
                Property("quality", kPropertyTypeInteger, 80, 1, 100)
            }),
            [display](const PropertyList& properties) -> ReturnValue {
                auto url = properties["url"].value<std::string>();
                auto quality = properties["quality"].value<int>();
                if (Application::GetInstance().IsLessonRuntimeActive()) {
                    throw std::runtime_error("screen snapshot disabled during lesson");
                }

                std::string jpeg_data;
                if (!display->SnapshotToJpeg(jpeg_data, quality)) {
                    throw std::runtime_error("Failed to snapshot screen");
                }

                ESP_LOGI(TAG, "Upload snapshot %u bytes to %s", jpeg_data.size(), url.c_str());
                
                // 构造multipart/form-data请求体
                std::string boundary = "----ESP32_SCREEN_SNAPSHOT_BOUNDARY";
                
                auto http = Board::GetInstance().GetNetwork()->CreateHttp(3);
                http->SetHeader("Content-Type", "multipart/form-data; boundary=" + boundary);
                if (!http->Open("POST", url)) {
                    throw std::runtime_error("Failed to open URL: " + url);
                }
                {
                    // 文件字段头部
                    std::string file_header;
                    file_header += "--" + boundary + "\r\n";
                    file_header += "Content-Disposition: form-data; name=\"file\"; filename=\"screenshot.jpg\"\r\n";
                    file_header += "Content-Type: image/jpeg\r\n";
                    file_header += "\r\n";
                    http->Write(file_header.c_str(), file_header.size());
                }

                // JPEG数据
                http->Write((const char*)jpeg_data.data(), jpeg_data.size());

                {
                    // multipart尾部
                    std::string multipart_footer;
                    multipart_footer += "\r\n--" + boundary + "--\r\n";
                    http->Write(multipart_footer.c_str(), multipart_footer.size());
                }
                http->Write("", 0);

                if (http->GetStatusCode() != 200) {
                    throw std::runtime_error("Unexpected status code: " + std::to_string(http->GetStatusCode()));
                }
                std::string result = http->ReadAll();
                http->Close();
                ESP_LOGI(TAG, "Snapshot screen result: %s", result.c_str());
                return true;
            });
        
        AddUserOnlyTool("self.screen.preview_image", "Preview an image on the screen",
            PropertyList({
                Property("url", kPropertyTypeString)
            }),
            [display](const PropertyList& properties) -> ReturnValue {
                auto url = properties["url"].value<std::string>();
                auto http = Board::GetInstance().GetNetwork()->CreateHttp(3);

                if (!http->Open("GET", url)) {
                    throw std::runtime_error("Failed to open URL: " + url);
                }
                int status_code = http->GetStatusCode();
                if (status_code != 200) {
                    throw std::runtime_error("Unexpected status code: " + std::to_string(status_code));
                }

                // The custom HttpClient reports GetBodyLength()==0 for ANY
                // Transfer-Encoding: chunked response (http_client.cc:709-710) — the
                // length is unknown up front, not necessarily empty. Two cases:
                //   content_length > 0  -> fixed Content-Length; read exactly that many.
                //   content_length == 0 -> chunked OR genuinely empty; the chunked parser
                //                          still buffers the de-chunked bytes, so read
                //                          until Read() returns 0 (EOF) into a growing,
                //                          capped buffer. (Same gap as lesson_handler.cc's
                //                          FetchLessonImage.)
                size_t content_length = http->GetBodyLength();
                char* data = nullptr;

                if (content_length > 0) {
                    // Fast path: server advertised a fixed length.
                    data = (char*)heap_caps_malloc(content_length, MALLOC_CAP_8BIT);
                    if (data == nullptr) {
                        throw std::runtime_error("Failed to allocate memory for image: " + url);
                    }
                    size_t total_read = 0;
                    while (total_read < content_length) {
                        int ret = http->Read(data + total_read, content_length - total_read);
                        if (ret < 0) {
                            heap_caps_free(data);
                            throw std::runtime_error("Failed to download image: " + url);
                        }
                        if (ret == 0) {
                            break;  // server closed early
                        }
                        total_read += ret;
                    }
                    http->Close();
                    if (total_read < content_length) {
                        // Truncated download: the tail of `data` is uninitialized, so
                        // decoding it would feed garbage to LVGL. Reject instead of
                        // rendering a corrupt image.
                        heap_caps_free(data);
                        throw std::runtime_error("Short read for image: " + url);
                    }
                } else {
                    // Chunked / unknown-length 200: grow a buffer until EOF. The cap is a
                    // heap backstop against a runaway/oversized stream; on a no-PSRAM
                    // board the realloc failure below fires long before it.
                    constexpr size_t kMaxImageBytes = 4 * 1024 * 1024;
                    size_t capacity = 32 * 1024;
                    data = (char*)heap_caps_malloc(capacity, MALLOC_CAP_8BIT);
                    if (data == nullptr) {
                        throw std::runtime_error("Failed to allocate memory for image: " + url);
                    }
                    size_t total_read = 0;
                    while (true) {
                        if (total_read == capacity) {
                            if (capacity >= kMaxImageBytes) {
                                heap_caps_free(data);
                                throw std::runtime_error("Image exceeds max size: " + url);
                            }
                            size_t new_capacity = std::min(capacity * 2, kMaxImageBytes);
                            char* grown = (char*)heap_caps_realloc(data, new_capacity, MALLOC_CAP_8BIT);
                            if (grown == nullptr) {
                                heap_caps_free(data);
                                throw std::runtime_error("Failed to grow image buffer: " + url);
                            }
                            data = grown;
                            capacity = new_capacity;
                        }
                        int ret = http->Read(data + total_read, capacity - total_read);
                        if (ret < 0) {
                            heap_caps_free(data);
                            throw std::runtime_error("Failed to download image: " + url);
                        }
                        if (ret == 0) {
                            break;  // EOF
                        }
                        total_read += ret;
                    }
                    http->Close();
                    if (total_read == 0) {
                        heap_caps_free(data);
                        throw std::runtime_error("Empty image body: " + url);
                    }
                    // Shrink the over-allocated buffer to the exact payload so the cached
                    // image doesn't hold spare capacity for its whole lifetime; keep the
                    // original block if the shrink can't be satisfied in place.
                    char* shrunk = (char*)heap_caps_realloc(data, total_read, MALLOC_CAP_8BIT);
                    if (shrunk != nullptr) {
                        data = shrunk;
                    }
                    content_length = total_read;
                }

                // LvglAllocatedImage takes ownership of `data` and frees it in its dtor.
                // Its ctor decodes the header and THROWS on undecodable bytes; on that
                // path the dtor never runs, so free `data` ourselves before rethrowing.
                std::unique_ptr<LvglAllocatedImage> image;
                try {
                    image = std::make_unique<LvglAllocatedImage>(data, content_length);
                } catch (...) {
                    heap_caps_free(data);
                    throw;
                }
                display->SetPreviewImage(std::move(image));
                return true;
            });
#endif // CONFIG_LV_USE_SNAPSHOT
    }
#endif // HAVE_LVGL

    AddUserOnlyTool("self.lesson_assets.evict_cache_key",
        "Evict one exact lesson asset cache key from the SD card.",
        PropertyList({
            Property("cacheKey", kPropertyTypeString)
        }),
        [](const PropertyList& properties) -> ReturnValue {
            const std::string cache_key =
                properties["cacheKey"].value<std::string>();
            const auto result = EvictLessonAssetCacheKey(cache_key, false);
            auto json = MakeCheckedCJsonObject();
            CheckedCJsonAddStringToObject(
                json.get(), "cacheKey", result.cache_key.c_str());
            CheckedCJsonAddStringToObject(
                json.get(), "status", LessonAssetCacheEvictCodeName(result.code));
            CheckedCJsonAddBoolToObject(json.get(), "evicted", result.evicted);
            CheckedCJsonAddBoolToObject(json.get(), "notFound", result.not_found);
            CheckedCJsonAddNumberToObject(json.get(), "fileCount", result.file_count);
            CheckedCJsonAddStringToObject(
                json.get(), "reason", LessonAssetCacheEvictCodeName(result.code));
            return json.release();
        });

    // Assets download url (always registered — Settings storage works regardless of partition layout)
    AddUserOnlyTool("self.lesson_assets.sync_sample_to_sd",
        "Download the built-in sample lesson images to the SD card for offline lesson rendering.",
        PropertyList({
            Property("base_url", kPropertyTypeString, "HTTP base URL containing the sample image files")
        }),
        [](const PropertyList& properties) -> ReturnValue {
            std::string base_url = properties["base_url"].value<std::string>();
            while (!base_url.empty() && base_url.back() == '/') {
                base_url.pop_back();
            }
            if (!IsAllowedSampleLessonAssetBaseUrl(base_url)) {
                throw std::runtime_error("base_url must be a valid HTTP(S) origin");
            }
            auto mutation =
                LessonAssetStorageCoordinator::GetInstance().TryBeginMutation("sync");
            if (!mutation) {
                ThrowLessonAssetMutationRefusal(mutation.code());
            }
            EnsureSampleLessonAssetDir(mutation);

            auto json = MakeCheckedCJsonObject();
            CheckedCJsonAddStringToObject(json.get(), "directory", kSampleLessonAssetDir);
            auto files = MakeCheckedCJsonArray();
            int downloaded = 0;
            size_t total_bytes = 0;

            for (const auto& asset : kSampleLessonAssets) {
                std::string url = base_url + "/" + asset.file;
                std::string dest = std::string(kSampleLessonAssetDir) + "/" + asset.file;
                size_t bytes = 0;
                try {
                    DownloadLessonAssetToVerifiedFile(
                        mutation, nullptr, false, 0, url, dest, asset.sha256, bytes);
                } catch (...) {
                    throw std::runtime_error("lesson asset transfer failed");
                }
                total_bytes += bytes;
                downloaded += 1;
                auto item = MakeCheckedCJsonObject();
                CheckedCJsonAddStringToObject(item.get(), "file", asset.file);
                CheckedCJsonAddNumberToObject(
                    item.get(), "bytes", static_cast<double>(bytes));
                CheckedCJsonAddStringToObject(item.get(), "sha256", asset.sha256);
                CheckedCJsonAddItemToArray(files.get(), std::move(item));
                ESP_LOGI(TAG, "sample lesson asset synced to SD: %s bytes=%u",
                         dest.c_str(), static_cast<unsigned>(bytes));
            }

            CheckedCJsonAddNumberToObject(json.get(), "downloadedCount", downloaded);
            CheckedCJsonAddNumberToObject(
                json.get(), "totalBytes", static_cast<double>(total_bytes));
            CheckedCJsonAddItemToObject(json.get(), "files", std::move(files));
            return json.release();
        });

    AddUserOnlyTool("self.lesson_assets.sync_to_sd",
        "Download a lesson assetPack to the SD card and verify each asset by sha256.",
        PropertyList({
            Property("assetPack", kPropertyTypeObject)
        }),
        [](const PropertyList& properties) -> ReturnValue {
            std::string asset_pack_json = properties["assetPack"].value<std::string>();
            std::unique_ptr<cJSON, decltype(&cJSON_Delete)> pack(
                cJSON_Parse(asset_pack_json.c_str()), cJSON_Delete);
            if (!cJSON_IsObject(pack.get())) {
                throw std::runtime_error("assetPack object is required");
            }
            const char* cache_key = nullptr;
            const auto validated_assets =
                ValidateLessonAssetSyncPackOrThrow(pack.get(), cache_key);
            auto mutation =
                LessonAssetStorageCoordinator::GetInstance().TryBeginMutation("sync");
            if (!mutation) {
                ThrowLessonAssetMutationRefusal(mutation.code());
            }

            auto json = MakeCheckedCJsonObject();
            CheckedCJsonAddStringToObject(json.get(), "cacheKey", cache_key);
            const char* manifest_checksum = JsonStringField(pack.get(), "manifestChecksum");
            auto files = MakeCheckedCJsonArray();
            int downloaded = 0;
            int skipped = 0;
            int failed = 0;
            int verified = 0;
            size_t total_bytes = 0;
            const int asset_count = static_cast<int>(validated_assets.size());

            for (const auto& asset : validated_assets) {
                auto item = MakeCheckedCJsonObject();
                CheckedCJsonAddStringToObject(item.get(), "key", asset.key);
                CheckedCJsonAddStringToObject(item.get(), "path", asset.path);

                try {
                    CheckedCJsonAddStringToObject(
                        item.get(), "localPath", asset.destination);
                    if (VerifyLessonAssetSha256(asset.destination, asset.sha256)) {
                        skipped += 1;
                        CheckedCJsonAddStringToObject(item.get(), "state", "SKIPPED");
                    } else {
                        size_t bytes = 0;
                        DownloadLessonAssetToVerifiedFile(
                            mutation,
                            cache_key,
                            asset.has_declared_size,
                            asset.declared_size,
                            asset.url,
                            asset.destination,
                            asset.sha256,
                            bytes);
                        total_bytes += bytes;
                        downloaded += 1;
                        CheckedCJsonAddStringToObject(
                            item.get(), "state", "DOWNLOADED");
                        CheckedCJsonAddNumberToObject(
                            item.get(), "bytes", static_cast<double>(bytes));
                    }
                    verified += 1;
                } catch (const std::runtime_error& error) {
                    if (std::strcmp(error.what(), kMcpResponseAllocationError) == 0) {
                        throw;
                    }
                    failed += 1;
                    CheckedCJsonAddStringToObject(item.get(), "state", "FAILED");
                    CheckedCJsonAddStringToObject(
                        item.get(), "error", "asset transfer failed");
                } catch (...) {
                    failed += 1;
                    CheckedCJsonAddStringToObject(item.get(), "state", "FAILED");
                    CheckedCJsonAddStringToObject(
                        item.get(), "error", "asset transfer failed");
                }
                CheckedCJsonAddItemToArray(files.get(), std::move(item));
            }

            AddLessonAssetSyncAttestation(
                json.get(), cache_key, manifest_checksum, asset_count, verified, failed);
            CheckedCJsonAddNumberToObject(json.get(), "downloadedCount", downloaded);
            CheckedCJsonAddNumberToObject(json.get(), "skippedCount", skipped);
            CheckedCJsonAddNumberToObject(json.get(), "failedCount", failed);
            CheckedCJsonAddNumberToObject(
                json.get(), "totalBytes", static_cast<double>(total_bytes));
            CheckedCJsonAddItemToObject(json.get(), "files", std::move(files));
            return json.release();
        });

    AddUserOnlyTool("self.assets.set_download_url", "Set the download url for the assets",
            PropertyList({
                Property("url", kPropertyTypeString)
            }),
            [](const PropertyList& properties) -> ReturnValue {
                auto url = properties["url"].value<std::string>();
                Settings settings("assets", true);
                settings.SetString("download_url", url);
                return true;
            });
}

void McpServer::AddTool(McpTool* tool) {
    // Prevent adding duplicate tools
    if (std::find_if(tools_.begin(), tools_.end(), [tool](const McpTool* t) { return t->name() == tool->name(); }) != tools_.end()) {
        ESP_LOGW(TAG, "Tool %s already added", tool->name().c_str());
        return;
    }

    ESP_LOGI(TAG, "Add tool: %s%s", tool->name().c_str(), tool->user_only() ? " [user]" : "");
    tools_.push_back(tool);
}

void McpServer::AddTool(const std::string& name, const std::string& description, const PropertyList& properties, std::function<ReturnValue(const PropertyList&)> callback) {
    AddTool(new McpTool(name, description, properties, callback));
}

void McpServer::AddUserOnlyTool(const std::string& name, const std::string& description, const PropertyList& properties, std::function<ReturnValue(const PropertyList&)> callback) {
    auto tool = new McpTool(name, description, properties, callback);
    tool->set_user_only(true);
    AddTool(tool);
}

void McpServer::AddUserOnlyTool(
    const std::string& name,
    const std::string& description,
    const PropertyList& properties,
    PreparedMcpCall mode,
    std::function<std::string(const PropertyList&)> callback
) {
    auto tool = new McpTool(name, description, properties, mode, std::move(callback));
    tool->set_user_only(true);
    AddTool(tool);
}

void McpServer::ParseMessage(const std::string& message) {
    cJSON* json = cJSON_Parse(message.c_str());
    if (json == nullptr) {
        ESP_LOGE(TAG, "Failed to parse MCP message: %s", message.c_str());
        return;
    }
    ParseMessage(json);
    cJSON_Delete(json);
}

void McpServer::ParseCapabilities(const cJSON* capabilities) {
    auto vision = cJSON_GetObjectItem(capabilities, "vision");
    if (cJSON_IsObject(vision)) {
        auto url = cJSON_GetObjectItem(vision, "url");
        auto token = cJSON_GetObjectItem(vision, "token");
        if (cJSON_IsString(url)) {
            auto camera = Board::GetInstance().GetCamera();
            if (camera) {
                std::string url_str = std::string(url->valuestring);
                std::string token_str;
                if (cJSON_IsString(token)) {
                    token_str = std::string(token->valuestring);
                }
                camera->SetExplainUrl(url_str, token_str);
            }
        }
    }
}

void McpServer::ParseMessage(const cJSON* json) {
    // Check JSONRPC version
    auto version = cJSON_GetObjectItem(json, "jsonrpc");
    if (version == nullptr || !cJSON_IsString(version) || strcmp(version->valuestring, "2.0") != 0) {
        ESP_LOGE(TAG, "Invalid JSONRPC version: %s", version ? version->valuestring : "null");
        return;
    }
    
    // Check method
    auto method = cJSON_GetObjectItem(json, "method");
    if (method == nullptr || !cJSON_IsString(method)) {
        ESP_LOGE(TAG, "Missing method");
        return;
    }
    
    auto method_str = std::string(method->valuestring);
    if (method_str.find("notifications") == 0) {
        return;
    }
    
    // Check params
    auto params = cJSON_GetObjectItem(json, "params");
    if (params != nullptr && !cJSON_IsObject(params)) {
        ESP_LOGE(TAG, "Invalid params for method: %s", method_str.c_str());
        return;
    }

    auto id = cJSON_GetObjectItem(json, "id");
    if (id == nullptr || !cJSON_IsNumber(id)) {
        ESP_LOGE(TAG, "Invalid id for method: %s", method_str.c_str());
        return;
    }
    auto id_int = id->valueint;
    
    if (method_str == "initialize") {
        if (cJSON_IsObject(params)) {
            auto capabilities = cJSON_GetObjectItem(params, "capabilities");
            if (cJSON_IsObject(capabilities)) {
                ParseCapabilities(capabilities);
            }
        }
        auto app_desc = esp_app_get_description();
        std::string message = "{\"protocolVersion\":\"2024-11-05\",\"capabilities\":{\"tools\":{}},\"serverInfo\":{\"name\":\"" BOARD_NAME "\",\"version\":\"";
        message += app_desc->version;
        message += "\"}}";
        ReplyResult(id_int, message);
    } else if (method_str == "tools/list") {
        std::string cursor_str = "";
        bool list_user_only_tools = false;
        if (params != nullptr) {
            auto cursor = cJSON_GetObjectItem(params, "cursor");
            if (cJSON_IsString(cursor)) {
                cursor_str = std::string(cursor->valuestring);
            }
            auto with_user_tools = cJSON_GetObjectItem(params, "withUserTools");
            if (cJSON_IsBool(with_user_tools)) {
                list_user_only_tools = with_user_tools->valueint == 1;
            }
        }
        GetToolsList(id_int, cursor_str, list_user_only_tools);
    } else if (method_str == "tools/call") {
        if (!cJSON_IsObject(params)) {
            ESP_LOGE(TAG, "tools/call: Missing params");
            ReplyError(id_int, "Missing params");
            return;
        }
        auto tool_name = cJSON_GetObjectItem(params, "name");
        if (!cJSON_IsString(tool_name)) {
            ESP_LOGE(TAG, "tools/call: Missing name");
            ReplyError(id_int, "Missing name");
            return;
        }
        auto tool_arguments = cJSON_GetObjectItem(params, "arguments");
        if (tool_arguments != nullptr && !cJSON_IsObject(tool_arguments)) {
            ESP_LOGE(TAG, "tools/call: Invalid arguments");
            ReplyError(id_int, "Invalid arguments");
            return;
        }
        DoToolCall(id_int, std::string(tool_name->valuestring), tool_arguments);
    } else {
        ESP_LOGE(TAG, "Method not implemented: %s", method_str.c_str());
        ReplyError(id_int, "Method not implemented: " + method_str);
    }
}

void McpServer::ReplyResult(int id, const std::string& result) {
    std::string payload = "{\"jsonrpc\":\"2.0\",\"id\":";
    payload += std::to_string(id) + ",\"result\":";
    payload += result;
    payload += "}";
    Application::GetInstance().SendMcpMessage(payload);
}

void McpServer::ReplyError(int id, const std::string& message) {
    std::string payload = "{\"jsonrpc\":\"2.0\",\"id\":";
    payload += std::to_string(id);
    payload += ",\"error\":{\"message\":\"";
    payload += message;
    payload += "\"}}";
    Application::GetInstance().SendMcpMessage(payload);
}

void McpServer::GetToolsList(int id, const std::string& cursor, bool list_user_only_tools) {
    std::string json = "{\"tools\":[";
    
    bool found_cursor = cursor.empty();
    auto it = tools_.begin();
    std::string next_cursor = "";
    
    while (it != tools_.end()) {
        // 如果我们还没有找到起始位置，继续搜索
        if (!found_cursor) {
            if ((*it)->name() == cursor) {
                found_cursor = true;
            } else {
                ++it;
                continue;
            }
        }

        if (!list_user_only_tools && (*it)->user_only()) {
            ++it;
            continue;
        }
        
        // 添加tool前检查大小
        std::string tool_json = (*it)->to_json() + ",";
        if (json.length() + tool_json.length() + 30 > kMcpToolsListMaxPayloadBytes) {
            // 如果添加这个tool会超出大小限制，设置next_cursor并退出循环
            next_cursor = (*it)->name();
            break;
        }
        
        json += tool_json;
        ++it;
    }
    
    if (json.back() == ',') {
        json.pop_back();
    }
    
    if (json.back() == '[' && !tools_.empty()) {
        // 如果没有添加任何tool，返回错误
        ESP_LOGE(TAG, "tools/list: Failed to add tool %s because of payload size limit", next_cursor.c_str());
        ReplyError(id, "Failed to add tool " + next_cursor + " because of payload size limit");
        return;
    }

    if (next_cursor.empty()) {
        json += "]}";
    } else {
        json += "],\"nextCursor\":\"" + next_cursor + "\"}";
    }

    ESP_LOGI(TAG, "tools/list page bytes=%u next_cursor=%s", (unsigned)json.size(),
             next_cursor.empty() ? "(none)" : next_cursor.c_str());
    
    ReplyResult(id, json);
}

void McpServer::DoToolCall(int id, const std::string& tool_name, const cJSON* tool_arguments) {
    auto tool_iter = std::find_if(tools_.begin(), tools_.end(), 
                                 [&tool_name](const McpTool* tool) { 
                                     return tool->name() == tool_name; 
                                 });
    
    if (tool_iter == tools_.end()) {
        ESP_LOGE(TAG, "tools/call: Unknown tool: %s", tool_name.c_str());
        ReplyError(id, "Unknown tool: " + tool_name);
        return;
    }

#if CONFIG_TBOT_HIL_STORAGE_FAULTS
    const bool is_lesson_storage_hil = IsExactLessonStorageHilToolName(tool_name);
    if (is_lesson_storage_hil) {
        const char* validation_error =
            ValidateLessonStorageHilRawArguments(tool_name, tool_arguments);
        if (validation_error != nullptr) {
            const auto sequence = LessonStorageHilController::GetInstance()
                                      .NextEvidenceSequence();
            ESP_LOGW(TAG,
                     "HIL_STORAGE_REFUSAL tool=%s reason=invalid_request sequence=%llu",
                     tool_name.c_str(),
                     static_cast<unsigned long long>(sequence));
            ReplyError(id, validation_error);
            return;
        }
    }
#endif

    const bool is_lesson_cache_evict =
        tool_name == "self.lesson_assets.evict_cache_key";
    if (!is_lesson_cache_evict &&
        Application::GetInstance().IsLessonRuntimeActive()) {
        ESP_LOGI(TAG, "MCP tool call rejected during lesson: %s", tool_name.c_str());
        ReplyError(id, "MCP tools disabled during lesson");
        return;
    }

    PropertyList arguments = (*tool_iter)->properties();
    try {
        for (auto& argument : arguments) {
            bool found = false;
            if (cJSON_IsObject(tool_arguments)) {
                auto value = cJSON_GetObjectItem(tool_arguments, argument.name().c_str());
                if (argument.type() == kPropertyTypeBoolean && cJSON_IsBool(value)) {
                    argument.set_value<bool>(value->valueint == 1);
                    found = true;
                } else if (argument.type() == kPropertyTypeInteger && cJSON_IsNumber(value)) {
                    argument.set_value<int>(value->valueint);
                    found = true;
                } else if (argument.type() == kPropertyTypeString && cJSON_IsString(value)) {
                    argument.set_value<std::string>(value->valuestring);
                    found = true;
                } else if (argument.type() == kPropertyTypeObject && cJSON_IsObject(value)) {
                    argument.set_value<std::string>(CheckedCJsonPrint(value));
                    found = true;
                }
            }

            if (!argument.has_default_value() && !found) {
                ESP_LOGE(TAG, "tools/call: Missing valid argument: %s", argument.name().c_str());
                ReplyError(id, "Missing valid argument: " + argument.name());
                return;
            }
        }
    } catch (const std::exception& e) {
#if CONFIG_TBOT_HIL_STORAGE_FAULTS
        if (is_lesson_storage_hil) {
            ESP_LOGW(TAG, "HIL_STORAGE_REFUSAL reason=argument_conversion");
            ReplyError(id, "lesson storage HIL operation failed");
            return;
        }
#endif
        ESP_LOGE(TAG, "tools/call: %s", e.what());
        ReplyError(id, e.what());
        return;
    }

    // Use main thread to call the tool
    auto& app = Application::GetInstance();
    app.Schedule([this, id, tool_iter, tool_name, is_lesson_cache_evict,
#if CONFIG_TBOT_HIL_STORAGE_FAULTS
                  is_lesson_storage_hil,
#endif
                  arguments = std::move(arguments)]() {
        if (!is_lesson_cache_evict &&
            Application::GetInstance().IsLessonRuntimeActive()) {
            ESP_LOGI(TAG, "scheduled MCP tool call rejected during lesson: %s", tool_name.c_str());
            ReplyError(id, "MCP tools disabled during lesson");
            return;
        }
        try {
            ReplyResult(id, (*tool_iter)->Call(arguments));
        } catch (const std::exception& e) {
#if CONFIG_TBOT_HIL_STORAGE_FAULTS
            if (is_lesson_storage_hil) {
                const auto sequence = LessonStorageHilController::GetInstance()
                                          .NextEvidenceSequence();
                ESP_LOGW(TAG,
                         "HIL_STORAGE_REFUSAL reason=operation_failed sequence=%llu",
                         static_cast<unsigned long long>(sequence));
                ReplyError(id, "lesson storage HIL operation failed");
                return;
            }
#endif
            ESP_LOGE(TAG, "tools/call: %s", e.what());
            ReplyError(id, e.what());
        }
    });
}
