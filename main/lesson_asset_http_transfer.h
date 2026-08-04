#ifndef LESSON_ASSET_HTTP_TRANSFER_H
#define LESSON_ASSET_HTTP_TRANSFER_H

#include <cstddef>
#include <string>

#include <http.h>

#ifndef CONFIG_TBOT_LESSON_ASSET_MAX_FILE_BYTES
#define CONFIG_TBOT_LESSON_ASSET_MAX_FILE_BYTES (128U * 1024U * 1024U)
#endif

#ifndef CONFIG_TBOT_LESSON_ASSET_MAX_PACK_BYTES
#define CONFIG_TBOT_LESSON_ASSET_MAX_PACK_BYTES (256U * 1024U * 1024U)
#endif

constexpr std::size_t LessonAssetMaxFileBytes() {
    return static_cast<std::size_t>(CONFIG_TBOT_LESSON_ASSET_MAX_FILE_BYTES);
}

constexpr std::size_t LessonAssetMaxPackBytes() {
    return static_cast<std::size_t>(CONFIG_TBOT_LESSON_ASSET_MAX_PACK_BYTES);
}

constexpr bool IsLessonAssetDeclaredFileSizeAllowed(std::size_t declared_size) {
    return declared_size > 0 && declared_size <= LessonAssetMaxFileBytes();
}

constexpr bool AccumulateLessonAssetDeclaredSize(
    std::size_t current_total,
    std::size_t declared_size,
    std::size_t& next_total
) {
    if (!IsLessonAssetDeclaredFileSizeAllowed(declared_size) ||
        current_total > LessonAssetMaxPackBytes() ||
        declared_size > LessonAssetMaxPackBytes() - current_total) {
        return false;
    }
    next_total = current_total + declared_size;
    return true;
}

static_assert(
    LessonAssetMaxPackBytes() >= LessonAssetMaxFileBytes(),
    "lesson asset pack limit must cover one maximum-sized file");

void DownloadLessonAssetHttpBodyToFile(
    Http& http,
    const char* cache_key,
    bool has_declared_size,
    std::size_t declared_size,
    const std::string& url,
    const std::string& destination,
    std::size_t& bytes_out,
    const char* media_type = nullptr
);

#endif  // LESSON_ASSET_HTTP_TRANSFER_H
