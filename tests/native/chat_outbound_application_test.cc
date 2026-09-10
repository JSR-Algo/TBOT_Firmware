#include "audio/chat_uplink_authorization.h"
#include "audio/audio_reset_epoch_publication.h"
#include "audio/audio_decode_fence.h"
#define private public
#include "audio/chat_playback_reset.h"
#undef private
#include <algorithm>
#include <functional>
#include <memory>
#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <future>
#include <optional>
#include <mutex>

constexpr int OPUS_FRAME_DURATION_MS = 60;
constexpr int AS_EVENT_AUDIO_PROCESSOR_RUNNING = 4, MAIN_EVENT_CHAT_OUTBOUND = 8192;
void xEventGroupSetBits(int, int) {}
void xEventGroupClearBits(int, int) {}
void xTaskNotifyGive(void*) {}
void* xTaskGetCurrentTaskHandle() { return nullptr; }
static uint64_t now_us = 100;
uint64_t esp_timer_get_time() { return now_us; }
static unsigned reboots = 0;
void esp_restart() { ++reboots; }
#define ESP_LOGE(...) ((void)0)
static std::atomic<unsigned> resets{0};
static bool throw_reset = false;
constexpr int ESP_AUDIO_ERR_OK = 0;
int esp_opus_dec_reset(void*) { if (throw_reset) return -1; ++resets; return 0; }
std::function<void()> reset_entry_hook;
struct Packet { uint32_t chat_reset_token=0, generation=0, response_generation=0; };
constexpr int ESP_AE_ERR_OK=0;
int esp_ae_rate_cvt_reset(void*) { return ESP_AE_ERR_OK; }
struct Processor {
    bool initialized = false, running = false;
    bool ready = true;
    unsigned prepares = 0;
    void Initialize(void*, int, void*) { initialized = true; }
    bool IsCaptureReady() const { return initialized && ready; }
    void PrepareCapture(ChatCaptureTag) { assert(initialized); ++prepares; }
    void Start() { assert(initialized); running = true; }
    void Stop() { running = false; }
};
struct AudioService {
    ChatUplinkAuthorization chat_uplink_authorization_;
    ChatPlaybackReset chat_playback_reset_;
    std::atomic<uint32_t> playback_generation_{0};
    std::mutex chat_prepare_mutex_, input_resampler_mutex_, audio_queue_mutex_, decoder_mutex_;
    std::mutex chat_decode_transition_mutex_;
    std::condition_variable audio_queue_cv_;
    AudioDecodeFence audio_decode_fence_;
    AudioResetEpochPublication audio_reset_epoch_publication_;
    Processor processor;
    Processor* audio_processor_ = &processor;
    bool audio_processor_initialized_ = false;
    bool chat_processor_initialization_attempted_ = false;
    uint32_t chat_prepared_revoked_ = 0;
    bool chat_prepared_scope_ = false;
    void* input_resampler_ = nullptr;
    void* output_resampler_ = nullptr;
    void* opus_decoder_ = reinterpret_cast<void*>(1);
    void* codec_ = nullptr;
    void* models_list_ = nullptr;
    int event_group_ = 0;
    std::deque<std::unique_ptr<Packet>> audio_decode_queue_, audio_playback_queue_;
    bool wake = false;
    unsigned wake_policy_queries = 0;
    bool IsAfeWakeWord() { ++wake_policy_queries;return true; }
    std::string GetLastWakeWord() { return "hello"; }
    unsigned cue_attempts = 0;
    int TryPlayChatCue(std::string_view,uint32_t generation,uint32_t reset,uint64_t deadline) {
        ++cue_attempts;
        if (now_us>=deadline) return 3;
        return generation != playback_generation_.load() || !chat_playback_reset_.Current(reset) ? 2 : 1;
    }
    bool wake_failure = false;
    void EnableWakeWordDetection(bool enabled) { if (!wake_failure) wake = enabled; }
    bool IsWakeWordRunning() { return wake; }
    unsigned stops = 0;
    void Stop() { ++stops; }
    uint32_t RevokeChatUplink() { return chat_uplink_authorization_.Revoke(); }
    void SetPlaybackGeneration(uint32_t value) { playback_generation_.store(value); }
    bool PrepareChatUplink(uint32_t, bool);
    bool PrepareChatAudioTransition(uint32_t, bool, bool, bool);
    uint32_t RequestChatPlaybackReset();
    uint32_t ChatPlaybackResetToken() const { return chat_playback_reset_.Requested(); }
    bool ResetChatDecoder(uint32_t);
};
struct Application {
    enum class ChatWakePolicy { Explicit, Listening };
    struct ChatAudioCleanup {
        uint32_t revoked = 0;
        uint32_t reset_serial = 0;
        bool processing = false, wake = false, chat_scope = true;
        bool reset = false, prepared = false, reset_done = false, stop_service = false;
        bool playback_only = false;
        ChatWakePolicy wake_policy = ChatWakePolicy::Explicit;
        bool read_wake = false;
        uint32_t wake_serial = 0;
        std::string wake_text;
        bool cue = false;
        uint32_t cue_serial = 0, cue_generation = 0;
        uint64_t cue_deadline_us = 0;
        std::string_view cue_sound;
        std::shared_ptr<const std::string> cue_owner;
        int cue_result = 1;
    };
    AudioService audio_service_;
    ChatAudioCleanup chat_audio_desired_{}, chat_audio_work_{};
    std::atomic<uint32_t> chat_audio_state_{0};
    uint32_t chat_audio_reset_serial_ = 0, chat_audio_reset_completed_ = 0;
    uint32_t chat_audio_prepared_ = 0;
    uint32_t chat_audio_completed_revoked_ = 0;
    uint32_t chat_playback_desired_ = 0, chat_playback_attempted_ = 0;
    bool chat_playback_fault_ = false;
    bool chat_wake_read_pending_ = false;
    uint32_t chat_wake_read_serial_ = 0;
    std::optional<std::string> chat_wake_read_result_;
    std::string_view chat_cue_sound_;
    std::shared_ptr<const std::string> chat_cue_owner_;
    std::mutex chat_cue_mutex_;
    uint32_t chat_cue_serial_=0,chat_cue_generation_=0,chat_cue_reset_=0;
    uint64_t chat_cue_deadline_us_=0;
    bool chat_cue_pending_=false,chat_cue_retry_=false;
    int chat_cue_result_=1;
    void* chat_audio_task_ = reinterpret_cast<void*>(1);
    int event_group_ = 0;
    void* application_task_ = nullptr;
    bool chat_audio_fault_ = false, chat_audio_exhausted_ = false;
    bool chat_reboot_audio_requested_ = false;
    uint64_t chat_reboot_deadline_us_ = 0;
    std::atomic<uint32_t> speaking_generation_{0};
    uint32_t RequestChatAudioCleanup(uint32_t, bool, bool, bool, bool = true, bool = false, ChatWakePolicy=ChatWakePolicy::Explicit);
    uint32_t RequestChatPlaybackCleanup(uint32_t);
    void PollChatAudioCleanup();
    bool RequestChatCue(std::string_view);
    void RunChatAudioCleanup();
    void RetryChatAudioCleanup();
    void BeginChatRebootAudioCleanup();
    void PollChatReboot();
};

