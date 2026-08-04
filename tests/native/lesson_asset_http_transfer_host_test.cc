#include "lesson_asset_download_staging.h"
#include "lesson_asset_http_transfer.h"
#include "lesson_storage_hil_controller.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

#include <esp_task_wdt.h>

namespace fs = std::filesystem;

struct LessonStorageHilControllerTestPeer {
    static void ExhaustObservationSequence(LessonStorageHilController& controller) {
        controller.next_sequence_ = std::numeric_limits<std::uint64_t>::max();
    }
};

namespace {

#ifndef TBOT_LESSON_ASSET_TRANSFER_TEST_ROOT
#define TBOT_LESSON_ASSET_TRANSFER_TEST_ROOT "/tmp/tbot-lesson-asset-transfer-host"
#endif
constexpr const char* kRoot = TBOT_LESSON_ASSET_TRANSFER_TEST_ROOT;
constexpr const char* kHilKey =
    "hil-task14/v1-0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";

void Expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

void Write(const std::string& path, const std::string& bytes) {
    std::ofstream stream(path, std::ios::binary);
    stream << bytes;
}

std::string Read(const std::string& path) {
    std::ifstream stream(path, std::ios::binary);
    return std::string(
        std::istreambuf_iterator<char>(stream),
        std::istreambuf_iterator<char>());
}

class FakeHttp final : public Http {
public:
    explicit FakeHttp(std::string body) : body_(body.begin(), body.end()) {}

    void SetTimeout(int) override {}
    void SetHeader(const std::string&, const std::string&) override {}
    void SetContent(std::string&&) override {}
    void SetKeepAlive(bool) override {}
    bool Open(const std::string&, const std::string&) override { return true; }
    void Close() override {}
    int Write(const char*, size_t) override { return -1; }
    int GetStatusCode() override { return 200; }
    std::string GetResponseHeader(const std::string&) const override { return {}; }
    size_t GetBodyLength() override { return body_.size(); }
    std::string ReadAll() override { return {}; }
    int GetLastError() override { return 0; }

    int Read(char* buffer, size_t want) override {
        read_wants.push_back(want);
        if (return_more_than_want) {
            const size_t count = std::min(want, body_.size() - position_);
            std::copy_n(body_.data() + position_, count, buffer);
            position_ += count;
            return static_cast<int>(want + 1);
        }
        const size_t remaining = body_.size() - position_;
        const size_t count = std::min(want, remaining);
        if (count == 0) return 0;
        std::copy_n(body_.data() + position_, count, buffer);
        position_ += count;
        return static_cast<int>(count);
    }

    bool return_more_than_want = false;
    std::vector<size_t> read_wants;

private:
    std::vector<char> body_;
    size_t position_ = 0;
};

class GeneratedHttp final : public Http {
public:
    explicit GeneratedHttp(size_t size) : size_(size) {}
    void SetTimeout(int) override {}
    void SetHeader(const std::string&, const std::string&) override {}
    void SetContent(std::string&&) override {}
    void SetKeepAlive(bool) override {}
    bool Open(const std::string&, const std::string&) override { return true; }
    void Close() override {}
    int Write(const char*, size_t) override { return -1; }
    int GetStatusCode() override { return 200; }
    std::string GetResponseHeader(const std::string&) const override { return {}; }
    size_t GetBodyLength() override { return size_; }
    std::string ReadAll() override { read_all_called = true; return {}; }
    int GetLastError() override { return 0; }
    int Read(char* buffer, size_t want) override {
        max_want = std::max(max_want, want);
        const size_t count = std::min(want, size_ - position_);
        if (count == 0) return 0;
        std::fill_n(buffer, count, 't');
        position_ += count;
        return static_cast<int>(count);
    }
    size_t max_want = 0;
    bool read_all_called = false;
private:
    size_t size_;
    size_t position_ = 0;
};

class ResumableHttp final : public Http {
public:
    struct Response {
        int status = 200;
        size_t start = 0;
        size_t reported_body_length = 0;
        std::string content_range;
        size_t fail_absolute_offset = std::numeric_limits<size_t>::max();
        bool fail_immediately = false;
    };

    explicit ResumableHttp(std::string body) : body_(std::move(body)) {}

    void PushResponse(Response response) {
        if (response.reported_body_length == 0 && response.start <= body_.size()) {
            response.reported_body_length = body_.size() - response.start;
        }
        responses_.push_back(std::move(response));
    }

