#include "course_mode_hil_diagnostic.h"

#include <cJSON.h>

#include <cctype>
#include <cstdint>
#include <memory>
#include <set>
#include <utility>

namespace {

constexpr int kMaxMotionMs = 750;
constexpr char kBase64Url[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

using JsonPtr = std::unique_ptr<cJSON, decltype(&cJSON_Delete)>;

bool ExactString(const cJSON* root, const char* key, const char* expected) {
    const cJSON* value = cJSON_GetObjectItemCaseSensitive(root, key);
    return cJSON_IsString(value) && value->valuestring != nullptr &&
           std::string(value->valuestring) == expected;
}

std::string String(const cJSON* root, const char* key) {
    const cJSON* value = cJSON_GetObjectItemCaseSensitive(root, key);
    return cJSON_IsString(value) && value->valuestring != nullptr ? value->valuestring : "";
}

bool ValidNonce(const std::string& nonce) {
    if (nonce.size() != 32) return false;
    for (char ch : nonce) {
        if (!std::isxdigit(static_cast<unsigned char>(ch)) ||
            std::isupper(static_cast<unsigned char>(ch))) return false;
    }
    return true;
}

bool LowerSha256(const std::string& value) {
    if (value.size() != 64) return false;
    for (char ch : value) {
        if (!std::isdigit(static_cast<unsigned char>(ch)) && (ch < 'a' || ch > 'f')) {
            return false;
        }
    }
    return true;
}

bool SafeAssetRelativePath(const std::string& value) {
    if (value.empty() || value.size() > 256 || value.front() == '/' ||
        value.back() == '/' || value.find("//") != std::string::npos) return false;
    for (unsigned char ch : value) {
        if (!std::isalnum(ch) && ch != '/' && ch != '-' && ch != '_' && ch != '.') {
            return false;
        }
    }
    std::size_t begin = 0;
    while (begin < value.size()) {
        const std::size_t slash = value.find('/', begin);
        const std::string component = value.substr(
            begin, slash == std::string::npos ? std::string::npos : slash - begin);
        if (component == "." || component == "..") return false;
        if (slash == std::string::npos) break;
        begin = slash + 1;
    }
    return true;
}

std::string Print(JsonPtr root) {
    char* raw = cJSON_PrintUnformatted(root.get());
    if (raw == nullptr) return R"({"type":"course_mode_hil_error","status":"FAIL"})";
    std::string result(raw);
    cJSON_free(raw);
    return result;
}

JsonPtr Response(const char* type, const std::string& nonce,
                 const char* probe, bool passed, const char* error = nullptr) {
    JsonPtr root(cJSON_CreateObject(), cJSON_Delete);
    if (!root) return root;
    cJSON_AddStringToObject(root.get(), "type", type);
    cJSON_AddNumberToObject(root.get(), "schemaVersion", 1);
    if (!nonce.empty()) cJSON_AddStringToObject(root.get(), "nonce", nonce.c_str());
    if (probe != nullptr) cJSON_AddStringToObject(root.get(), "probe", probe);
    cJSON_AddStringToObject(root.get(), "status", passed ? "PASS" : "FAIL");
    if (error != nullptr) cJSON_AddStringToObject(root.get(), "error", error);
    return root;
}

bool SafeEnvelope(const cJSON* root) {
    const cJSON* schema = cJSON_GetObjectItemCaseSensitive(root, "schemaVersion");
    const cJSON* flashing = cJSON_GetObjectItemCaseSensitive(root, "flashingAllowed");
    return cJSON_IsNumber(schema) && schema->valuedouble == 1 &&
           cJSON_IsFalse(flashing) &&
           ExactString(root, "safeTestProtocol", "course-mode-hil.v1");
}

bool UniqueObjectKeys(const cJSON* value) {
    if (cJSON_IsObject(value)) {
        std::set<std::string> keys;
        for (const cJSON* child = value->child; child != nullptr; child = child->next) {
            if (child->string == nullptr || !keys.insert(child->string).second ||
                !UniqueObjectKeys(child)) return false;
        }
    } else if (cJSON_IsArray(value)) {
        for (const cJSON* child = value->child; child != nullptr; child = child->next) {
            if (!UniqueObjectKeys(child)) return false;
        }
    }
    return true;
}

int Base64UrlValue(char ch) {
    if (ch >= 'A' && ch <= 'Z') return ch - 'A';
    if (ch >= 'a' && ch <= 'z') return ch - 'a' + 26;
    if (ch >= '0' && ch <= '9') return ch - '0' + 52;
    if (ch == '-') return 62;
    if (ch == '_') return 63;
    return -1;
}

}  // namespace

std::string EncodeCourseModeHilToken(const std::string& canonical_json) {
    if (canonical_json.empty() || canonical_json.size() > kCourseModeHilMaxDecodedBytes) {
        return "";
    }
    std::string output;
    output.reserve((canonical_json.size() * 4 + 2) / 3);
    std::uint32_t accumulator = 0;
    int bits = 0;
    for (unsigned char byte : canonical_json) {
        accumulator = (accumulator << 8) | byte;
        bits += 8;
        while (bits >= 6) {
            bits -= 6;
            output.push_back(kBase64Url[(accumulator >> bits) & 0x3f]);
        }
    }
    if (bits > 0) output.push_back(kBase64Url[(accumulator << (6 - bits)) & 0x3f]);
    return output.size() <= kCourseModeHilMaxEncodedBytes ? output : "";
}

bool DecodeCourseModeHilToken(const std::string& token, std::string* canonical_json) {
    if (canonical_json == nullptr || token.empty() ||
        token.size() > kCourseModeHilMaxEncodedBytes || token.size() % 4 == 1) return false;
    std::string decoded;
    decoded.reserve(token.size() * 3 / 4);
    std::uint32_t accumulator = 0;
    int bits = 0;
    for (char ch : token) {
        const int value = Base64UrlValue(ch);
        if (value < 0) return false;
        accumulator = (accumulator << 6) | static_cast<std::uint32_t>(value);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            decoded.push_back(static_cast<char>((accumulator >> bits) & 0xff));
            if (decoded.size() > kCourseModeHilMaxDecodedBytes) return false;
        }
    }
    if (bits > 0 && (accumulator & ((1U << bits) - 1U)) != 0) return false;
    JsonPtr root(cJSON_ParseWithLength(decoded.data(), decoded.size()), cJSON_Delete);
    if (!root || !cJSON_IsObject(root.get()) || !UniqueObjectKeys(root.get())) return false;
    char* printed = cJSON_PrintUnformatted(root.get());
    if (printed == nullptr) return false;
    const bool canonical = decoded == printed;
    cJSON_free(printed);
    if (!canonical) return false;
    *canonical_json = std::move(decoded);
    return true;
}

