#pragma once

#include <cstddef>
#include <cstdio>
#include <string>

struct CourseModeHilAssetFile {
    FILE* file = nullptr;
    std::size_t bytes = 0;
};

bool OpenCourseModeHilAssetFile(
    const std::string& root,
    const std::string& relative_path,
    std::size_t max_bytes,
    CourseModeHilAssetFile* result);
void CloseCourseModeHilAssetFile(CourseModeHilAssetFile* file);
