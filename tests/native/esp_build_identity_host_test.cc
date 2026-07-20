#include "esp_build_identity.h"

#include <cstdlib>
#include <iostream>
#include <map>
#include <string>

namespace {

void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << "\n";
        std::exit(1);
    }
}

EspBuildIdentity valid_identity() {
    EspBuildIdentity value;
    value.schema_version = 1;
    value.hil_profile = "task14-hil-v1";
    value.project_name = "xiaozhi";
    value.project_version = "2.2.75";
    value.idf_version = "v5.4.1";
    value.secure_version = 0;
    value.elf_sha256 = std::string(64, 'a');
    value.app_sha256 = std::string(64, 'b');
    value.build_id = "tbot-esp-v1:" + value.elf_sha256;
    return value;
}

}  // namespace

int main() {
    auto value = valid_identity();
    std::string error;
    require(ValidateEspBuildIdentity(value, &error), "valid identity accepted");

    const auto headers = EspBuildIdentityHeaders(value);
    std::map<std::string, std::string> by_name(headers.begin(), headers.end());
    require(headers.size() == 9, "exact header count");
    require(by_name["x-tbot-build-schema"] == "1", "schema header");
    require(by_name["x-tbot-hil-profile"] == "task14-hil-v1", "profile header");
    require(by_name["x-tbot-project-name"] == "xiaozhi", "project header");
    require(by_name["x-tbot-project-version"] == "2.2.75", "version header");
    require(by_name["x-tbot-idf-version"] == "v5.4.1", "idf header");
    require(by_name["x-tbot-secure-version"] == "0", "secure version header");
    require(by_name["x-tbot-elf-sha256"] == std::string(64, 'a'), "elf header");
    require(by_name["x-tbot-app-sha256"] == std::string(64, 'b'), "app header");
    require(by_name["x-tbot-build-id"] == "tbot-esp-v1:" + std::string(64, 'a'), "build id header");
    require(EspBuildId(value) == "tbot-esp-v1:" + std::string(64, 'a'), "derived build id");

    value.elf_sha256 = std::string(63, 'a');
    require(!ValidateEspBuildIdentity(value, &error), "short ELF digest rejected");
    value = valid_identity();
    value.app_sha256[0] = 'A';
    require(!ValidateEspBuildIdentity(value, &error), "uppercase app digest rejected");
    value = valid_identity();
    value.hil_profile = "task14 hil";
    require(!ValidateEspBuildIdentity(value, &error), "unsafe profile rejected");
    value = valid_identity();
    value.build_id = "tbot-esp-v1:" + std::string(64, 'c');
    require(!ValidateEspBuildIdentity(value, &error), "mismatched build id rejected");
    value = valid_identity();
    value.schema_version = 2;
    require(!ValidateEspBuildIdentity(value, &error), "unknown schema rejected");

    std::cout << "PASS: esp build identity native contract\n";
    return 0;
}
