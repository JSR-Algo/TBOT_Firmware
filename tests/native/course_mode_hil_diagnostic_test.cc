#include "course_mode_hil_diagnostic.h"

#include <cJSON.h>

#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

extern "C" size_t esp_console_split_argv(char* line, char** argv, size_t argv_size);

namespace {

int checks = 0;

void require(bool condition, const char* message) {
    ++checks;
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

std::string Field(const std::string& json, const char* key) {
    cJSON* root = cJSON_Parse(json.c_str());
    require(root != nullptr, "response is JSON");
    const cJSON* value = cJSON_GetObjectItemCaseSensitive(root, key);
    std::string result = cJSON_IsString(value) ? value->valuestring : "";
    cJSON_Delete(root);
    return result;
}

void test_dispatches_real_probe_callbacks_and_echoes_nonce() {
    int motion_ms = 0;
    bool rest_required = false;
    bool reboot_requested = false;
    CourseModeHilDiagnosticCallbacks callbacks;
    callbacks.identity = [] {
        return CourseModeHilIdentity{
            "91deb5af-c1c0-416b-956d-266d510eac5e", "esp32s3", std::string(64, 'a'),
            "0123456789abcdef0123456789abcdef", "software", 4};
    };
    callbacks.tft_test_pattern = [] { return true; };
    callbacks.sd_read_cache = [](const std::string&, const std::string& sha) {
        return CourseModeHilSdEvidence{true, 1, sha, "verifiedReadback"};
    };
    callbacks.audio_drain = [] { return true; };
    callbacks.safe_motion = [&](int duration_ms, bool require_rest) {
        motion_ms = duration_ms;
        rest_required = require_rest;
        return true;
    };
    callbacks.stop_and_rest = [] { return true; };
    callbacks.reboot = [&] { reboot_requested = true; };

    CourseModeHilDiagnostic diagnostic(std::move(callbacks));
    const std::string identity = diagnostic.HandleLine(
        R"({"type":"course_mode_hil_probe","schemaVersion":1,"probe":"identity","nonce":"0123456789abcdef0123456789abcdef","flashingAllowed":false,"safeTestProtocol":"course-mode-hil.v1"})");
    require(Field(identity, "nonce") == "0123456789abcdef0123456789abcdef", "nonce is echoed");
    require(Field(identity, "deviceId") == "91deb5af-c1c0-416b-956d-266d510eac5e", "identity comes from callback");
    require(Field(identity, "bootId") == "0123456789abcdef0123456789abcdef",
            "identity includes current boot id");

    const std::string motion = diagnostic.HandleLine(
        R"({"type":"course_mode_hil_probe","schemaVersion":1,"probe":"motionAck","nonce":"1123456789abcdef0123456789abcdef","flashingAllowed":false,"safeTestProtocol":"course-mode-hil.v1","maxMotionMs":750,"requireRestAfter":true})");
    require(Field(motion, "status") == "PASS", "safe motion passes");
    require(motion_ms == 750 && rest_required, "motion is bounded and requires rest");

    const std::string reboot = diagnostic.HandleLine(
        R"({"type":"course_mode_hil_probe","schemaVersion":1,"probe":"rebootRecovery","nonce":"2123456789abcdef0123456789abcdef","flashingAllowed":false,"safeTestProtocol":"course-mode-hil.v1","action":"rebootAndRecover"})");
    require(Field(reboot, "status") == "PASS" && reboot_requested, "reboot is acknowledged then requested");
}

void test_sd_probe_requires_candidate_bound_verified_readback() {
    std::string path;
    std::string sha;
    CourseModeHilDiagnosticCallbacks callbacks;
    callbacks.sd_read_cache = [&](const std::string& value, const std::string& digest) {
        path = value;
        sha = digest;
        return CourseModeHilSdEvidence{true, 43599, digest, "verifiedReadback"};
    };
    CourseModeHilDiagnostic diagnostic(std::move(callbacks));
    const std::string request =
        R"({"type":"course_mode_hil_probe","schemaVersion":1,"probe":"sdReadCache","nonce":"6123456789abcdef0123456789abcdef","flashingAllowed":false,"safeTestProtocol":"course-mode-hil.v1","assetRelativePath":"candidate/background.jpg","assetSha256":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"})";
    const std::string response = diagnostic.HandleLine(request);
    require(Field(response, "status") == "PASS", "verified asset readback passes");
    require(path == "candidate/background.jpg" && sha == std::string(64, 'a'),
            "candidate path and hash reach physical reader");
    require(Field(response, "cacheOutcome") == "verifiedReadback",
            "response reports verified cache outcome");
    const std::string traversal = diagnostic.HandleLine(
        std::string(request).replace(request.find("candidate/background.jpg"),
                                     std::string("candidate/background.jpg").size(), "../secret"));
    require(Field(traversal, "status") == "FAIL", "path traversal is rejected");
    const std::string dot_component = diagnostic.HandleLine(
        std::string(request).replace(request.find("candidate/background.jpg"),
                                     std::string("candidate/background.jpg").size(),
                                     "candidate/./background.jpg"));
    require(Field(dot_component, "status") == "FAIL", "dot path component is rejected");
}