bool DecodeCourseModeHilConsoleArgv(
    int argc, char* const* argv, std::string* canonical_json) {
    return argc == 2 && argv != nullptr && argv[0] != nullptr && argv[1] != nullptr &&
           std::string(argv[0]) == "course_mode_hil" &&
           DecodeCourseModeHilToken(argv[1], canonical_json);
}

CourseModeHilDiagnostic::CourseModeHilDiagnostic(CourseModeHilDiagnosticCallbacks callbacks)
    : callbacks_(std::move(callbacks)) {}

std::string CourseModeHilDiagnostic::HandleLine(const std::string& line) const {
    if (line.empty() || line.size() > kCourseModeHilMaxDecodedBytes) {
        return Print(Response("course_mode_hil_error", "", nullptr, false, "invalidSize"));
    }
    JsonPtr root(cJSON_ParseWithLength(line.data(), line.size()), cJSON_Delete);
    if (!root || !cJSON_IsObject(root.get())) {
        return Print(Response("course_mode_hil_error", "", nullptr, false, "invalidJson"));
    }
    const std::string nonce = String(root.get(), "nonce");
    if (!ValidNonce(nonce) || !SafeEnvelope(root.get())) {
        return Print(Response("course_mode_hil_error", nonce, nullptr, false, "unsafeEnvelope"));
    }

    if (ExactString(root.get(), "type", "course_mode_hil_safety")) {
        const bool passed = ExactString(root.get(), "action", "stopAndRest") &&
                            callbacks_.stop_and_rest && callbacks_.stop_and_rest();
        return Print(Response("course_mode_hil_safety_ack", nonce, nullptr, passed,
                              passed ? nullptr : "stopRestFailed"));
    }
    if (!ExactString(root.get(), "type", "course_mode_hil_probe")) {
        return Print(Response("course_mode_hil_error", nonce, nullptr, false, "unsupportedType"));
    }

    const std::string probe = String(root.get(), "probe");
    bool passed = false;
    JsonPtr response = Response("course_mode_hil_evidence", nonce, probe.c_str(), false);
    if (probe == "identity" && callbacks_.identity) {
        const CourseModeHilIdentity identity = callbacks_.identity();
        passed = !identity.device_id.empty() && identity.chip == "esp32s3" &&
                 identity.firmware_sha.size() == 64 && identity.boot_id.size() == 32 &&
                 !identity.reset_reason.empty() && identity.boot_count > 0;
        if (passed) {
            cJSON_AddStringToObject(response.get(), "deviceId", identity.device_id.c_str());
            cJSON_AddStringToObject(response.get(), "chip", identity.chip.c_str());
            cJSON_AddStringToObject(response.get(), "firmwareSha", identity.firmware_sha.c_str());
            cJSON_AddStringToObject(response.get(), "bootId", identity.boot_id.c_str());
            cJSON_AddStringToObject(response.get(), "resetReason", identity.reset_reason.c_str());
            cJSON_AddNumberToObject(response.get(), "bootCount", identity.boot_count);
        }
    } else if (probe == "protocol") {
        passed = true;
        cJSON_AddNumberToObject(response.get(), "baud", 115200);
        cJSON_AddStringToObject(response.get(), "protocolVersion", "teebot-lesson-renderer.v5");
    } else if (probe == "capability") {
        passed = true;
        cJSON_AddBoolToObject(response.get(), "lessonRendererV5", true);
    } else if (probe == "tftTestPattern") {
        passed = callbacks_.tft_test_pattern && callbacks_.tft_test_pattern();
    } else if (probe == "sdReadCache") {
        const std::string path = String(root.get(), "assetRelativePath");
        const std::string sha256 = String(root.get(), "assetSha256");
        if (callbacks_.sd_read_cache && SafeAssetRelativePath(path) && LowerSha256(sha256)) {
            const CourseModeHilSdEvidence evidence = callbacks_.sd_read_cache(path, sha256);
            passed = evidence.passed && evidence.bytes > 0 && evidence.sha256 == sha256 &&
                     evidence.cache_outcome == "verifiedReadback";
            if (passed) {
                cJSON_AddNumberToObject(response.get(), "bytes", evidence.bytes);
                cJSON_AddStringToObject(response.get(), "sha256", evidence.sha256.c_str());
                cJSON_AddStringToObject(
                    response.get(), "cacheOutcome", evidence.cache_outcome.c_str());
            }
        }
    } else if (probe == "audioDrain") {
        passed = callbacks_.audio_drain && callbacks_.audio_drain();
    } else if (probe == "motionAck") {
        const cJSON* duration = cJSON_GetObjectItemCaseSensitive(root.get(), "maxMotionMs");
        const cJSON* rest = cJSON_GetObjectItemCaseSensitive(root.get(), "requireRestAfter");
        passed = cJSON_IsNumber(duration) && duration->valuedouble > 0 &&
                 duration->valuedouble <= kMaxMotionMs && cJSON_IsTrue(rest) &&
                 callbacks_.safe_motion &&
                 callbacks_.safe_motion(static_cast<int>(duration->valuedouble), true);
    } else if (probe == "stopRest" || probe == "reconnect") {
        passed = callbacks_.stop_and_rest && callbacks_.stop_and_rest();
    } else if (probe == "rebootRecovery") {
        passed = ExactString(root.get(), "action", "rebootAndRecover") &&
                 callbacks_.stop_and_rest && callbacks_.stop_and_rest();
        if (passed && callbacks_.reboot) callbacks_.reboot();
    }
    cJSON_ReplaceItemInObjectCaseSensitive(
        response.get(), "status", cJSON_CreateString(passed ? "PASS" : "FAIL"));
    if (!passed) cJSON_AddStringToObject(response.get(), "error", "probeFailed");
    return Print(std::move(response));
}
