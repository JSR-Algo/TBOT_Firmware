#pragma once

#include <string>
#include <utility>
#include <vector>

struct EspBuildIdentity {
    int schema_version = 1;
    std::string hil_profile;
    std::string project_name;
    std::string project_version;
    std::string idf_version;
    unsigned int secure_version = 0;
    std::string elf_sha256;
    std::string app_sha256;
    std::string build_id;
};

extern const char kTbotEmbeddedProfileAudit[];

std::string EspBuildId(const EspBuildIdentity& identity);
bool ValidateEspBuildIdentity(const EspBuildIdentity& identity, std::string* error = nullptr);
std::vector<std::pair<std::string, std::string>> EspBuildIdentityHeaders(
    const EspBuildIdentity& identity);
bool PreloadRunningEspBuildIdentity(std::string* error = nullptr);
bool ReadRunningEspBuildIdentity(EspBuildIdentity* identity, std::string* error = nullptr);
