#include "lesson_asset_download_staging.h"

#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

int main() {
    const std::string destination = TBOT_REAL_SHA256_TEST_ROOT "/asset.bin";
    const std::string abc_sha256 =
        "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";
    std::filesystem::create_directories(TBOT_REAL_SHA256_TEST_ROOT);
    {
        std::ofstream(destination, std::ios::binary) << "abc";
    }
    assert(VerifyLessonAssetSha256(destination, abc_sha256));
    assert(!VerifyLessonAssetSha256(destination, std::string(64, '0')));
    {
        LessonAssetDownloadStagingFile staging(destination);
        { std::ofstream(staging.path(), std::ios::binary) << "abd"; }
        bool rejected = false;
        try {
            CommitVerifiedLessonAssetDownload(staging, nullptr, destination, abc_sha256);
        } catch (const std::runtime_error&) {
            rejected = true;
        }
        assert(rejected);
        assert(VerifyLessonAssetSha256(destination, abc_sha256));
    }
    assert(!std::filesystem::exists(destination + ".download"));
    {
        LessonAssetDownloadStagingFile staging(destination);
        { std::ofstream(staging.path(), std::ios::binary); }
        const std::string empty_sha256 =
            "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855";
        CommitVerifiedLessonAssetDownload(staging, nullptr, destination, empty_sha256);
        assert(VerifyLessonAssetSha256(destination, empty_sha256));
    }
    assert(!std::filesystem::exists(destination + ".backup"));
    std::cout << "lesson asset real SHA256 host test OK (7 checks)\n";
}
