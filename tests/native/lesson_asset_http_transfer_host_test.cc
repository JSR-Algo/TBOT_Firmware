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
    TestZeroLimitContinueFailsClosedInsteadOfSpinning();
    fs::remove_all(kRoot);
    std::cout << "lesson asset HTTP transfer host tests passed\n";
    return 0;
}
