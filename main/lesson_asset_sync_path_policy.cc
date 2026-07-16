#include "lesson_asset_sync_path_policy.h"

#include <cstddef>
#include <cstring>
#include <string>
#include <string_view>

#include "lesson_asset_cache_evict.h"

namespace {

constexpr const char* kLessonAssetPackRoot = "/sdcard/tbot/lesson-assets/";
constexpr std::size_t kLessonAssetBasenameMaxBytes = 255;

char AsciiLower(char value) {
    const unsigned char byte = static_cast<unsigned char>(value);
    if (byte >= 'A' && byte <= 'Z') {
        return static_cast<char>(byte + ('a' - 'A'));
    }
    return value;
}

std::string AsciiLowerCopy(const std::string& value) {
    std::string lowered;
    lowered.reserve(value.size());
    for (char byte : value) {
        lowered.push_back(AsciiLower(byte));
    }
    return lowered;
}

bool EndsWith(const std::string& value, const char* suffix) {
    const std::string suffix_value(suffix);
    return value.size() >= suffix_value.size() &&
           value.compare(value.size() - suffix_value.size(), suffix_value.size(), suffix_value) == 0;
}

bool IsReservedBasename(const std::string& basename) {
    const std::string lowered = AsciiLowerCopy(basename);
    if (!lowered.empty() && lowered.back() == '.') {
        return true;
    }
    if (lowered == "." || lowered == ".." ||
        lowered == "current.json" || lowered == "pvg.json" ||
        lowered == "activation.json" || lowered == "lesson-pack-activation.json") {
        return true;
    }
    return EndsWith(lowered, ".tmp") || EndsWith(lowered, ".download") ||
           EndsWith(lowered, ".part") || EndsWith(lowered, ".backup");
}

bool IsUpperHex(char value) {
    return (value >= '0' && value <= '9') || (value >= 'A' && value <= 'F');
}

bool IsUnreservedByte(unsigned char value) {
    return (value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z') ||
           (value >= '0' && value <= '9') || value == '-' || value == '.' ||
           value == '_' || value == '~';
}

bool IsCanonicalEncodedBasename(const std::string& basename) {
    if (basename.empty() || basename.size() > kLessonAssetBasenameMaxBytes) {
        return false;
    }
    for (std::size_t index = 0; index < basename.size(); ++index) {
        const unsigned char byte = static_cast<unsigned char>(basename[index]);
        if (IsUnreservedByte(byte)) {
            continue;
        }
        if (byte != '%' || index + 2 >= basename.size() ||
            !IsUpperHex(basename[index + 1]) || !IsUpperHex(basename[index + 2])) {
            return false;
        }
        index += 2;
    }
    return true;
}

char UpperHexDigit(unsigned char value) {
    return value < 10 ? static_cast<char>('0' + value)
                      : static_cast<char>('A' + (value - 10));
}

bool PercentTripletMatches(
    const std::string& basename,
    std::size_t offset,
    unsigned char value
) {
    return offset + 2 < basename.size() && basename[offset] == '%' &&
           basename[offset + 1] == UpperHexDigit(value >> 4) &&
           basename[offset + 2] == UpperHexDigit(value & 0x0f);
}

bool BasenameMatchesAssetKey(
    const std::string& basename,
    std::string_view asset_key
) {
    if (asset_key.empty() || asset_key.size() > kLessonAssetSyncKeyMaxBytes ||
        asset_key.find('\0') != std::string::npos) {
        return false;
    }
    std::size_t basename_offset = 0;
    for (unsigned char key_byte : asset_key) {
        if (IsUnreservedByte(key_byte) && basename_offset < basename.size() &&
            basename[basename_offset] == static_cast<char>(key_byte)) {
            ++basename_offset;
            continue;
        }
        if (!PercentTripletMatches(basename, basename_offset, key_byte)) {
            return false;
        }
        basename_offset += 3;
    }
    return basename_offset == basename.size();
}

}  // namespace

bool IsCanonicalLessonAssetSyncCacheKey(std::string_view cache_key) {
    return cache_key.size() <= kLessonAssetCacheKeyMaxBytes &&
           IsCanonicalLessonCacheKey(std::string(cache_key));
}

bool IsExactLowerLessonAssetSha256(std::string_view value) {
    if (value.size() != kLessonAssetSyncSha256HexBytes) {
        return false;
    }
    for (char byte : value) {
        if (!((byte >= '0' && byte <= '9') || (byte >= 'a' && byte <= 'f'))) {
            return false;
        }
    }
    return true;
}

bool IsAllowedLessonAssetSyncUrl(std::string_view value) {
    if (value.empty() || value.size() > kLessonAssetSyncUrlMaxBytes ||
        value.find('\0') != std::string::npos || value.find('\\') != std::string::npos) {
        return false;
    }
    std::size_t authority_start = 0;
    if (value.rfind("https://", 0) == 0) {
        authority_start = std::strlen("https://");
    } else if (value.rfind("http://", 0) == 0) {
        authority_start = std::strlen("http://");
    } else {
        return false;
    }
    std::size_t authority_end = value.find_first_of("/?#", authority_start);
    if (authority_end == std::string::npos) {
        authority_end = value.size();
    }
    if (authority_end == authority_start ||
        value.find('@', authority_start) < authority_end) {
        return false;
    }
    const std::string_view authority =
        value.substr(authority_start, authority_end - authority_start);
    if (authority.front() == '[') {
        const std::size_t close = authority.find(']');
        if (close <= 1 ||
            (close + 1 < authority.size() && authority[close + 1] != ':')) {
            return false;
        }
    } else if (authority.front() == ':' || authority.front() == '.') {
        return false;
    }
    for (unsigned char byte : value) {
        if (byte < 0x21 || byte > 0x7e) {
            return false;
        }
    }
    return true;
}

LessonAssetSyncPathResult ValidateLessonAssetSyncPath(
    std::string_view cache_key,
    std::string_view local_path,
    std::string_view asset_key
) {
    LessonAssetSyncPathResult result{LessonAssetSyncPathCode::kInvalidPath, {}};
    if (!IsCanonicalLessonAssetSyncCacheKey(cache_key)) {
        result.code = LessonAssetSyncPathCode::kInvalidCacheKey;
        return result;
    }

    const std::string expected_prefix =
        std::string(kLessonAssetPackRoot) + std::string(cache_key) + "/";
    if (local_path.size() <= expected_prefix.size() ||
        local_path.compare(0, expected_prefix.size(), expected_prefix) != 0) {
        return result;
    }

    const std::string basename(local_path.substr(expected_prefix.size()));
    if (!IsCanonicalEncodedBasename(basename) ||
        !BasenameMatchesAssetKey(basename, asset_key)) {
        return result;
    }
    if (IsReservedBasename(basename)) {
        result.code = LessonAssetSyncPathCode::kReservedDestination;
        return result;
    }

    const std::string reconstructed = expected_prefix + basename;
    if (reconstructed.size() != local_path.size() || reconstructed != local_path) {
        return result;
    }
    result.code = LessonAssetSyncPathCode::kValid;
    result.destination = reconstructed;
    return result;
}

bool LessonAssetSyncDestinationsCollide(
    std::string_view left,
    std::string_view right
) {
    while (!left.empty() && left.back() == '.') {
        left.remove_suffix(1);
    }
    while (!right.empty() && right.back() == '.') {
        right.remove_suffix(1);
    }
    if (left.size() != right.size()) {
        return false;
    }
    for (std::size_t index = 0; index < left.size(); ++index) {
        if (AsciiLower(left[index]) != AsciiLower(right[index])) {
            return false;
        }
    }
    return true;
}
