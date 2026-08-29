#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

constexpr std::size_t kCourseModeHilMaxDecodedBytes = 2048;
constexpr std::size_t kCourseModeHilMaxEncodedBytes = 2731;

std::string EncodeCourseModeHilToken(const std::string& canonical_json);
bool DecodeCourseModeHilToken(const std::string& token, std::string* canonical_json);
bool DecodeCourseModeHilConsoleArgv(
    int argc, char* const* argv, std::string* canonical_json);

struct CourseModeHilIdentity {
    std::string device_id;
    std::string chip;
    std::string firmware_sha;
    std::string boot_id;
    std::string reset_reason;
    std::uint32_t boot_count = 0;
};

struct CourseModeHilSdEvidence {
    bool passed = false;
    std::uint32_t bytes = 0;
    std::string sha256;
    std::string cache_outcome;
};

struct CourseModeHilDiagnosticCallbacks {
    std::function<CourseModeHilIdentity()> identity;
    std::function<bool()> tft_test_pattern;
    std::function<CourseModeHilSdEvidence(
        const std::string&, const std::string&)> sd_read_cache;
    std::function<bool()> audio_drain;
    std::function<bool(int, bool)> safe_motion;
    std::function<bool()> stop_and_rest;
    std::function<void()> reboot;
};

class CourseModeHilDiagnostic {
public:
    explicit CourseModeHilDiagnostic(CourseModeHilDiagnosticCallbacks callbacks);
    std::string HandleLine(const std::string& line) const;

private:
    CourseModeHilDiagnosticCallbacks callbacks_;
};