void test_identity_fails_closed_without_boot_proof() {
    CourseModeHilDiagnosticCallbacks callbacks;
    callbacks.identity = [] {
        return CourseModeHilIdentity{
            "91deb5af-c1c0-416b-956d-266d510eac5e", "esp32s3", std::string(64, 'a'),
            "", "software", 4};
    };
    CourseModeHilDiagnostic diagnostic(std::move(callbacks));
    const std::string response = diagnostic.HandleLine(
        R"({"type":"course_mode_hil_probe","schemaVersion":1,"probe":"identity","nonce":"7123456789abcdef0123456789abcdef","flashingAllowed":false,"safeTestProtocol":"course-mode-hil.v1"})");
    require(Field(response, "status") == "FAIL", "missing boot id fails identity probe");
}

void test_rejects_unsafe_or_forged_commands_before_callbacks() {
    int calls = 0;
    CourseModeHilDiagnosticCallbacks callbacks;
    callbacks.safe_motion = [&](int, bool) { ++calls; return true; };
    callbacks.stop_and_rest = [&] { ++calls; return true; };
    CourseModeHilDiagnostic diagnostic(std::move(callbacks));

    const std::string flash = diagnostic.HandleLine(
        R"({"type":"course_mode_hil_probe","schemaVersion":1,"probe":"motionAck","nonce":"3123456789abcdef0123456789abcdef","flashingAllowed":true,"safeTestProtocol":"course-mode-hil.v1","maxMotionMs":750,"requireRestAfter":true})");
    require(Field(flash, "status") == "FAIL", "flashing permission is rejected");
    const std::string long_motion = diagnostic.HandleLine(
        R"({"type":"course_mode_hil_probe","schemaVersion":1,"probe":"motionAck","nonce":"4123456789abcdef0123456789abcdef","flashingAllowed":false,"safeTestProtocol":"course-mode-hil.v1","maxMotionMs":751,"requireRestAfter":true})");
    require(Field(long_motion, "status") == "FAIL", "motion above cap is rejected");
    const std::string missing_nonce = diagnostic.HandleLine(
        R"({"type":"course_mode_hil_safety","schemaVersion":1,"action":"stopAndRest","flashingAllowed":false,"safeTestProtocol":"course-mode-hil.v1"})");
    require(Field(missing_nonce, "status") == "FAIL", "missing nonce is rejected");
    require(calls == 0, "unsafe commands do not reach physical callbacks");
}

void test_base64url_token_survives_real_esp_console_argv_splitting() {
    const std::string json =
        R"({"type":"course_mode_hil_probe","schemaVersion":1,"probe":"protocol","nonce":"5123456789abcdef0123456789abcdef","flashingAllowed":false,"safeTestProtocol":"course-mode-hil.v1"})";
    const std::string token = EncodeCourseModeHilToken(json);
    require(!token.empty() && token.find('=') == std::string::npos,
            "command uses unpadded base64url");
    const std::string command = "course_mode_hil " + token;
    std::vector<char> line(command.begin(), command.end());
    line.push_back('\0');
    char* argv[4]{};
    const size_t argc = esp_console_split_argv(line.data(), argv, 4);
    require(argc == 2 && std::string(argv[0]) == "course_mode_hil",
            "real ESP console parser keeps exactly one payload argv");
    std::string decoded;
    const bool decoded_ok = DecodeCourseModeHilConsoleArgv(
        static_cast<int>(argc), argv, &decoded);
    require(decoded_ok && decoded == json,
            "console payload decodes byte-for-byte");

    CourseModeHilDiagnostic diagnostic({});
    require(Field(diagnostic.HandleLine(decoded), "status") == "PASS",
            "decoded console command reaches dispatcher");
    require(!DecodeCourseModeHilToken(token + "=", &decoded),
            "padding is rejected");
    require(!DecodeCourseModeHilToken(
                EncodeCourseModeHilToken(
                    R"({"type":"course_mode_hil_probe","type":"course_mode_hil_probe","schemaVersion":1})"),
                &decoded),
            "duplicate object keys are rejected");
    require(!DecodeCourseModeHilToken(
                EncodeCourseModeHilToken(R"({ "type":"course_mode_hil_probe"})"),
                &decoded),
            "noncanonical JSON whitespace is rejected");
    char extra_command[] = "course_mode_hil token extra";
    char* extra_argv[4]{};
    const size_t extra_argc = esp_console_split_argv(extra_command, extra_argv, 4);
    require(!DecodeCourseModeHilConsoleArgv(
                static_cast<int>(extra_argc), extra_argv, &decoded),
            "extra console argv are rejected");
}

}  // namespace

int main() {
    test_dispatches_real_probe_callbacks_and_echoes_nonce();
    test_rejects_unsafe_or_forged_commands_before_callbacks();
    test_sd_probe_requires_candidate_bound_verified_readback();
    test_identity_fails_closed_without_boot_proof();
    test_base64url_token_survives_real_esp_console_argv_splitting();
    std::cout << "course mode HIL diagnostic test OK (" << checks << " checks)\n";
}