    void SetTimeout(int) override {}
    void SetHeader(const std::string& key, const std::string& value) override {
        headers_[key] = value;
    }
    void SetContent(std::string&&) override {}
    void SetKeepAlive(bool) override {}
    bool Open(const std::string& method, const std::string& url) override {
        open_methods.push_back(method);
        open_urls.push_back(url);
        observed_ranges.push_back(Header("Range"));
        if (open_count >= responses_.size()) return false;
        active_ = responses_[open_count++];
        position_ = active_.start;
        return true;
    }
    void Close() override { close_count += 1; }
    int Write(const char*, size_t) override { return -1; }
    int GetStatusCode() override { return active_.status; }
    std::string GetResponseHeader(const std::string& key) const override {
        if (key == "Content-Range") return active_.content_range;
        return {};
    }
    size_t GetBodyLength() override { return active_.reported_body_length; }
    std::string ReadAll() override { return {}; }
    int GetLastError() override { return 0; }

    int Read(char* buffer, size_t want) override {
        read_wants.push_back(want);
        if (active_.fail_immediately || position_ == active_.fail_absolute_offset) {
            return -1;
        }
        size_t limit = body_.size();
        if (active_.fail_absolute_offset != std::numeric_limits<size_t>::max()) {
            limit = std::min(limit, active_.fail_absolute_offset);
        }
        const size_t remaining = limit - position_;
        const size_t count = std::min(want, remaining);
        if (count == 0) return 0;
        std::copy_n(body_.data() + position_, count, buffer);
        position_ += count;
        return static_cast<int>(count);
    }

    size_t open_count = 0;
    size_t close_count = 0;
    std::vector<std::string> observed_ranges;
    std::vector<std::string> open_methods;
    std::vector<std::string> open_urls;
    std::vector<size_t> read_wants;

private:
    std::string Header(const std::string& key) const {
        const auto found = headers_.find(key);
        return found == headers_.end() ? std::string() : found->second;
    }

