#include "lesson_asset_download_staging.h"
#include "lesson_asset_http_transfer.h"
#include "lesson_storage_hil_controller.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

#include <esp_heap_caps.h>
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
    void SetHeader(const std::string& key, const std::string& value) override {
        if (key == "Range") range_header = value;
    }
    void SetContent(std::string&&) override {}
    void SetKeepAlive(bool) override {}
    bool Open(const std::string&, const std::string&) override {
        wdt_calls_at_open = g_esp_task_wdt_reset_calls;
        ++open_calls;
        if (!open_ok) return false;
        if (range_header.rfind("bytes=", 0) == 0) {
            requested_offset = static_cast<size_t>(
                std::stoull(range_header.substr(std::string("bytes=").size())));
            if (!ignore_range) position_ = requested_offset;
        }
        return true;
    }
    void Close() override { ++close_calls; }
    int Write(const char*, size_t) override { return -1; }
    int GetStatusCode() override {
        wdt_calls_at_status = g_esp_task_wdt_reset_calls;
        return open_calls == 0 || ignore_range ? 200 : resume_status;
    }
    std::string GetResponseHeader(const std::string& key) const override {
        if (key != "Content-Range" || open_calls == 0 || ignore_range) return {};
        const size_t start = mismatched_range ? requested_offset + 1 : requested_offset;
        return "bytes " + std::to_string(start) + "-" +
               std::to_string(body_.size() - 1) + "/" +
               std::to_string(body_.size());
    }
    size_t GetBodyLength() override {
        if (open_calls > 0 && !ignore_range && position_ <= body_.size()) {
            return body_.size() - position_;
        }
        return body_.size();
    }
    std::string ReadAll() override { return {}; }
    int GetLastError() override { return 0; }

    int Read(char* buffer, size_t want) override {
        read_wants.push_back(want);
        if (fail_once_at != std::numeric_limits<size_t>::max() &&
            position_ >= fail_once_at && !read_failed_) {
            wdt_calls_at_error = g_esp_task_wdt_reset_calls;
            read_failed_ = true;
            return -1;
        }
        if (read_failed_ && wdt_calls_at_first_resumed_read == 0) {
            wdt_calls_at_first_resumed_read = g_esp_task_wdt_reset_calls;
        }
        if (return_more_than_want) {
            const size_t count = std::min(want, body_.size() - position_);
            std::copy_n(body_.data() + position_, count, buffer);
            position_ += count;
            return static_cast<int>(want + 1);
        }
        const size_t remaining = body_.size() - position_;
        size_t count = std::min(want, remaining);
        if (!read_failed_ && fail_once_at != std::numeric_limits<size_t>::max() &&
            position_ < fail_once_at) {
            count = std::min(count, fail_once_at - position_);
        }
        if (count == 0) return 0;
        std::copy_n(body_.data() + position_, count, buffer);
        position_ += count;
        return static_cast<int>(count);
    }

    bool return_more_than_want = false;
    bool ignore_range = false;
    bool mismatched_range = false;
    bool open_ok = true;
    int resume_status = 206;
    size_t fail_once_at = std::numeric_limits<size_t>::max();
    size_t requested_offset = 0;
    int open_calls = 0;
    int close_calls = 0;
    int wdt_calls_at_error = 0;
    int wdt_calls_at_open = 0;
    int wdt_calls_at_status = 0;
    int wdt_calls_at_first_resumed_read = 0;
    std::string range_header;
    std::vector<size_t> read_wants;

private:
    std::vector<char> body_;
    size_t position_ = 0;
    bool read_failed_ = false;
};

std::string HostSha256(const std::string& bytes) {
    unsigned char digest[32] = {};
    for (std::size_t index = 0; index < bytes.size(); ++index) {
        digest[index % 32] ^= static_cast<unsigned char>(bytes[index]);
    }
    char hex[65];
    for (std::size_t index = 0; index < 32; ++index) {
        std::snprintf(hex + index * 2, sizeof(hex) - index * 2, "%02x", digest[index]);
    }
    hex[64] = '\0';
    return std::string(hex);
}

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

void TestReceiveTimeoutNearEofResumesAtExactOffsetAndVerifiesSha() {
    const std::string body(4301312, 'r');
    FakeHttp http(body);
    http.fail_once_at = 4245824;
    const std::string destination = std::string(kRoot) + "/resume-near-eof.bin";
    size_t bytes = 0;
    {
        LessonAssetDownloadStagingFile staging(destination);
        Expect(Transfer(http, staging, nullptr, true, body.size(), bytes),
               "receive timeout near EOF did not resume");
        Expect(http.range_header == "bytes=4245824-",
               "resume did not request the exact persisted byte offset");
        Expect(http.open_calls == 1 && http.close_calls == 1,
               "receive timeout did not use one bounded reconnect");
        Expect(bytes == body.size(), "resumed transfer reported the wrong final size");
        Expect(Read(staging.path()) == body, "resumed transfer duplicated or skipped bytes");
        CommitVerifiedLessonAssetDownload(
            staging, nullptr, destination, HostSha256(body));
    }
    Expect(fs::file_size(destination) == body.size(),
           "verified resumed asset has the wrong committed size");
    Expect(VerifyLessonAssetSha256(destination, HostSha256(body)),
           "verified resumed asset has the wrong committed checksum");
}

