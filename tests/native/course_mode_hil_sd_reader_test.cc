#include "course_mode_hil_sd_reader.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

namespace fs = std::filesystem;

namespace {

int checks = 0;
const fs::path kBase = "/tmp/tbot-course-mode-hil-sd-reader-test";

void Expect(bool condition, const char* message) {
    ++checks;
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

void Write(const fs::path& path, const std::string& value) {
    fs::create_directories(path.parent_path());
    std::ofstream(path, std::ios::binary) << value;
}

void test_opens_only_regular_files_beneath_trusted_root() {
    fs::remove_all(kBase);
    const fs::path root = kBase / "root";
    const fs::path outside = kBase / "outside";
    Write(root / "candidate" / "asset.bin", "candidate-data");
    Write(outside / "secret.bin", "secret-data");

    CourseModeHilAssetFile file;
    Expect(OpenCourseModeHilAssetFile(
               root.string(), "candidate/asset.bin", 1024, &file),
           "regular candidate asset opens beneath trusted root");
    Expect(file.bytes == 14 && file.file != nullptr, "opener reports bounded file size");
    CloseCourseModeHilAssetFile(&file);

    fs::create_directory_symlink(outside, root / "intermediate-link");
    Expect(!OpenCourseModeHilAssetFile(
               root.string(), "intermediate-link/secret.bin", 1024, &file),
           "intermediate symlink cannot escape trusted root");

    fs::create_symlink(outside / "secret.bin", root / "final-link.bin");
    Expect(!OpenCourseModeHilAssetFile(
               root.string(), "final-link.bin", 1024, &file),
           "final symlink cannot escape trusted root");

    const fs::path root_link = kBase / "root-link";
    fs::create_directory_symlink(root, root_link);
    Expect(!OpenCourseModeHilAssetFile(
               root_link.string(), "candidate/asset.bin", 1024, &file),
           "trusted root itself must not be a symlink");
    Expect(!OpenCourseModeHilAssetFile(
               root.string(), "candidate/../asset.bin", 1024, &file),
           "parent traversal component is rejected");
    Expect(!OpenCourseModeHilAssetFile(
               root.string(), "candidate/asset.bin", 4, &file),
           "oversized candidate is rejected before reading");
    fs::remove_all(kBase);
}

}  // namespace

int main() {
    test_opens_only_regular_files_beneath_trusted_root();
    std::cout << "course mode HIL SD reader test OK (" << checks << " checks)\n";
}
