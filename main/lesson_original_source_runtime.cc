#include "lesson_original_source_runtime.h"

#include "lesson_original_source_allocator.h"
#include "lesson_original_source_pack_media.h"

#include <atomic>
#include <cstring>

#ifdef ESP_PLATFORM
#include "lesson_asset_storage_coordinator.h"
#endif

namespace tbot {
namespace {

std::mutex g_active_mutex;
std::atomic<OriginalSourceRuntime*> g_active_runtime{nullptr};
std::atomic<bool> g_advertised{false};
std::atomic<bool> g_timer_routes_v6{false};

bool StartsWith(const std::string& value, const char* prefix) {
    return value.compare(0, std::strlen(prefix), prefix) == 0;
}

}  // namespace

LessonCinematicError OriginalSourceErrorCode(const std::string& reason) {
    if (reason == "stale command") return LessonCinematicError::kStaleCommand;
    if (reason == "invalid state") return LessonCinematicError::kInvalidState;
    if (StartsWith(reason, "contract: ")) return LessonCinematicError::kMetadataMismatch;
    const std::string cause = StartsWith(reason, "media: ") ? reason.substr(7)
                              : StartsWith(reason, "scene: ") ? reason.substr(7) : reason;
    if (cause == "no memory") return LessonCinematicError::kInsufficientPsram;
    if (cause == "panel refused the frame") return LessonCinematicError::kPresentFailed;
    if (cause == "io" || cause == "integrity" || cause == "lease unavailable" ||
        cause == "lesson read lease unavailable" || cause == "scene is not in the pack" ||
        cause == "scene is shorter than the reference" || cause == "scene is longer than the reference" ||
        cause == "scene sha256 differs from the reference") {
        return LessonCinematicError::kFileRead;
    }
    if (cause == "decode" || cause == "original has no frames" || cause == "end") {
        return LessonCinematicError::kDecodeFailed;
    }
    return LessonCinematicError::kMetadataMismatch;
}

OriginalSourceRuntime::OriginalSourceRuntime(std::unique_ptr<OriginalSourceScenePlayer> player,
                                             std::function<void()> on_discard)
    : player_(std::move(player)), on_discard_(std::move(on_discard)) {}

OriginalSourceControlResult OriginalSourceRuntime::Handle(const char* frame_type, const cJSON* body,
                                                          std::uint64_t now_ms) {
    std::lock_guard<std::mutex> lock(mutex_);
    OriginalSourceControlResult result = player_->Handle(frame_type, body, now_ms);
    if (result.accepted && frame_type != nullptr && std::strcmp(frame_type, "lesson_prepare") == 0 && failed_) {
        // A new cue after a failure starts a new runtime generation.
        AdvanceRuntimeGeneration();
        failed_ = false;
    }
    return result;
}

bool OriginalSourceRuntime::Tick(std::uint64_t now_ms) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (failed_ || player_->controller().active_cue() == nullptr) return false;
    if (const char* reason = player_->Tick(now_ms)) {
        failed_ = true;
        const auto* cue = player_->controller().active_cue();
        pending_runtime_error_ = LessonLayeredRuntimeError{
            runtime_generation_, player_->controller().last_sequence(), cue != nullptr ? cue->cue_id : "",
            OriginalSourceErrorCode(reason)};
        return false;
    }
    return true;
}

void OriginalSourceRuntime::DiscardSession() {
    std::lock_guard<std::mutex> lock(mutex_);
    player_->Reset();
    if (on_discard_) on_discard_();
    AdvanceRuntimeGeneration();
    failed_ = false;
}

void OriginalSourceRuntime::AdvanceRuntimeGeneration() {
    ++runtime_generation_;
    if (runtime_generation_ == 0) ++runtime_generation_;
    pending_runtime_error_.reset();
}

std::uint64_t OriginalSourceRuntime::RuntimeGeneration() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return runtime_generation_;
}

std::optional<LessonLayeredRuntimeError> OriginalSourceRuntime::PendingRuntimeError() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return pending_runtime_error_;
}

bool OriginalSourceRuntime::AcknowledgeRuntimeError(std::uint64_t generation, std::uint64_t sequence) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!pending_runtime_error_ || pending_runtime_error_->generation != generation ||
        pending_runtime_error_->command_sequence_id != sequence) return false;
    pending_runtime_error_.reset();
    return true;
}