// PRODUCTION_METHODS

int main() {
    {
        Application delayed;
        delayed.RequestChatAudioCleanup(1,true,false,false);
        assert(delayed.RequestChatCue("late"));
        Application contended;
        contended.chat_cue_mutex_.lock();
        auto rejected=std::async(std::launch::async,[&]{return contended.RequestChatCue("contended");});
        assert(rejected.wait_for(std::chrono::seconds(1))==std::future_status::ready);
        assert(!rejected.get());contended.chat_cue_mutex_.unlock();
        const auto deadline=delayed.chat_cue_deadline_us_;
        std::promise<void> entered;
        reset_entry_hook=[&]{entered.set_value();};
        delayed.audio_service_.decoder_mutex_.lock();
        auto resetting=std::async(std::launch::async,[&]{delayed.RunChatAudioCleanup();});
        entered.get_future().wait();
        now_us=deadline;
        delayed.audio_service_.decoder_mutex_.unlock();resetting.get();reset_entry_hook={};
        delayed.PollChatAudioCleanup();
        assert(!delayed.chat_cue_pending_ && delayed.chat_cue_result_==3);
        assert(delayed.audio_service_.cue_attempts==0);now_us=100;resets=0;
    }
    {
        Application queued;
        assert(queued.RequestChatCue("queued"));
        assert(queued.chat_audio_work_.cue_deadline_us==queued.chat_cue_deadline_us_);
        now_us=queued.chat_cue_deadline_us_;
        queued.RunChatAudioCleanup();queued.PollChatAudioCleanup();
        assert(!queued.chat_cue_pending_ && queued.chat_cue_result_==3);now_us=100;
    }
    {
        Application busy_cue;
        busy_cue.speaking_generation_=1;
        busy_cue.RequestChatAudioCleanup(1,true,false,false);
        busy_cue.RunChatAudioCleanup();busy_cue.PollChatAudioCleanup();
        busy_cue.RequestChatCue("popup");busy_cue.RunChatAudioCleanup();busy_cue.PollChatAudioCleanup();
        assert(busy_cue.chat_cue_pending_ && busy_cue.chat_cue_retry_);
        const auto serial=busy_cue.chat_cue_serial_;const auto deadline=busy_cue.chat_cue_deadline_us_;
        assert(!busy_cue.RequestChatCue("replacement"));
        assert(busy_cue.chat_cue_sound_=="popup" && busy_cue.chat_cue_serial_==serial && busy_cue.chat_cue_deadline_us_==deadline);
        auto attempts=busy_cue.audio_service_.cue_attempts;
        busy_cue.speaking_generation_=2;
        busy_cue.RequestChatAudioCleanup(2,true,false,false);
        assert(!busy_cue.chat_audio_work_.cue);
        busy_cue.RunChatAudioCleanup();busy_cue.PollChatAudioCleanup();
        assert(busy_cue.chat_audio_reset_completed_==busy_cue.chat_audio_reset_serial_);
        assert(busy_cue.audio_service_.cue_attempts==attempts);
        busy_cue.RetryChatAudioCleanup();busy_cue.RunChatAudioCleanup();busy_cue.PollChatAudioCleanup();
        assert(!busy_cue.chat_cue_pending_ && busy_cue.chat_cue_result_==2);
        resets=0;
    }
    {
        Application listening;
        auto revoked = listening.RequestChatAudioCleanup(1, false, true, false, true, false,
            Application::ChatWakePolicy::Listening);
        assert(listening.audio_service_.wake_policy_queries == 0);
        listening.RunChatAudioCleanup();listening.PollChatAudioCleanup();
#ifdef CONFIG_WAKE_WORD_DETECTION_IN_LISTENING
        assert(listening.audio_service_.wake && listening.audio_service_.wake_policy_queries == 1);
#else
        assert(!listening.audio_service_.wake && listening.audio_service_.wake_policy_queries == 0);
#endif
        assert(listening.chat_audio_prepared_ == revoked);
        assert(listening.audio_service_.chat_uplink_authorization_.Arm(revoked));
        assert(resets == 0);
    }
    {
        Application pending;
        auto revoked = pending.RequestChatAudioCleanup(1, false, true, false);
        pending.RunChatAudioCleanup();
        assert(pending.chat_audio_state_ == 2 && pending.chat_audio_prepared_ == 0);
        auto reset = pending.RequestChatPlaybackCleanup(2);
        assert(pending.chat_audio_prepared_ == 0);
        pending.RunChatAudioCleanup(); pending.PollChatAudioCleanup();
        assert(pending.chat_audio_reset_completed_ == reset);
        assert(pending.chat_audio_prepared_ == revoked);
        assert(pending.audio_service_.chat_uplink_authorization_.Arm(revoked));
        resets = 0;
    }
    {
        Application playback;
        auto revoked = playback.RequestChatAudioCleanup(1, false, true, false);
        playback.RunChatAudioCleanup(); playback.PollChatAudioCleanup();
        assert(playback.audio_service_.chat_uplink_authorization_.Arm(revoked));
        auto prepares = playback.audio_service_.processor.prepares;
        playback.RequestChatPlaybackCleanup(2);
        assert(playback.audio_service_.chat_uplink_authorization_.Capture().chat_scope);
        playback.RunChatAudioCleanup(); playback.PollChatAudioCleanup();
        assert(playback.audio_service_.processor.prepares == prepares);
        assert(playback.audio_service_.processor.running);
        assert(playback.audio_service_.chat_uplink_authorization_.Capture().chat_scope);
        resets = 0;
    }
    Application app;
    app.audio_service_.audio_playback_queue_.push_back(std::make_unique<Packet>());
    // No decoder reset on clean normal-drain preparation.
    auto first = app.RequestChatAudioCleanup(3, false, true, false);
    assert(!app.audio_service_.chat_uplink_authorization_.Arm(first));
    app.RunChatAudioCleanup();
    app.PollChatAudioCleanup();
    assert(app.chat_audio_prepared_ == first);
    assert(resets == 0 && app.audio_service_.audio_playback_queue_.size() == 1);
    assert(app.audio_service_.processor.running);
    assert(app.audio_service_.chat_uplink_authorization_.Arm(first));

    // Hold the actual decoder mutex; app invalidation and admission never wait.
    std::unique_lock<std::mutex> decoder(app.audio_service_.decoder_mutex_);
    auto second = app.RequestChatAudioCleanup(4, true, false, true);
    assert(app.audio_service_.playback_generation_ == 4);
    assert(!app.audio_service_.chat_uplink_authorization_.Accepts(
        ChatUplinkAuthorization::PreparedTag(first, true)));
    std::promise<void> reset_entered;
    reset_entry_hook=[&]{reset_entered.set_value();};
    auto worker = std::async(std::launch::async, [&] { app.RunChatAudioCleanup(); });
    reset_entered.get_future().wait();
    assert(app.audio_service_.chat_playback_reset_.Pending());
    assert(worker.wait_for(std::chrono::milliseconds(0)) == std::future_status::timeout);
    auto third_request = std::async(std::launch::async, [&] {
        return app.RequestChatAudioCleanup(5, true, true, false);
    });
    assert(third_request.wait_for(std::chrono::seconds(2)) == std::future_status::ready);
    auto third = third_request.get();
    app.PollChatAudioCleanup();
    assert(app.chat_audio_prepared_ == 0);
    assert(!app.audio_service_.chat_uplink_authorization_.Arm(second));
    assert(!app.audio_service_.chat_uplink_authorization_.Arm(third));
    decoder.unlock();
    worker.get();
    reset_entry_hook={};
    app.PollChatAudioCleanup();
    assert(app.chat_audio_prepared_ == 0);
    app.RunChatAudioCleanup();
    app.PollChatAudioCleanup();
    assert(resets == 1);
    assert(app.chat_audio_prepared_ == third);
    assert(app.audio_service_.chat_uplink_authorization_.Arm(third));
    app.PollChatAudioCleanup();
    app.RunChatAudioCleanup();
    assert(resets == 1);

    // A fresh legacy scope is prepared and armed, never era zero.
    auto legacy = app.RequestChatAudioCleanup(5, false, true, true, false);
    app.RunChatAudioCleanup();
    app.PollChatAudioCleanup();
    assert(app.audio_service_.chat_uplink_authorization_.Arm(legacy, false));
    assert(!app.audio_service_.chat_uplink_authorization_.IsCurrentChat(
        app.audio_service_.chat_uplink_authorization_.Capture()));

    Application unavailable;
    unavailable.chat_audio_task_ = nullptr;
    auto revoked = unavailable.RequestChatAudioCleanup(9, true, false, true);
    assert(unavailable.chat_audio_fault_);
    assert(unavailable.chat_audio_desired_.revoked == revoked);
    assert(unavailable.chat_audio_state_ == 0 && resets == 1);
    unavailable.chat_audio_task_ = reinterpret_cast<void*>(1);
    unavailable.PollChatAudioCleanup();
    unavailable.RunChatAudioCleanup();
    unavailable.PollChatAudioCleanup();
    assert(resets == 2);

    Application exhausted;
    exhausted.chat_audio_reset_serial_ = UINT32_MAX;
    exhausted.chat_audio_reset_completed_ = UINT32_MAX;
    exhausted.audio_service_.chat_playback_reset_.requested_.store(UINT32_MAX);
    auto exhausted_token = exhausted.RequestChatAudioCleanup(10, true, true, true);
    exhausted.RunChatAudioCleanup();
    exhausted.PollChatAudioCleanup();
    assert(exhausted.chat_audio_fault_ && exhausted.chat_audio_prepared_ == 0);
    assert(!exhausted.audio_service_.chat_uplink_authorization_.Arm(exhausted_token));

    Application failure;
    throw_reset = true;
    failure.RequestChatAudioCleanup(11, true, true, true);
    failure.RunChatAudioCleanup();
    failure.PollChatAudioCleanup();
    assert(failure.chat_audio_fault_ && failure.chat_audio_prepared_ == 0);
    assert(failure.chat_audio_reset_completed_ == 0);
    throw_reset = false;
    failure.RetryChatAudioCleanup();
    failure.RunChatAudioCleanup();
    failure.PollChatAudioCleanup();
    assert(failure.chat_audio_reset_completed_ == 1 && !failure.chat_audio_fault_);
    const auto reset_count = resets.load();
    failure.RetryChatAudioCleanup(); failure.RunChatAudioCleanup();
    assert(resets == reset_count);

    Application no_processor;
    no_processor.audio_service_.processor.ready = false;
    auto no_ready = no_processor.RequestChatAudioCleanup(12, false, true, false);
    no_processor.RunChatAudioCleanup();
    no_processor.PollChatAudioCleanup();
    assert(no_processor.chat_audio_fault_ && no_processor.chat_audio_prepared_ == 0);
    assert(!no_processor.audio_service_.audio_processor_initialized_);
    assert(!no_processor.audio_service_.processor.running);
    assert(!no_processor.audio_service_.chat_uplink_authorization_.Arm(no_ready));

    Application no_wake;
    no_wake.audio_service_.wake_failure = true;
    no_wake.RequestChatAudioCleanup(13, false, false, true);
    no_wake.RunChatAudioCleanup();
    no_wake.PollChatAudioCleanup();
    assert(no_wake.chat_audio_fault_);

    Application reboot;
    reboot.BeginChatRebootAudioCleanup();
    const auto stop_token = reboot.chat_audio_desired_.revoked;
    reboot.RequestChatAudioCleanup(90, false, true, false);
    assert(reboot.chat_audio_desired_.revoked == stop_token);
    reboot.PollChatReboot(); assert(reboots == 0);
    reboot.RunChatAudioCleanup(); reboot.PollChatAudioCleanup();
    assert(reboot.audio_service_.stops == 1 && reboots == 0);
    now_us += 999999; reboot.PollChatReboot(); assert(reboots == 0);
    ++now_us; reboot.PollChatReboot(); assert(reboots == 1);
    reboot.PollChatReboot(); assert(reboots == 1);
}