    std::string body_;
    std::vector<Response> responses_;
    std::map<std::string, std::string> headers_;
    Response active_;
    size_t position_ = 0;
};

void ArmSync(
    LessonStorageHilCheckpoint checkpoint,
    LessonStorageHilAction action,
    std::uint32_t threshold = 0,
    std::uint32_t declared_asset_bytes = 0
) {
    auto& controller = LessonStorageHilController::GetInstance();
    controller.Reset();
    const auto result = controller.Arm({
        kHilKey,
        LessonStorageHilOperation::kSync,
        checkpoint,
        action,
        threshold,
        declared_asset_bytes,
        action == LessonStorageHilAction::kPause ? 5U : 0U,
    });
    Expect(result.code == LessonStorageHilArmCode::kArmed, "sync fault did not arm");
}

void ExpectConsumed(const char* message) {
    const auto status = LessonStorageHilController::GetInstance().Status();
    Expect(status.reached && status.consumed && !status.armed, message);
}

bool Transfer(
    FakeHttp& http,
    LessonAssetDownloadStagingFile& staging,
    const char* cache_key,
    bool has_declared_size,
    size_t declared_size,
    size_t& bytes_out
) {
    try {
        DownloadLessonAssetHttpBodyToFile(
            http,
            cache_key,
            has_declared_size,
            declared_size,
            "https://assets.example/test.bin",
            staging.path(),
            bytes_out);
        return true;
    } catch (const std::runtime_error&) {
        return false;
    }
}

void TestBeforeFirstWriteFailAndNoSpaceCleanAllTemporaryState() {
    for (const auto action : {
             LessonStorageHilAction::kFail,
             LessonStorageHilAction::kNoSpace,
         }) {
        const std::string destination = std::string(kRoot) + "/before-write.bin";
        Write(destination, "known-good");
        ArmSync(LessonStorageHilCheckpoint::kBeforeDownloadWrite, action);
        FakeHttp http("replacement");
        size_t bytes = 999;
        {
            LessonAssetDownloadStagingFile staging(destination);
            Expect(!Transfer(http, staging, kHilKey, true, 11, bytes),
                   "before-write injection did not stop real transfer");
            Expect(bytes == 0, "before-write injection wrote bytes");
            Expect(!fs::exists(staging.path()), "before-write left .download");
            Expect(!fs::exists(staging.path() + ".tmp"), "before-write left .tmp");
        }
        Expect(Read(destination) == "known-good", "before-write replaced destination");
        ExpectConsumed("before-write arm was not consumed");
    }
}

void TestAfterBytesCapsTheActualHttpReadAndCleansNoSpaceFailure() {
    const std::string destination = std::string(kRoot) + "/after-bytes.bin";
    Write(destination, "known-good");
    ArmSync(LessonStorageHilCheckpoint::kAfterDownloadBytes,
            LessonStorageHilAction::kNoSpace, 5, 11);
    FakeHttp http("replacement");
    size_t bytes = 0;
    {
        LessonAssetDownloadStagingFile staging(destination);
        Expect(!Transfer(http, staging, kHilKey, true, 11, bytes),
               "after-bytes injection did not stop real transfer");
        Expect(bytes == 5, "real transfer overshot byte checkpoint");
        Expect(http.read_wants == std::vector<size_t>{5},
               "real Http::Read want was not capped exactly");
        Expect(!fs::exists(staging.path()), "after-bytes left .download");
        Expect(!fs::exists(staging.path() + ".tmp"), "after-bytes left .tmp");
    }
    Expect(Read(destination) == "known-good", "after-bytes replaced destination");
    ExpectConsumed("after-bytes arm was not consumed");
}

void TestMissingAndMismatchedDeclaredSizeCannotConsumeAfterBytesArm() {
    for (const auto declared : {size_t{0}, size_t{12}}) {
        ArmSync(LessonStorageHilCheckpoint::kAfterDownloadBytes,
                LessonStorageHilAction::kFail, 5, 11);
        FakeHttp http("replacement");
        const std::string destination = std::string(kRoot) + "/declared-" +
                                        std::to_string(declared) + ".bin";
        size_t bytes = 0;
        {
            LessonAssetDownloadStagingFile staging(destination);
            const bool transferred =
                Transfer(http, staging, kHilKey, declared != 0, declared, bytes);
            Expect(transferred == (declared == 0),
                   "nonmatching declared size did not fail closed");
        }
        const auto status = LessonStorageHilController::GetInstance().Status();
        Expect(status.armed && !status.reached && !status.consumed,
               "nonmatching declared size consumed arm");
        LessonStorageHilController::GetInstance().Reset();
    }
}

void TestDeclaredSizeMustMatchDownloadedBytesBeforeCommit() {
    {
        const std::string destination = std::string(kRoot) + "/declared-match.bin";
        size_t bytes = 0;
        LessonAssetDownloadStagingFile staging(destination);
        FakeHttp http("replacement");
        Expect(Transfer(http, staging, nullptr, true, 11, bytes),
               "matching declared size rejected transfer");
        Expect(bytes == 11, "matching declared size reported wrong bytes");
        Expect(Read(staging.path()) == "replacement",
               "matching declared size did not commit temp transfer");
    }

    {
        const std::string destination = std::string(kRoot) + "/declared-mismatch.bin";
        size_t bytes = 0;
        LessonAssetDownloadStagingFile staging(destination);
        FakeHttp http("replacement");
        Expect(!Transfer(http, staging, nullptr, true, 12, bytes),
               "mismatched declared size accepted transfer");
        Expect(bytes == 11, "mismatched declared size hid actual bytes");
        Expect(!fs::exists(staging.path()),
               "mismatched declared size left staged download");
        Expect(!fs::exists(staging.path() + ".tmp"),
               "mismatched declared size left temp file");
    }
}

void TestSampleEmptyContextCannotConsumeCanonicalArm() {
    ArmSync(LessonStorageHilCheckpoint::kBeforeDownloadWrite,
            LessonStorageHilAction::kFail);
    FakeHttp http("sample");
    const std::string destination = std::string(kRoot) + "/sample.bin";
    size_t bytes = 0;
    {
        LessonAssetDownloadStagingFile staging(destination);
        Expect(Transfer(http, staging, nullptr, false, 0, bytes),
               "sample empty context affected real transfer");
    }
    const auto status = LessonStorageHilController::GetInstance().Status();
    Expect(status.armed && !status.reached && !status.consumed,
           "sample empty context consumed canonical arm");
    LessonStorageHilController::GetInstance().Reset();
}

void TestOversizedHttpReadFailsBeforeWriting() {
    FakeHttp http("replacement");
    http.return_more_than_want = true;
    const std::string destination = std::string(kRoot) + "/oversized-read.bin";
    Write(destination, "known-good");
    size_t bytes = 0;
    {
        LessonAssetDownloadStagingFile staging(destination);
        Expect(!Transfer(http, staging, nullptr, false, 0, bytes),
               "oversized Http::Read result was accepted");
        Expect(bytes == 0, "oversized Http::Read wrote bytes");
        Expect(!fs::exists(staging.path()), "oversized read left .download");
        Expect(!fs::exists(staging.path() + ".tmp"), "oversized read left .tmp");
    }
    Expect(Read(destination) == "known-good", "oversized read replaced destination");
}

void TestCinematicMp4LargerThanLegacyImageCapStreamsToSd() {
    const std::string body(3 * 1024 * 1024, 'm');
    FakeHttp http(body);
    const std::string destination = std::string(kRoot) + "/cinematic.mp4";
    size_t bytes = 0;
    {
        LessonAssetDownloadStagingFile staging(destination);
        Expect(Transfer(http, staging, nullptr, true, body.size(), bytes),
               "cinematic MP4 above the legacy image cap was rejected");
        Expect(bytes == body.size(), "cinematic MP4 byte count was truncated");
        Expect(fs::file_size(staging.path()) == body.size(),
               "cinematic MP4 was not streamed completely to SD staging");
    }
}

void TestProductionTrgbStreamsWithBoundedBufferAndRejectsInvalidSizes() {
    constexpr size_t kTrgbBytes = 29186048;
    GeneratedHttp http(kTrgbBytes);
    const std::string destination = std::string(kRoot) + "/production.trgb";
    size_t bytes = 0;
    {
        LessonAssetDownloadStagingFile staging(destination);
        try {
            DownloadLessonAssetHttpBodyToFile(
                http, nullptr, true, kTrgbBytes, "https://assets.example/cue.trgb",
                staging.path(), bytes, "application/vnd.tbot.rgb565-indexed");
        } catch (...) {
            Expect(false, "valid production TRGB transfer was rejected");
        }
        Expect(bytes == kTrgbBytes && fs::file_size(staging.path()) == kTrgbBytes,
               "production TRGB was not completely streamed to SD");
        Expect(http.max_want <= 4096 && !http.read_all_called,
               "TRGB transfer allocated or requested the whole file");
    }

    for (const size_t invalid : std::vector<size_t>{
             kTrgbBytes + 1, static_cast<size_t>(64U * 1024U * 1024U + 1U)}) {
        GeneratedHttp rejected(invalid);
        const std::string rejected_destination =
            std::string(kRoot) + "/rejected-" + std::to_string(invalid) + ".trgb";
        size_t rejected_bytes = 0;
        LessonAssetDownloadStagingFile staging(rejected_destination);
        bool failed = false;
        try {
            DownloadLessonAssetHttpBodyToFile(
                rejected, nullptr, true, invalid, "https://assets.example/rejected.trgb",
                staging.path(), rejected_bytes, "application/vnd.tbot.rgb565-indexed");
        } catch (...) {
            failed = true;
        }
        Expect(failed && rejected_bytes == 0 && rejected.max_want == 0,
               "invalid TRGB size reached the streaming loop");
    }

    constexpr size_t kFarmV8Bytes = 116U * 1024U * 1024U;
    Expect(IsLessonAssetDeclaredFileSizeAllowed(kFarmV8Bytes),
           "configured file policy rejects the Farm v8 asset");
    Expect(!IsLessonAssetDeclaredFileSizeAllowed(LessonAssetMaxFileBytes() + 1),
           "configured file policy accepts an oversized asset");
    size_t aggregate = 0;
    Expect(AccumulateLessonAssetDeclaredSize(0, kFarmV8Bytes, aggregate) &&
               aggregate == kFarmV8Bytes,
           "configured aggregate policy rejects the Farm v8 pack");
    Expect(!AccumulateLessonAssetDeclaredSize(LessonAssetMaxPackBytes(), 1, aggregate),
           "configured aggregate policy accepts overflow");

    GeneratedHttp cinematic(4U * 1024U * 1024U + 1U);
    size_t cinematic_bytes = 0;
    LessonAssetDownloadStagingFile cinematic_staging(std::string(kRoot) + "/cinematic.mp4");
    bool cinematic_failed = false;
    try {
        DownloadLessonAssetHttpBodyToFile(
            cinematic, nullptr, true, cinematic.GetBodyLength(),
            "https://assets.example/cinematic.mp4", cinematic_staging.path(),
            cinematic_bytes, "video/mp4");
    } catch (...) {
        cinematic_failed = true;
    }
    Expect(!cinematic_failed && cinematic_bytes == cinematic.GetBodyLength(),
           "configured policy still enforces the legacy MP4 cap");
}

void TestZeroLimitContinueFailsClosedInsteadOfSpinning() {
    ArmSync(LessonStorageHilCheckpoint::kAfterDownloadBytes,
            LessonStorageHilAction::kPause, 5, 11);
    auto& controller = LessonStorageHilController::GetInstance();
    LessonStorageHilControllerTestPeer::ExhaustObservationSequence(controller);
    FakeHttp http("replacement");
    const std::string destination = std::string(kRoot) + "/zero-limit.bin";
    size_t bytes = 0;
    {
        LessonAssetDownloadStagingFile staging(destination);
        Expect(!Transfer(http, staging, kHilKey, true, 11, bytes),
               "zero-limit continue did not fail closed");
        Expect(http.read_wants == std::vector<size_t>{5},
               "zero-limit path attempted another read after threshold");
        Expect(!fs::exists(staging.path() + ".tmp"), "zero-limit path left .tmp");
    }
    controller.Reset();
}

void TestInterruptedTransferResumesFromPartialOffset() {
    constexpr size_t kInterruptAt = 546358;
    const std::string body(600000, 'r');
    const std::string destination = std::string(kRoot) + "/range-resume.bin";
    size_t bytes = 0;
    ResumableHttp http(body);
    http.PushResponse({200, 0, body.size(), "", kInterruptAt, false});
    http.PushResponse({
        206,
        kInterruptAt,
        body.size() - kInterruptAt,
        "bytes 546358-599999/600000",
        std::numeric_limits<size_t>::max(),
        false,
    });

    LessonAssetDownloadStagingFile staging(destination);
    try {
        DownloadLessonAssetHttpBodyToFile(
            http, nullptr, true, body.size(), "https://assets.example/range.bin",
            staging.path(), bytes);
    } catch (...) {
        Expect(false, "interrupted transfer did not resume successfully");
    }

    Expect(bytes == body.size(), "resumed transfer reported wrong byte count");
    Expect(Read(staging.path()) == body, "resumed transfer did not append exact bytes");
    Expect(http.open_count == 2, "resumed transfer used wrong open count");
    Expect(http.observed_ranges == std::vector<std::string>{"", "bytes=546358-"},
           "resumed transfer did not reopen with exact Range offset");
}

void TestCheckpointAndReconnectFeedWatchdogAroundBlockingSteps() {
    g_esp_task_wdt_status_result = ESP_OK;
    g_esp_task_wdt_reset_calls = 0;
    constexpr size_t kInterruptAt = 4096;
    const std::string body(12288, 'w');
    ResumableHttp http(body);
    http.PushResponse({200, 0, body.size(), "", kInterruptAt, false});
    http.PushResponse({
        206,
        kInterruptAt,
        body.size() - kInterruptAt,
        "bytes 4096-12287/12288",
        std::numeric_limits<size_t>::max(),
        false,
    });
    size_t bytes = 0;
    LessonAssetDownloadStagingFile staging(std::string(kRoot) + "/resume-watchdog.bin");
    DownloadLessonAssetHttpBodyToFile(
        http, nullptr, true, body.size(), "https://assets.example/watchdog.bin",
        staging.path(), bytes);
    Expect(g_esp_task_wdt_reset_calls >= 16,
           "checkpoint and reconnect blocking steps were not watchdog-fed");
    g_esp_task_wdt_status_result = ESP_ERR_NOT_FOUND;
}

void TestResumeRejectsUnexpectedStatusAndCleansPartial() {
    constexpr size_t kInterruptAt = 546358;
    const std::string body(600000, 's');
    const std::string destination = std::string(kRoot) + "/resume-status.bin";
    Write(destination, "known-good");
    size_t bytes = 0;
    ResumableHttp http(body);
    http.PushResponse({200, 0, body.size(), "", kInterruptAt, false});
    http.PushResponse({200, kInterruptAt, body.size() - kInterruptAt, "", 0, false});

    {
        LessonAssetDownloadStagingFile staging(destination);
        bool failed = false;
        try {
            DownloadLessonAssetHttpBodyToFile(
                http, nullptr, true, body.size(), "https://assets.example/status.bin",
                staging.path(), bytes);
        } catch (...) {
            failed = true;
        }
        Expect(failed, "resume accepted non-206 response");
        Expect(http.observed_ranges == std::vector<std::string>{"", "bytes=546358-"},
               "resume status failure did not request the partial offset");
        Expect(!fs::exists(staging.path()), "resume status failure left .download");
        Expect(!fs::exists(staging.path() + ".tmp"), "resume status failure left .tmp");
    }
    Expect(Read(destination) == "known-good", "resume status failure replaced destination");
}

void TestResumeRejectsOversizeResponseAndCleansPartial() {
    constexpr size_t kInterruptAt = 546358;
    constexpr size_t kDeclaredSize = 600000;
    const std::string body(kDeclaredSize + 1, 'o');
    const std::string destination = std::string(kRoot) + "/resume-oversize.bin";
    Write(destination, "known-good");
    size_t bytes = 0;
    ResumableHttp http(body);
    http.PushResponse({200, 0, kDeclaredSize, "", kInterruptAt, false});
    http.PushResponse({
        206,
        kInterruptAt,
        body.size() - kInterruptAt,
        "bytes 546358-600000/600001",
        std::numeric_limits<size_t>::max(),
        false,
    });

    {
        LessonAssetDownloadStagingFile staging(destination);
        bool failed = false;
        try {
            DownloadLessonAssetHttpBodyToFile(
                http, nullptr, true, kDeclaredSize, "https://assets.example/oversize.bin",
                staging.path(), bytes);
        } catch (...) {
            failed = true;
        }
        Expect(failed, "resume accepted response beyond declared size");
        Expect(!fs::exists(staging.path()), "resume oversize failure left .download");
        Expect(!fs::exists(staging.path() + ".tmp"), "resume oversize failure left .tmp");
    }
    Expect(Read(destination) == "known-good", "resume oversize failure replaced destination");
}

void TestExhaustedResumeRetriesCleanPartialStaging() {
    constexpr size_t kInterruptAt = 546358;
    const std::string body(600000, 'x');
    const std::string destination = std::string(kRoot) + "/resume-exhausted.bin";
    Write(destination, "known-good");
    size_t bytes = 0;
    ResumableHttp http(body);
    http.PushResponse({200, 0, body.size(), "", kInterruptAt, false});
    http.PushResponse({
        206,
        kInterruptAt,
        body.size() - kInterruptAt,
        "bytes 546358-599999/600000",
        kInterruptAt,
        false,
    });
    http.PushResponse({
        206,
        kInterruptAt,
        body.size() - kInterruptAt,
        "bytes 546358-599999/600000",
        kInterruptAt,
        false,
    });
    http.PushResponse({
        206,
        kInterruptAt,
        body.size() - kInterruptAt,
        "bytes 546358-599999/600000",
        kInterruptAt,
        false,
    });

    {
        LessonAssetDownloadStagingFile staging(destination);
        bool failed = false;
        try {
            DownloadLessonAssetHttpBodyToFile(
                http, nullptr, true, body.size(), "https://assets.example/exhausted.bin",
                staging.path(), bytes);
        } catch (...) {
            failed = true;
        }
        Expect(failed, "exhausted resume retries accepted incomplete transfer");
        Expect(http.open_count == 4, "resume retry budget was not exactly three attempts");
        Expect(!fs::exists(staging.path()), "exhausted resume left .download");
        Expect(!fs::exists(staging.path() + ".tmp"), "exhausted resume left .tmp");
    }
    Expect(Read(destination) == "known-good", "exhausted resume replaced destination");
}

}  // namespace