bool OriginalSourceRuntime::ReleaseFailedRuntimeResources(std::uint64_t generation, std::uint64_t sequence) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!pending_runtime_error_ || pending_runtime_error_->generation != generation ||
        pending_runtime_error_->command_sequence_id != sequence) return false;
    player_->Release();
    return true;
}

bool OriginalSourceRuntime::WithRuntimeGeneration(std::uint64_t generation, const std::function<void()>& operation) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (runtime_generation_ != generation) return false;
    operation();
    return true;
}

std::uint64_t OriginalSourceRuntime::PresentedFrames() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return player_->presented_frames();
}

OriginalSourcePlayerTimings OriginalSourceRuntime::Timings() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return player_->timings();
}

std::uint64_t OriginalSourceRuntime::OpenedStreams() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return player_->opened_streams();
}

void SetActiveOriginalSourceRuntime(OriginalSourceRuntime* runtime) {
    std::lock_guard<std::mutex> lock(g_active_mutex);
    g_active_runtime.store(runtime, std::memory_order_release);
}

OriginalSourceRuntime* ActiveOriginalSourceRuntime() { return g_active_runtime.load(std::memory_order_acquire); }

void SetOriginalSourceRendererAdvertised(bool advertised) {
    g_advertised.store(advertised, std::memory_order_release);
}

bool OriginalSourceRendererCapabilityReady() {
    return g_advertised.load(std::memory_order_acquire) && ActiveOriginalSourceRuntime() != nullptr;
}

void AddOriginalSourceRendererFeatures(cJSON* features, cJSON* renderers) {
    if (features == nullptr || renderers == nullptr || !OriginalSourceRendererCapabilityReady()) return;
    cJSON_AddItemToArray(renderers, cJSON_CreateString(kLessonRendererV6));
    cJSON* detail = cJSON_CreateObject();
    if (detail == nullptr) return;
    cJSON_AddBoolToObject(detail, "originalSourceScene", true);
    cJSON_AddBoolToObject(detail, "sdAssetPack", true);
    cJSON* capabilities = cJSON_AddArrayToObject(detail, "capabilities");
    for (const char* capability : kOriginalSourceDeviceCapabilities) {
        cJSON_AddItemToArray(capabilities, cJSON_CreateString(capability));
    }
    cJSON_AddItemToObject(features, "lessonRendererV6", detail);
}

bool TickActiveOriginalSourceRuntime(std::uint64_t now_ms) {
    std::lock_guard<std::mutex> lock(g_active_mutex);
    auto* runtime = g_active_runtime.load(std::memory_order_acquire);
    return runtime != nullptr && runtime->Tick(now_ms);
}

void SetLessonCinematicTimerRouteV6(bool enabled) { g_timer_routes_v6.store(enabled, std::memory_order_release); }
bool LessonCinematicTimerRoutesV6() { return g_timer_routes_v6.load(std::memory_order_acquire); }

#ifdef ESP_PLATFORM
namespace {

constexpr char kPackRoot[] = "/sdcard/tbot/lesson-assets";
// Decoder memory for a lesson session (BE08 R19): one PSRAM region reserved at the first
// decoder allocation, sized to the Farm scene's ~5.5 MB decoder peak so other PSRAM owners
// keep their share; freed blocks of >= 256 KiB are retained for the next open.
constexpr std::size_t kDecoderRegionBytes = 5632 * 1024;
constexpr std::size_t kDecoderRetentionBytes = 4 * 1024 * 1024;

struct ProductionSession {
    std::mutex mutex;
    std::string assignment_id, session_id;
    std::uint64_t generation = 0;
};

struct ProductionContext {
    ProductionSession session;
    OriginalSourceAllocationState allocations;
    std::unique_ptr<OriginalSourceRegionBackend> region;
    std::unique_ptr<OriginalSourceRetainingBackend> retention;
    std::unique_ptr<OriginalSourceAllocator> allocator;
    std::unique_ptr<OriginalSourcePackMedia> media;
    std::unique_ptr<OriginalSourceRuntime> runtime;
    LcdDisplayPresenter* panel = nullptr;
};

std::unique_ptr<ProductionContext> g_production;

LessonAssetReadLease ProductionLease() {
    if (!g_production) return {};
    std::lock_guard<std::mutex> lock(g_production->session.mutex);
    if (g_production->session.generation == 0) return {};
    return LessonAssetStorageCoordinator::GetInstance().TryRetainLessonSession(
        g_production->session.assignment_id, g_production->session.session_id, g_production->session.generation);
}

}  // namespace