// TRANSPORT_FIXTURE
#include "protocol_work_lifetime.h"
#include "chat_protocol_signals.h"
#include "connect_close_deferral.h"
#include <atomic>
#include <cassert>
#include <condition_variable>
#include <future>
#include <memory>
#include <mutex>

#define ESP_LOGE(...) ((void)0)
constexpr int MAIN_EVENT_CHAT_OUTBOUND = 8192, pdTRUE = 1;
void xEventGroupSetBits(int, int) {}
struct Barrier {
    std::mutex mutex;
    std::condition_variable cv;
    bool entered = false, leave = false;
    void Block() {
        std::unique_lock<std::mutex> lock(mutex);
        entered = true; cv.notify_all(); cv.wait(lock, [&] { return leave; });
    }
    void Wait() {
        std::unique_lock<std::mutex> lock(mutex);
        assert(cv.wait_for(lock, std::chrono::seconds(2), [&] { return entered; }));
    }
    void Release() { std::lock_guard<std::mutex> lock(mutex); leave = true; cv.notify_all(); }
};
static int destroyed = 0, closes = 0;
struct Protocol {
    Barrier* close_barrier = nullptr;
    bool throw_close = false;
    ~Protocol() { ++destroyed; }
    void CloseAudioChannel() { if (throw_close) throw 1; ++closes; if (close_barrier) close_barrier->Block(); }
    void CompleteDeferredClose(uint32_t epoch) { assert(epoch == 7); CloseAudioChannel(); }
};
enum class NetworkWorkKind { kProtocolCleanup };
struct NetworkWorkItem { NetworkWorkKind kind; void* context; };
static void* open_channel_queue = reinterpret_cast<void*>(1);
static void* open_channel_task = reinterpret_cast<void*>(1);
static bool queue_accept = true;
static unsigned queued_count = 0;
int xQueueSend(void*, const NetworkWorkItem* work, int timeout) {
    assert(timeout == 0 && work->kind == NetworkWorkKind::kProtocolCleanup);
    if (queue_accept) ++queued_count;
    return queue_accept;
}
class Application {
public:
    struct ChatProtocolCleanup {
        std::unique_ptr<Protocol> protocol;
        ProtocolWorkLifetime::Action action = ProtocolWorkLifetime::Action::kNone;
        uint32_t epoch = 0;
        bool intentional = false, success = false, destructive_prepared = false;
    } chat_protocol_work_;
    std::atomic<uint32_t> chat_protocol_state_{0};
    std::atomic<bool> chat_protocol_owned_{false};
    std::atomic<uint32_t> lesson_protocol_readers_{0};
    bool chat_cleanup_enabled_ = true, chat_protocol_fault_ = false;
    bool chat_protocol_infrastructure_fault_ = false;
    std::shared_ptr<ChatProtocolSignals> chat_protocol_signals_;
    std::unique_ptr<Protocol> protocol_{new Protocol};
    ProtocolWorkLifetime protocol_work_lifetime_;
    ConnectCloseDeferral connect_close_deferral_;
    std::atomic<uint64_t> protocol_generation_{1};
    uint64_t deferred_close_generation_ = 0, protocol_start_pending_generation_ = 0;
    uint32_t deferred_close_epoch_ = 0;
    std::atomic<bool> reset_pending_{false}, protocol_reinit_pending_{false}, reboot_pending_{false};
    std::atomic<bool> connect_in_flight_{false};
    bool protocol_heap_monitor_pending_ = false, claim_protocol_completion_pending_ = false;
    enum class ProtocolActivation { kNone, kNormal };
    ProtocolActivation protocol_activation_pending_ = ProtocolActivation::kNone;
    int event_group_ = 0, initialized = 0, reboot_finishes = 0;
    bool chat_reboot_audio_requested_ = false;
    unsigned retires = 0, abandonments = 0, entrance_cancels = 0;
    void RetireChatOutbound() { ++retires; }
    void CancelConnectWatchdog() {}
    void RequestLessonStorageAbandonment() { ++abandonments; }
    void CancelLessonRobotEntranceOnDisplay() { ++entrance_cancels; }
    void InitializeProtocol() { assert(!protocol_ && !protocol_work_lifetime_.Pending()); ++initialized; }
    void CompleteProtocolActivation() {}
    void BeginChatRebootAudioCleanup() { ++reboot_finishes; chat_reboot_audio_requested_ = true; }
    void CompleteReboot() { assert(false); }
    void DoResetProtocol() { assert(false); }
    bool CompletePendingProtocolWork();
    bool PollChatProtocolCleanup();
    void RunChatProtocolCleanup();
};
namespace SystemInfo { unsigned monitor_stops = 0; void StopHeapPhaseMonitor() { ++monitor_stops; } }
// PRODUCTION_TRANSPORT_METHODS

