#ifndef LESSON_ASSET_HTTP_TRANSFER_H
#define LESSON_ASSET_HTTP_TRANSFER_H

#include <cstddef>
#include <string>

#include <http.h>

void DownloadLessonAssetHttpBodyToFile(
    Http& http,
    const char* cache_key,
    bool has_declared_size,
    std::size_t declared_size,
    const std::string& url,
    const std::string& destination,
    std::size_t& bytes_out
);

#endif  // LESSON_ASSET_HTTP_TRANSFER_H
