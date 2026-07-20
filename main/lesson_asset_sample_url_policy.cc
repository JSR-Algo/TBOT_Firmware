#include "lesson_asset_sample_url_policy.h"

#include <cstddef>
#include <string_view>

#include "lesson_asset_sync_path_policy.h"

namespace {

bool IsAsciiDigit(char value) {
    return value >= '0' && value <= '9';
}

bool IsAsciiAlphaNumeric(char value) {
    return (value >= 'a' && value <= 'z') ||
           (value >= 'A' && value <= 'Z') || IsAsciiDigit(value);
}

bool IsValidSampleLessonAssetPort(std::string_view port) {
    if (port.empty() || port.size() > 5) {
        return false;
    }
    unsigned value = 0;
    for (char byte : port) {
        if (!IsAsciiDigit(byte)) {
            return false;
        }
        value = value * 10 + static_cast<unsigned>(byte - '0');
    }
    return value > 0 && value <= 65535;
}

bool IsValidSampleLessonAssetHost(std::string_view host) {
    if (host.empty() || host.size() > 253 || host.front() == '.' || host.back() == '.') {
        return false;
    }
    std::size_t label_start = 0;
    while (label_start < host.size()) {
        const std::size_t label_end = host.find('.', label_start);
        const std::size_t end = label_end == std::string_view::npos ? host.size() : label_end;
        const std::string_view label = host.substr(label_start, end - label_start);
        if (label.empty() || label.size() > 63 || !IsAsciiAlphaNumeric(label.front()) ||
            !IsAsciiAlphaNumeric(label.back())) {
            return false;
        }
        for (char byte : label) {
            if (!IsAsciiAlphaNumeric(byte) && byte != '-') {
                return false;
            }
        }
        if (label_end == std::string_view::npos) {
            break;
        }
        label_start = label_end + 1;
    }
    return true;
}

bool IsValidSampleLessonAssetAuthority(std::string_view authority) {
    if (authority.empty()) {
        return false;
    }
    if (authority.front() == '[') {
        const std::size_t close = authority.find(']');
        if (close == std::string_view::npos || close <= 1 ||
            authority.find('[', 1) != std::string_view::npos ||
            authority.find(']', close + 1) != std::string_view::npos) {
            return false;
        }
        const std::string_view literal = authority.substr(1, close - 1);
        for (char byte : literal) {
            if (!IsAsciiDigit(byte) && !(byte >= 'a' && byte <= 'f') &&
                !(byte >= 'A' && byte <= 'F') && byte != ':' && byte != '.') {
                return false;
            }
        }
        if (literal.find(':') == std::string_view::npos) {
            return false;
        }
        return close + 1 == authority.size() ||
               (authority[close + 1] == ':' &&
                IsValidSampleLessonAssetPort(authority.substr(close + 2)));
    }

    const std::size_t colon = authority.find(':');
    if (colon != std::string_view::npos && authority.find(':', colon + 1) != std::string_view::npos) {
        return false;
    }
    const std::string_view host = authority.substr(0, colon);
    return IsValidSampleLessonAssetHost(host) &&
           (colon == std::string_view::npos ||
            IsValidSampleLessonAssetPort(authority.substr(colon + 1)));
}

}  // namespace

bool IsAllowedSampleLessonAssetBaseUrl(std::string_view value) {
    if (!IsAllowedLessonAssetSyncUrl(value) || value.find('?') != std::string_view::npos ||
        value.find('#') != std::string_view::npos) {
        return false;
    }
    const std::size_t authority_start = value.find("://") + 3;
    const std::size_t authority_end = value.find('/', authority_start);
    return IsValidSampleLessonAssetAuthority(value.substr(
        authority_start,
        authority_end == std::string_view::npos ? value.size() - authority_start
                                                : authority_end - authority_start));
}