void TestServerIgnoringOrMismatchingRangeFailsClosed() {
    for (const bool ignore_range : {true, false}) {
        FakeHttp http("replacement-body");
        http.fail_once_at = 5;
        http.ignore_range = ignore_range;
        http.mismatched_range = !ignore_range;
        const std::string destination = std::string(kRoot) +
            (ignore_range ? "/ignored-range.bin" : "/mismatched-range.bin");
        Write(destination, "known-good");
        size_t bytes = 0;
        {
            LessonAssetDownloadStagingFile staging(destination);
            Expect(!Transfer(http, staging, nullptr, true, 16, bytes),
                   "unsafe range response was accepted");
            Expect(bytes == 5, "unsafe range response changed persisted byte count");
            Expect(!fs::exists(staging.path()), "unsafe range response left staging file");
            Expect(!fs::exists(staging.path() + ".tmp"),
                   "unsafe range response left transfer temp file");
        }
        Expect(Read(destination) == "known-good",
               "unsafe range response replaced last-known-good asset");
    }
}

void TestStalePowerLossTempIsRestartedSafely() {
    const std::string body = "complete-replacement";
    FakeHttp http(body);
    const std::string destination = std::string(kRoot) + "/power-loss.bin";
    Write(destination, "known-good");
    size_t bytes = 0;
    {
        LessonAssetDownloadStagingFile staging(destination);
        Write(staging.path() + ".tmp", "stale-partial");
        Expect(Transfer(http, staging, nullptr, true, body.size(), bytes),
               "stale power-loss temp blocked a clean transfer");
        Expect(Read(staging.path()) == body,
               "stale power-loss temp was appended to the new transfer");
        Expect(!fs::exists(staging.path() + ".tmp"),
               "successful transfer left its temp checkpoint");
    }
    Expect(Read(destination) == "known-good",
           "unverified transfer replaced last-known-good asset");
}

void TestReconnectReusesSingleDownloadBuffer() {
    g_heap_caps_malloc_calls = 0;
    g_heap_caps_last_size = 0;
    g_heap_caps_last_caps = 0;
    FakeHttp http(std::string(16384, 'b'));
    http.fail_once_at = 8192;
    const std::string destination = std::string(kRoot) + "/single-buffer.bin";
    size_t bytes = 0;
    {
        LessonAssetDownloadStagingFile staging(destination);
        Expect(Transfer(http, staging, nullptr, true, 16384, bytes),
               "single-buffer resume transfer failed");
    }
    Expect(g_heap_caps_malloc_calls == 1,
           "reconnect allocated an extra download buffer");
    Expect(g_heap_caps_last_size == 4096,
           "download buffer size changed unexpectedly");
    Expect((g_heap_caps_last_caps & MALLOC_CAP_INTERNAL) != 0,
           "host fallback did not request the bounded internal buffer");
}

void TestCheckpointAndReconnectFeedWatchdogAroundBlockingSteps() {
    g_esp_task_wdt_reset_calls = 0;
    FakeHttp http(std::string(12288, 'w'));
    http.fail_once_at = 4096;
    const std::string destination = std::string(kRoot) + "/resume-watchdog.bin";
    size_t bytes = 0;
    {
        LessonAssetDownloadStagingFile staging(destination);
        Expect(Transfer(http, staging, nullptr, true, 12288, bytes),
               "watchdog regression transfer failed");
    }
    Expect(http.wdt_calls_at_open >= http.wdt_calls_at_error + 4,
           "checkpoint flush/fsync and reconnect open were not watchdog-fed");
    Expect(http.wdt_calls_at_status > http.wdt_calls_at_open,
           "status wait was not watchdog-fed after reconnect open");
    Expect(http.wdt_calls_at_first_resumed_read > http.wdt_calls_at_status,
           "watchdog was not reset after reconnect status wait");
}

void TestDeclaredAssetSizePolicyCoversFarmV8AndRejectsOverflow() {
    constexpr size_t kFarmV8Bytes = 116 * 1024 * 1024;
    Expect(IsLessonAssetDeclaredFileSizeAllowed(kFarmV8Bytes),
           "firmware file policy rejects the Farm v8 asset");
    Expect(!IsLessonAssetDeclaredFileSizeAllowed(0),
           "firmware file policy accepts an empty declared asset");
    Expect(!IsLessonAssetDeclaredFileSizeAllowed(LessonAssetMaxFileBytes() + 1),
           "firmware file policy accepts an oversized declared asset");
    Expect(IsLessonAssetDeclaredFileSizeAllowed(LessonAssetMaxFileBytes()),
           "firmware file policy rejects its exact configured limit");

    size_t aggregate = 0;
    Expect(AccumulateLessonAssetDeclaredSize(0, kFarmV8Bytes, aggregate) &&
               aggregate == kFarmV8Bytes,
           "firmware aggregate policy rejects the Farm v8 pack");
    Expect(!AccumulateLessonAssetDeclaredSize(
               LessonAssetMaxPackBytes(), 1, aggregate),
           "firmware aggregate policy accepts overflow beyond its limit");
    Expect(AccumulateLessonAssetDeclaredSize(
               LessonAssetMaxPackBytes() - LessonAssetMaxFileBytes(),
               LessonAssetMaxFileBytes(),
               aggregate) && aggregate == LessonAssetMaxPackBytes(),
           "firmware aggregate policy rejects its exact configured limit");
    Expect(!AccumulateLessonAssetDeclaredSize(
               std::numeric_limits<size_t>::max(), 1, aggregate),
           "firmware aggregate policy permits size_t overflow");
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
    TestReceiveTimeoutNearEofResumesAtExactOffsetAndVerifiesSha();
    TestServerIgnoringOrMismatchingRangeFailsClosed();
    TestStalePowerLossTempIsRestartedSafely();
    TestReconnectReusesSingleDownloadBuffer();
    TestCheckpointAndReconnectFeedWatchdogAroundBlockingSteps();
    TestDeclaredAssetSizePolicyCoversFarmV8AndRejectsOverflow();
    TestZeroLimitContinueFailsClosedInsteadOfSpinning();
    fs::remove_all(kRoot);
    std::cout << "lesson asset HTTP transfer host tests passed\n";
    return 0;
}