bool InitializeProductionOriginalSourceRuntime(LcdDisplayPresenter* panel, TVideoTextRenderer* text) {
    if (panel == nullptr || g_production) return false;
    auto context = std::make_unique<ProductionContext>();
    context->panel = panel;
    context->region = std::make_unique<OriginalSourceRegionBackend>(
        OriginalSourceAllocator::EspBackend(), OriginalSourceRegionBackend::EspHeapOps(), kDecoderRegionBytes);
    context->retention =
        std::make_unique<OriginalSourceRetainingBackend>(context->region->backend(), kDecoderRetentionBytes);
    context->allocator = std::make_unique<OriginalSourceAllocator>(context->allocations, context->retention->backend());
    if (!context->allocator->Bind()) return false;
    context->media = std::make_unique<OriginalSourcePackMedia>(kPackRoot, ProductionLease, &context->allocations, nullptr);
    auto player = std::make_unique<OriginalSourceScenePlayer>(
        MakeOriginalSourcePackSceneLoader(kPackRoot, ProductionLease), context->media.get(), text,
        [panel](const std::uint16_t* rgb565, int width, int height) { return panel->Present(rgb565, width, height); },
        [panel](int width, int height, const std::function<void(std::uint16_t*)>& fill) {
            return panel->PresentInto(width, height, fill);
        });
    // The lesson session ended and every stream is closed: return the decoder memory.
    OriginalSourceRetainingBackend* retention = context->retention.get();
    OriginalSourceRegionBackend* region = context->region.get();
    context->runtime = std::make_unique<OriginalSourceRuntime>(std::move(player), [retention, region] {
        retention->ReleaseRetained();
        region->ReleaseRegion();
    });
    g_production = std::move(context);
    SetActiveOriginalSourceRuntime(g_production->runtime.get());
    return true;
}

void ConfigureProductionOriginalSourceSession(const std::string& assignment_id, const std::string& session_id,
                                              std::uint64_t generation) {
    if (!g_production) return;
    std::lock_guard<std::mutex> lock(g_production->session.mutex);
    g_production->session.assignment_id = assignment_id;
    g_production->session.session_id = session_id;
    g_production->session.generation = generation;
}

bool ProductionOriginalSourceAllocatorStats(OriginalSourceAllocatorStats* stats, bool reset_peak) {
    if (!g_production || stats == nullptr) return false;
    if (reset_peak) g_production->allocator->ResetPeak();
    *stats = g_production->allocator->stats();
    return true;
}

bool ProductionOriginalSourceRetentionStats(OriginalSourceRetentionStats* stats, OriginalSourceRegionStats* region) {
    if (!g_production || stats == nullptr || region == nullptr) return false;
    *stats = g_production->retention->stats();
    *region = g_production->region->stats();
    return true;
}

void ShutdownProductionOriginalSourceRuntime() {
    SetActiveOriginalSourceRuntime(nullptr);
    SetLessonCinematicTimerRouteV6(false);
    if (!g_production) return;
    g_production->runtime.reset();
    g_production->media.reset();
    g_production->allocator->Unbind();
    g_production.reset();
}
#else
bool InitializeProductionOriginalSourceRuntime(LcdDisplayPresenter*, TVideoTextRenderer*) { return false; }
void ConfigureProductionOriginalSourceSession(const std::string&, const std::string&, std::uint64_t) {}
bool ProductionOriginalSourceAllocatorStats(OriginalSourceAllocatorStats*, bool) { return false; }
bool ProductionOriginalSourceRetentionStats(OriginalSourceRetentionStats*, OriginalSourceRegionStats*) {
    return false;
}
void ShutdownProductionOriginalSourceRuntime() {
    SetActiveOriginalSourceRuntime(nullptr);
    SetLessonCinematicTimerRouteV6(false);
}
#endif

}  // namespace tbot
