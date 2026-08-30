#include "course_mode_hil_sd_reader.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <limits>
#include <cctype>
#include <cstdint>
#include <vector>

namespace {

bool SplitSafeRelativePath(const std::string& value, std::vector<std::string>* components) {
    if (components == nullptr || value.empty() || value.size() > 256 ||
        value.front() == '/' || value.back() == '/') return false;
    std::size_t begin = 0;
    while (begin < value.size()) {
        const std::size_t slash = value.find('/', begin);
        const std::string component = value.substr(
            begin, slash == std::string::npos ? std::string::npos : slash - begin);
        if (component.empty() || component == "." || component == "..") return false;
        for (unsigned char ch : component) {
            if (!std::isalnum(ch) && ch != '-' && ch != '_' && ch != '.') return false;
        }
        components->push_back(component);
        if (slash == std::string::npos) break;
        begin = slash + 1;
    }
    return !components->empty();
}

bool ValidRegularFile(int descriptor, std::size_t max_bytes, std::size_t* bytes) {
    struct stat metadata {};
    if (descriptor < 0 || bytes == nullptr || fstat(descriptor, &metadata) != 0 ||
        !S_ISREG(metadata.st_mode) || metadata.st_size <= 0 ||
        static_cast<std::uint64_t>(metadata.st_size) > max_bytes ||
        static_cast<std::uint64_t>(metadata.st_size) >
            std::numeric_limits<std::size_t>::max()) return false;
    *bytes = static_cast<std::size_t>(metadata.st_size);
    return true;
}

}  // namespace

bool OpenCourseModeHilAssetFile(
    const std::string& root,
    const std::string& relative_path,
    std::size_t max_bytes,
    CourseModeHilAssetFile* result) {
    if (result == nullptr || result->file != nullptr || max_bytes == 0) return false;
    std::vector<std::string> components;
    if (!SplitSafeRelativePath(relative_path, &components)) return false;

#if !defined(ESP_PLATFORM)
    int directory = open(root.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (directory < 0) return false;
    for (std::size_t index = 0; index + 1 < components.size(); ++index) {
        const int next = openat(
            directory, components[index].c_str(),
            O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        close(directory);
        if (next < 0) return false;
        directory = next;
    }
    const int descriptor = openat(
        directory, components.back().c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    close(directory);
#else
    // ESP-IDF FatFS exposes no openat/lstat, does not open directories as descriptors,
    // and cannot represent symlinks. Validate every component and use O_NOFOLLOW on the
    // final file. This fallback must remain scoped to the fixed /sdcard FatFS mount.
    std::string path = root;
    struct stat root_metadata {};
    if (root != "/sdcard/tbot/lesson-assets" || stat(path.c_str(), &root_metadata) != 0 ||
        !S_ISDIR(root_metadata.st_mode)) return false;
    for (std::size_t index = 0; index < components.size(); ++index) {
        path.push_back('/');
        path += components[index];
        struct stat metadata {};
        if (stat(path.c_str(), &metadata) != 0 || S_ISLNK(metadata.st_mode)) return false;
        const bool final = index + 1 == components.size();
        if ((!final && !S_ISDIR(metadata.st_mode)) || (final && !S_ISREG(metadata.st_mode))) {
            return false;
        }
    }
    const int descriptor = open(path.c_str(), O_RDONLY | O_NOFOLLOW);
#endif
    std::size_t bytes = 0;
    if (!ValidRegularFile(descriptor, max_bytes, &bytes)) {
        if (descriptor >= 0) close(descriptor);
        return false;
    }
    FILE* file = fdopen(descriptor, "rb");
    if (file == nullptr) {
        close(descriptor);
        return false;
    }
    result->file = file;
    result->bytes = bytes;
    return true;
}

void CloseCourseModeHilAssetFile(CourseModeHilAssetFile* file) {
    if (file == nullptr) return;
    if (file->file != nullptr) std::fclose(file->file);
    file->file = nullptr;
    file->bytes = 0;
}
