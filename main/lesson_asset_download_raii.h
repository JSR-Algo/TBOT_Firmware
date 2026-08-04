#ifndef LESSON_ASSET_DOWNLOAD_RAII_H
#define LESSON_ASSET_DOWNLOAD_RAII_H

#include <cstdio>
#include <stdexcept>
#include <string>

template <typename HttpType>
class ScopedHttpClose {
public:
    explicit ScopedHttpClose(HttpType* http) : http_(http) {}
    ~ScopedHttpClose() { Close(); }

    void Close() {
        if (http_ != nullptr) {
            http_->Close();
            http_ = nullptr;
        }
    }

    ScopedHttpClose(const ScopedHttpClose&) = delete;
    ScopedHttpClose& operator=(const ScopedHttpClose&) = delete;

private:
    HttpType* http_;
};

class ScopedCFile {
public:
    using CloseFunction = int (*)(FILE*);

    explicit ScopedCFile(FILE* file) : file_(file), close_function_(std::fclose) {}
#if !defined(ESP_PLATFORM)
    ScopedCFile(FILE* file, CloseFunction close_function)
        : file_(file), close_function_(close_function) {}
#endif
    ~ScopedCFile() {
        if (file_ != nullptr) {
            close_function_(file_);
        }
    }

    ScopedCFile(const ScopedCFile&) = delete;
    ScopedCFile& operator=(const ScopedCFile&) = delete;

    FILE* get() const { return file_; }
    int Close() {
        if (file_ == nullptr) {
            return 0;
        }
        FILE* file = file_;
        file_ = nullptr;
        return close_function_(file);
    }

private:
    FILE* file_;
    CloseFunction close_function_;
};

class ScopedHeapAllocation {
public:
    using FreeFunction = void (*)(void*);

    ScopedHeapAllocation(void* allocation, FreeFunction free_function)
        : allocation_(allocation), free_function_(free_function) {}
    ~ScopedHeapAllocation() {
        if (allocation_ != nullptr) {
            free_function_(allocation_);
        }
    }

    ScopedHeapAllocation(const ScopedHeapAllocation&) = delete;
    ScopedHeapAllocation& operator=(const ScopedHeapAllocation&) = delete;

    void* get() const { return allocation_; }

private:
    void* allocation_;
    FreeFunction free_function_;
};

class ScopedTempPath {
public:
    using RemoveFunction = int (*)(const char*);
    using RenameFunction = int (*)(const char*, const char*);

    explicit ScopedTempPath(std::string path)
        : path_(std::move(path)),
          remove_function_(std::remove),
          rename_function_(std::rename) {}
#if !defined(ESP_PLATFORM)
    ScopedTempPath(
        std::string path,
        RemoveFunction remove_function,
        RenameFunction rename_function
    ) : path_(std::move(path)),
        remove_function_(remove_function),
        rename_function_(rename_function) {}
#endif

    ~ScopedTempPath() {
        if (armed_) {
            remove_function_(path_.c_str());
        }
    }

    ScopedTempPath(const ScopedTempPath&) = delete;
    ScopedTempPath& operator=(const ScopedTempPath&) = delete;

    const std::string& path() const { return path_; }

    void RemoveIfPresent() { remove_function_(path_.c_str()); }

    void CommitTo(const std::string& destination) {
        remove_function_(destination.c_str());
        if (rename_function_(path_.c_str(), destination.c_str()) != 0) {
            throw std::runtime_error("failed to commit SD file");
        }
        armed_ = false;
    }

private:
    std::string path_;
    RemoveFunction remove_function_;
    RenameFunction rename_function_;
    bool armed_ = true;
};

#endif  // LESSON_ASSET_DOWNLOAD_RAII_H