int main() {
    using Action = ProtocolWorkLifetime::Action;
    Application app;
    auto lease = app.protocol_work_lifetime_.Reserve();
    app.protocol_work_lifetime_.Request(Action::kClose);
    app.connect_close_deferral_.Request(true);
    assert(app.CompletePendingProtocolWork());
    assert(app.protocol_ && queued_count == 0);
    app.protocol_work_lifetime_.Release(lease);
    queue_accept = false;
    assert(app.CompletePendingProtocolWork());
    assert(!app.protocol_ && app.chat_protocol_owned_);
    assert(app.protocol_work_lifetime_.Pending());
    assert(!app.protocol_work_lifetime_.Reserve());
    assert(app.chat_protocol_work_.protocol && queued_count == 0);
    app.protocol_work_lifetime_.Request(Action::kReinitialize);
    assert(app.PollChatProtocolCleanup());
    queue_accept = true;
    assert(app.PollChatProtocolCleanup());
    assert(queued_count == 1);
    assert(app.PollChatProtocolCleanup() && queued_count == 1);
    app.RunChatProtocolCleanup();
    assert(destroyed == 1 && app.initialized == 0);
    assert(app.PollChatProtocolCleanup());
    assert(app.initialized == 1 && !app.chat_protocol_owned_);
    assert(!app.PollChatProtocolCleanup());

    Application blocked;
    Barrier barrier;
    blocked.protocol_->close_barrier = &barrier;
    blocked.protocol_work_lifetime_.Request(Action::kClose);
    blocked.connect_close_deferral_.Request(true);
    blocked.PollChatProtocolCleanup();
    auto worker = std::async(std::launch::async, [&] { blocked.RunChatProtocolCleanup(); });
    barrier.Wait();
    blocked.protocol_work_lifetime_.Request(Action::kReboot);
    assert(blocked.PollChatProtocolCleanup());
    assert(blocked.reboot_finishes == 0 && destroyed == 1);
    barrier.Release(); worker.get();
    blocked.PollChatProtocolCleanup();
    assert(blocked.reboot_finishes == 0 && blocked.chat_protocol_work_.protocol);
    blocked.RunChatProtocolCleanup();
    assert(destroyed == 2);
    blocked.PollChatProtocolCleanup();
    assert(blocked.reboot_finishes == 1);
    assert(blocked.PollChatProtocolCleanup());
    assert(!blocked.protocol_work_lifetime_.Reserve());

    Application close;
    auto* original = close.protocol_.get();
    close.deferred_close_epoch_ = 7;
    close.deferred_close_generation_ = close.protocol_generation_.load();
    close.protocol_work_lifetime_.Request(Action::kClose);
    close.lesson_protocol_readers_=1;
    assert(close.PollChatProtocolCleanup());
    assert(close.protocol_.get()==original && close.chat_protocol_owned_ && close.chat_protocol_state_==0);
    close.lesson_protocol_readers_=0;
    close.PollChatProtocolCleanup(); close.RunChatProtocolCleanup(); close.PollChatProtocolCleanup();
    assert(close.protocol_.get() == original && !close.chat_protocol_owned_);
    assert(!close.protocol_work_lifetime_.Pending());

    Application failed;
    failed.protocol_->throw_close = true;
    failed.protocol_work_lifetime_.Request(Action::kClose);
    failed.PollChatProtocolCleanup(); failed.RunChatProtocolCleanup(); failed.PollChatProtocolCleanup();
    assert(failed.chat_protocol_fault_ && failed.chat_protocol_owned_);
    failed.protocol_work_lifetime_.Request(Action::kReinitialize);
    failed.PollChatProtocolCleanup(); failed.RunChatProtocolCleanup(); failed.PollChatProtocolCleanup();
    assert(failed.initialized == 1 && !failed.chat_protocol_owned_);

    for (int phase : {0, 1, 2}) {
        for (auto promoted : {Action::kReset, Action::kReinitialize, Action::kReboot}) {
            Application escalation;
            escalation.protocol_heap_monitor_pending_ = true;
            escalation.protocol_start_pending_generation_ = 42;
            const auto stops = SystemInfo::monitor_stops;
            escalation.protocol_->throw_close = phase == 2;
            queue_accept = phase != 0;
            escalation.protocol_work_lifetime_.Request(Action::kClose);
            escalation.PollChatProtocolCleanup();
            if (phase != 0) escalation.RunChatProtocolCleanup();
            assert(escalation.abandonments == 0);
            queue_accept = false;
            escalation.protocol_work_lifetime_.Request(promoted);
            escalation.PollChatProtocolCleanup();
            assert(escalation.abandonments == 1 && escalation.entrance_cancels == 1);
            assert(escalation.protocol_start_pending_generation_ == 0);
            assert(!escalation.protocol_heap_monitor_pending_);
            assert(SystemInfo::monitor_stops == stops + 1);
            escalation.PollChatProtocolCleanup();
            assert(escalation.abandonments == 1 && SystemInfo::monitor_stops == stops + 1);
            queue_accept = true;
            escalation.PollChatProtocolCleanup();
            escalation.RunChatProtocolCleanup();
            escalation.PollChatProtocolCleanup();
            assert(escalation.abandonments == 1 && escalation.entrance_cancels == 1);
        }
    }
}

// SIGNAL_FIXTURE
#include "chat_protocol_signals.h"
#include <cassert>
#include <future>

int main() {
    ChatProtocolSignals signal;
    const auto original = signal.Capture();
    std::promise<void> entered, release;
    auto released = release.get_future().share();
    auto old_callback = std::async(std::launch::async, [&] {
        const auto captured = signal.Capture();
        entered.set_value(); released.wait();
        assert(!signal.Publish(captured, ChatProtocolSignals::Closed));
    });
    entered.get_future().wait();
    signal.Disable();
    assert(!signal.Capture());
    assert(!signal.Publish(original, ChatProtocolSignals::Error));
    assert(signal.Enable());
    assert(signal.Capture() != original);
    assert(signal.Publish(signal.Capture(), ChatProtocolSignals::Error));
    release.set_value(); old_callback.get();
    uint32_t flags = 0;
    assert(signal.Collect(flags) && flags == ChatProtocolSignals::Error);
    assert(signal.Collect(flags) && flags == 0);
    assert(signal.Publish(signal.Capture(), ChatProtocolSignals::Closed));
    signal.Disable();
    assert(signal.Collect(flags) && flags == 0);
}
