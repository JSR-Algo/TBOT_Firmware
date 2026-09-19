#include "lesson_asset_retained_selection.h"
#include "lesson_asset_cache_evict.h"
#include "lesson_asset_storage_coordinator.h"
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdlib>
#include <sstream>
#include <stdexcept>
#include <vector>
#ifdef ESP_PLATFORM
#include <nvs.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#ifndef TBOT_LESSON_ASSET_ROOT
#define TBOT_LESSON_ASSET_ROOT "/sdcard/tbot/lesson-assets"
#endif
namespace {
constexpr std::uint64_t kMaxRevision = 9007199254740991ULL;
constexpr std::size_t kMaxRecord = 2048;
std::atomic<bool> write_uncertain{false};
[[noreturn]] void Refuse() { throw std::runtime_error("retained_selection_unavailable"); }
bool Hex(const std::string& value, std::size_t size) {
    return value.size() == size && std::all_of(value.begin(), value.end(), [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    });
}
bool Uuid(const std::string& value) {
    if (value.size() != 36) return false;
    for (std::size_t i = 0; i < value.size(); ++i) {
        if (i == 8 || i == 13 || i == 18 || i == 23) { if (value[i] != '-') return false; }
        else if (!((value[i] >= '0' && value[i] <= '9') || (value[i] >= 'a' && value[i] <= 'f'))) return false;
    }
    return true;
}
std::uint64_t Number(const std::string& text) {
    if (text.empty() || (text.size() > 1 && text[0] == '0')) Refuse();
    std::uint64_t number = 0;
    for (char c : text) {
        if (c < '0' || c > '9' || number > (kMaxRevision - (c - '0')) / 10) Refuse();
        number = number * 10 + (c - '0');
    }
    return number;
}
std::string Encode(const RetainedSelectionState& s) {
    const auto& o = s.owner;
    std::ostringstream out;
    out << "retained-assignment-device.v1\n" << s.revision << '\n' << (s.active ? "held" : "released") << '\n'
        << o.operation_id << '\n' << o.request_id << '\n' << o.device_id << '\n' << o.consumer_id << '\n'
        << o.lesson_row_id << '\n' << o.lesson_key << '\n' << o.lesson_version << '\n' << o.manifest_version << '\n'
        << o.manifest_checksum << '\n' << o.cache_key << '\n' << o.descriptor_checksum << '\n'
        << o.request_revision << '\n' << o.selection_revision << '\n' << o.assignment_id << '\n' << o.assignment_version << '\n';
    return out.str();
}
RetainedSelectionState Decode(const std::string& text) {
    std::istringstream in(text); std::vector<std::string> fields; std::string field;
    while (std::getline(in, field)) fields.push_back(field);
    if (fields.size() != 18 || fields[0] != "retained-assignment-device.v1" ||
        (fields[2] != "held" && fields[2] != "released")) Refuse();
    RetainedSelectionState s; s.revision = Number(fields[1]); s.active = fields[2] == "held";
    auto& o = s.owner;
    o.operation_id = fields[3]; o.request_id = fields[4]; o.device_id = fields[5]; o.consumer_id = fields[6];
    o.lesson_row_id = fields[7]; o.lesson_key = fields[8]; o.lesson_version = Number(fields[9]);
    o.manifest_version = fields[10]; o.manifest_checksum = fields[11]; o.cache_key = fields[12];
    o.descriptor_checksum = fields[13]; o.request_revision = Number(fields[14]); o.selection_revision = Number(fields[15]);
    o.assignment_id = fields[16]; o.assignment_version = Number(fields[17]);
    ValidateRetainedSelectionOwner(o);
    if (s.revision != o.selection_revision || s.active != !o.assignment_id.empty() || Encode(s) != text) Refuse();
    return s;
}
bool SameSelection(const RetainedSelectionOwner& a, const RetainedSelectionOwner& b) {
    return a.request_id == b.request_id && a.device_id == b.device_id && a.consumer_id == b.consumer_id && a.lesson_row_id == b.lesson_row_id &&
        a.lesson_key == b.lesson_key && a.lesson_version == b.lesson_version &&
        a.manifest_version == b.manifest_version && a.manifest_checksum == b.manifest_checksum &&
        a.cache_key == b.cache_key && a.descriptor_checksum == b.descriptor_checksum;
}
#ifndef ESP_PLATFORM
std::string StatePath() {
    const char* test_path = std::getenv("TBOT_RETAINED_TEST_STATE_PATH");
    return test_path ? test_path : TBOT_LESSON_ASSET_ROOT "/.retained-selection";
}
#endif
std::string ReadRecord() {
#ifdef ESP_PLATFORM
    nvs_handle_t handle;
    esp_err_t error = nvs_open("lesson_select", NVS_READONLY, &handle);
    if (error == ESP_ERR_NVS_NOT_FOUND) return {};
    if (error != ESP_OK) Refuse();
    std::size_t size = 0;
    error = nvs_get_blob(handle, "owner", nullptr, &size);
    if (error == ESP_ERR_NVS_NOT_FOUND) { nvs_close(handle); return {}; }
    if (error != ESP_OK || size == 0 || size > kMaxRecord) { nvs_close(handle); Refuse(); }
    std::string text(size, '\0');
    error = nvs_get_blob(handle, "owner", text.data(), &size);
    nvs_close(handle);
    if (error != ESP_OK || size != text.size()) Refuse();
    return text;
#else
    const int fd = open(StatePath().c_str(), O_RDONLY | O_NOFOLLOW);
    if (fd < 0) { if (errno == ENOENT) return {}; Refuse(); }
    struct stat info {};
    if (fstat(fd, &info) || !S_ISREG(info.st_mode) || info.st_size <= 0 || info.st_size > static_cast<off_t>(kMaxRecord)) {
        close(fd); Refuse();
    }
    std::string text(static_cast<std::size_t>(info.st_size), '\0');
    const auto count = read(fd, text.data(), text.size()); close(fd);
    if (count != static_cast<ssize_t>(text.size())) Refuse();
    return text;
#endif
}
void WriteRecord(const std::string& text) {
#ifdef ESP_PLATFORM
    nvs_handle_t handle;
    if (nvs_open("lesson_select", NVS_READWRITE, &handle) != ESP_OK) Refuse();
    const esp_err_t set = nvs_set_blob(handle, "owner", text.data(), text.size());
    const esp_err_t commit = set == ESP_OK ? nvs_commit(handle) : set;
    nvs_close(handle);
    if (commit != ESP_OK) { write_uncertain = true; Refuse(); }
#else
    const auto path = StatePath(); const auto tmp = path + ".tmp";
    const int fd = open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW, 0600);
    if (fd < 0) Refuse();
    const bool written = write(fd, text.data(), text.size()) == static_cast<ssize_t>(text.size());
    const bool synced = written && fsync(fd) == 0;
    const bool closed = close(fd) == 0;
    if (!synced || !closed || rename(tmp.c_str(), path.c_str()) != 0) { write_uncertain = true; Refuse(); }
    const auto parent = path.substr(0, path.rfind('/'));
    const int directory = open(parent.c_str(), O_RDONLY | O_DIRECTORY);
    const bool durable = directory >= 0 && fsync(directory) == 0;
    if (directory >= 0) close(directory);
    if (!durable) { write_uncertain = true; Refuse(); }
#endif
}
}  // namespace

