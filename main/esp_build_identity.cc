#include "esp_build_identity.h"

#include <atomic>
#include <cctype>
#include <cstdio>

#ifndef TBOT_BUILD_IDENTITY_HOST_TEST
#include <esp_app_desc.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>
#endif

#ifdef TBOT_HIL_PROFILE
#error "TBOT_HIL_PROFILE is derived from CONFIG_TBOT_HIL_STORAGE_FAULTS"
#endif

#if defined(CONFIG_TBOT_COURSE_MODE_LOCAL_ENDPOINT) && CONFIG_TBOT_COURSE_MODE_LOCAL_ENDPOINT
#define TBOT_EMBEDDED_PROFILE "course-mode-task07-local-endpoint"
#else
#if defined(CONFIG_TBOT_HIL_STORAGE_FAULTS) && CONFIG_TBOT_HIL_STORAGE_FAULTS
#define TBOT_EMBEDDED_PROFILE "task14-hil-v1"
#else
#define TBOT_EMBEDDED_PROFILE "production"
#endif
#endif

const char kTbotEmbeddedProfileAudit[] =
    "TBOT_EMBEDDED_PROFILE=" TBOT_EMBEDDED_PROFILE;

namespace {

bool SetError(std::string* error, const char* value) {
    if (error != nullptr) {
        *error = value;
    }
    return false;
}

bool IsLowerSha256(const std::string& value) {
    if (value.size() != 64) return false;
    for (char ch : value) {
        if (!std::isdigit(static_cast<unsigned char>(ch)) && (ch < 'a' || ch > 'f')) return false;
    }
    return true;
}

bool IsSafeValue(const std::string& value) {
    if (value.empty() || value.size() > 64) return false;
    for (char ch : value) {
        const unsigned char byte = static_cast<unsigned char>(ch);
        if (!std::isalnum(byte) && ch != '.' && ch != '_' && ch != '+' && ch != '-') return false;
    }
    return true;
}

#ifndef TBOT_BUILD_IDENTITY_HOST_TEST
std::string Hex(const uint8_t* bytes, size_t size) {
    std::string rendered(size * 2, '0');
    for (size_t index = 0; index < size; ++index) {
        std::snprintf(&rendered[index * 2], 3, "%02x", bytes[index]);
    }
    return rendered;
}

EspBuildIdentity cached_running_identity;
std::string cached_running_identity_error;
std::atomic<bool> running_identity_preloaded{false};
bool running_identity_valid = false;

bool LoadRunningEspBuildIdentity(EspBuildIdentity* identity, std::string* error) {
    const esp_app_desc_t* descriptor = esp_app_get_description();
    const esp_partition_t* partition = esp_ota_get_running_partition();
    if (descriptor == nullptr || partition == nullptr) return SetError(error, "running_image_unavailable");
    uint8_t app_sha256[32];
    if (esp_partition_get_sha256(partition, app_sha256) != ESP_OK) {
        return SetError(error, "app_sha256_unavailable");
    }
    EspBuildIdentity value;
    value.hil_profile = kTbotEmbeddedProfileAudit + sizeof("TBOT_EMBEDDED_PROFILE=") - 1;
    value.project_name = descriptor->project_name;
    value.project_version = descriptor->version;
    value.idf_version = descriptor->idf_ver;
    value.secure_version = descriptor->secure_version;
    value.elf_sha256 = Hex(descriptor->app_elf_sha256, sizeof(descriptor->app_elf_sha256));
    value.app_sha256 = Hex(app_sha256, sizeof(app_sha256));
    value.build_id = EspBuildId(value);
    if (!ValidateEspBuildIdentity(value, error)) return false;
    *identity = std::move(value);
    return true;
}
#endif

}  // namespace

std::string EspBuildId(const EspBuildIdentity& identity) {
    return "tbot-esp-v1:" + identity.elf_sha256;
}

bool ValidateEspBuildIdentity(const EspBuildIdentity& identity, std::string* error) {
    if (identity.schema_version != 1) return SetError(error, "unsupported_schema");
    if (!IsSafeValue(identity.hil_profile) || !IsSafeValue(identity.project_name) ||
        !IsSafeValue(identity.project_version) || !IsSafeValue(identity.idf_version)) {
        return SetError(error, "unsafe_text_field");
    }
    if (!IsLowerSha256(identity.elf_sha256) || !IsLowerSha256(identity.app_sha256)) {
        return SetError(error, "invalid_sha256");
    }
    if (identity.build_id != EspBuildId(identity)) return SetError(error, "build_id_mismatch");
    return true;
}

std::vector<std::pair<std::string, std::string>> EspBuildIdentityHeaders(
    const EspBuildIdentity& identity) {
    return {
        {"x-tbot-build-schema", std::to_string(identity.schema_version)},
        {"x-tbot-hil-profile", identity.hil_profile},
        {"x-tbot-project-name", identity.project_name},
        {"x-tbot-project-version", identity.project_version},
        {"x-tbot-idf-version", identity.idf_version},
        {"x-tbot-secure-version", std::to_string(identity.secure_version)},
        {"x-tbot-elf-sha256", identity.elf_sha256},
        {"x-tbot-app-sha256", identity.app_sha256},
        {"x-tbot-build-id", identity.build_id},
    };
}

bool PreloadRunningEspBuildIdentity(std::string* error) {
#ifdef TBOT_BUILD_IDENTITY_HOST_TEST
    return SetError(error, "host_test_has_no_running_partition");
#else
    if (running_identity_preloaded.load(std::memory_order_acquire)) {
        if (!running_identity_valid) {
            return SetError(error, cached_running_identity_error.c_str());
        }
        return true;
    }
    running_identity_valid = LoadRunningEspBuildIdentity(
        &cached_running_identity, &cached_running_identity_error);
    running_identity_preloaded.store(true, std::memory_order_release);
    if (!running_identity_valid) {
        return SetError(error, cached_running_identity_error.c_str());
    }
    return true;
#endif
}

bool ReadRunningEspBuildIdentity(EspBuildIdentity* identity, std::string* error) {
#ifdef TBOT_BUILD_IDENTITY_HOST_TEST
    (void)identity;
    return SetError(error, "host_test_has_no_running_partition");
#else
    if (identity == nullptr) return SetError(error, "null_output");
    if (!running_identity_preloaded.load(std::memory_order_acquire)) {
        return SetError(error, "build_identity_not_preloaded");
    }
    if (!running_identity_valid) {
        return SetError(error, cached_running_identity_error.c_str());
    }
    *identity = cached_running_identity;
    return true;
#endif
}