int main() {
    fs::remove_all(kRoot);
    fs::create_directories(kRoot);
    TestBeforeFirstWriteFailAndNoSpaceCleanAllTemporaryState();
    TestAfterBytesCapsTheActualHttpReadAndCleansNoSpaceFailure();
    TestMissingAndMismatchedDeclaredSizeCannotConsumeAfterBytesArm();
    TestDeclaredSizeMustMatchDownloadedBytesBeforeCommit();
    TestSampleEmptyContextCannotConsumeCanonicalArm();
    TestOversizedHttpReadFailsBeforeWriting();
    TestCinematicMp4LargerThanLegacyImageCapStreamsToSd();
    TestProductionTrgbStreamsWithBoundedBufferAndRejectsInvalidSizes();
    TestZeroLimitContinueFailsClosedInsteadOfSpinning();
    TestInterruptedTransferResumesFromPartialOffset();
    TestCheckpointAndReconnectFeedWatchdogAroundBlockingSteps();
    TestResumeRejectsUnexpectedStatusAndCleansPartial();
    TestResumeRejectsOversizeResponseAndCleansPartial();
    TestExhaustedResumeRetriesCleanPartialStaging();
    fs::remove_all(kRoot);
    std::cout << "lesson asset HTTP transfer host tests passed\n";
    return 0;
}