void ValidateRetainedSelectionOwner(const RetainedSelectionOwner& o) {
    if (!Uuid(o.operation_id) || !Uuid(o.request_id) || !Uuid(o.device_id) || !Uuid(o.consumer_id) || !Uuid(o.lesson_row_id) ||
        !o.request_revision || o.request_revision > kMaxRevision || !o.selection_revision || o.selection_revision > kMaxRevision ||
        !o.lesson_version || o.lesson_version > 2147483647ULL || !Hex(o.manifest_checksum, 64) || !Hex(o.descriptor_checksum, 64) ||
        o.manifest_version.size() != 25 || o.manifest_version.substr(0, 24) != "teebot-lesson-renderer.v" ||
        o.manifest_version.back() < '1' || o.manifest_version.back() > '5' ||
        o.cache_key != o.lesson_key + "/v" + std::to_string(o.lesson_version) + "-" + o.manifest_checksum ||
        !IsCanonicalLessonCacheKey(o.cache_key) || (o.assignment_id.empty() != (o.assignment_version == 0)) ||
        (!o.assignment_id.empty() && (!Uuid(o.assignment_id) || o.assignment_version > kMaxRevision))) Refuse();
}
RetainedSelectionState ReadRetainedSelection() {
    if (write_uncertain) Refuse();
    const auto text = ReadRecord();
    return text.empty() ? RetainedSelectionState{} : Decode(text);
}
void ApplyRetainedSelection(const LessonAssetMutationLease& mutation, const RetainedSelectionOwner& owner, bool release) {
    if (!mutation) Refuse();
    ValidateRetainedSelectionOwner(owner);
    if (release != owner.assignment_id.empty()) Refuse();
    const auto current = ReadRetainedSelection();
    RetainedSelectionState next{owner.selection_revision, !release, owner};
    if (owner.selection_revision < current.revision) Refuse();
    if (owner.selection_revision == current.revision) {
        if (Encode(current) != Encode(next)) Refuse();
        return;
    }
    if (current.revision && current.owner.device_id != owner.device_id) Refuse();
    if (current.active && (!SameSelection(current.owner, owner) || owner.request_revision <= current.owner.request_revision)) Refuse();
    // A release may fence an acquisition that never arrived, but never another owner.
    if (release && current.revision && !SameSelection(current.owner, owner)) Refuse();
    if (!release && !current.active && current.revision && owner.request_id == current.owner.request_id) Refuse();
    WriteRecord(Encode(next));
}
bool RetainedSyncAllowed(const std::string& cache_key, std::uint64_t revision, const RetainedSelectionOwner* owner) {
    try {
        const auto current = ReadRetainedSelection();
        if (revision != current.revision) return false;
        if (!current.active) return owner == nullptr;
        if (!owner) return false;
        ValidateRetainedSelectionOwner(*owner);
        return cache_key == current.owner.cache_key && SameSelection(current.owner, *owner) &&
            owner->request_revision == current.owner.request_revision && owner->selection_revision == current.revision &&
            !owner->assignment_id.empty() && owner->assignment_id == current.owner.assignment_id &&
            owner->assignment_version == current.owner.assignment_version;
    } catch (...) { return false; }
}
bool RetainedEvictionAllowed(const std::string& cache_key) {
    try { const auto current = ReadRetainedSelection(); return !current.active || current.owner.cache_key != cache_key; }
    catch (...) { return false; }
}
