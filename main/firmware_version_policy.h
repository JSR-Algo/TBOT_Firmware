#ifndef FIRMWARE_VERSION_POLICY_H
#define FIRMWARE_VERSION_POLICY_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string_view>

struct ParsedFirmwareVersion {
    static constexpr size_t kMaxSegments = 8;
    std::array<uint32_t, kMaxSegments> segments{};
    size_t count = 0;
};

inline bool ParseStrictFirmwareVersion(std::string_view version,
                                       ParsedFirmwareVersion* parsed) {
    if (parsed == nullptr || version.empty()) {
        return false;
    }

    ParsedFirmwareVersion candidate;
    uint32_t value = 0;
    bool has_digit = false;
    for (size_t index = 0; index <= version.size(); ++index) {
        const char character = index < version.size() ? version[index] : '.';
        if (character >= '0' && character <= '9') {
            const uint32_t digit = static_cast<uint32_t>(character - '0');
            if (value > (std::numeric_limits<uint32_t>::max() - digit) / 10) {
                return false;
            }
            value = value * 10 + digit;
            has_digit = true;
            continue;
        }
        if (character != '.' || !has_digit ||
            candidate.count >= ParsedFirmwareVersion::kMaxSegments) {
            return false;
        }
        candidate.segments[candidate.count++] = value;
        value = 0;
        has_digit = false;
    }

    *parsed = candidate;
    return true;
}

inline int CompareFirmwareVersions(const ParsedFirmwareVersion& current,
                                   const ParsedFirmwareVersion& candidate) {
    const size_t count = current.count > candidate.count
        ? current.count
        : candidate.count;
    for (size_t index = 0; index < count; ++index) {
        const uint32_t current_segment =
            index < current.count ? current.segments[index] : 0;
        const uint32_t candidate_segment =
            index < candidate.count ? candidate.segments[index] : 0;
        if (candidate_segment > current_segment) {
            return 1;
        }
        if (candidate_segment < current_segment) {
            return -1;
        }
    }
    return 0;
}

struct FirmwareResponseDecision {
    bool valid = false;
    bool should_download = false;
};

inline FirmwareResponseDecision EvaluateFirmwareResponse(
        std::string_view current_version, std::string_view candidate_version,
        std::string_view firmware_url, bool force_install) {
    ParsedFirmwareVersion current;
    ParsedFirmwareVersion candidate;
    if (!ParseStrictFirmwareVersion(current_version, &current) ||
        !ParseStrictFirmwareVersion(candidate_version, &candidate)) {
        return {};
    }

    const bool should_download =
        force_install || CompareFirmwareVersions(current, candidate) > 0;
    if (should_download && firmware_url.empty()) {
        return {};
    }
    FirmwareResponseDecision decision;
    decision.valid = true;
    decision.should_download = should_download;
    return decision;
}

#endif  // FIRMWARE_VERSION_POLICY_H
