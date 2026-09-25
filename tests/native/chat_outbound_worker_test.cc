#include "chat_outbound_worker.h"
#include "protocol_work_lifetime.h"
#include "connect_close_deferral.h"
#include "protocol_lifetime_token.h"
#include <cassert>
#include <condition_variable>
#include <deque>
#include <future>
#include <iostream>
#include <thread>
using ChatRequestContext = std::shared_ptr<void>;
struct McpServer {
    static McpServer& GetInstance() { static McpServer server; return server; }
    void PollLessonAssetSyncCompletion() {}
};

static bool forbid_allocation = false;
void* operator new(size_t size) {
    if (forbid_allocation) throw std::bad_alloc();
    if (auto* result = std::malloc(size ? size : 1)) return result;
    throw std::bad_alloc();
}
void operator delete(void* pointer) noexcept { std::free(pointer); }

using Job = ChatOutboundMailbox::Job;
using Completion = ChatOutboundMailbox::Completion;
using Result = ChatOutboundMailbox::Result;
using Kind = ChatOutboundMailbox::Kind;
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
using DeviceState = int;
constexpr DeviceState kDeviceStateConnecting = 1, kDeviceStateIdle = 2;
namespace Lang { namespace Strings { const char* PLEASE_WAIT = "wait"; } }
struct Board {
    static Board& GetInstance() { static Board b; return b; }
    Board* GetDisplay() { return this; }
    void SetStatus(const char*) {}
};
namespace SystemInfo { void StopHeapPhaseMonitor() {} }
void CancelLessonRobotEntranceOnDisplay() {}
void vTaskDelay(int) {}
void esp_timer_stop(void*) {}
static int reboots = 0;
void esp_restart() { ++reboots; }
void Protocol::SetError(const std::string&) {}
bool Protocol::IsTimeout() const { return false; }
void Protocol::SendWakeWordDetected(const std::string&) {}
void Protocol::SendStartListening(ListeningMode) {}
void Protocol::SendStopListening() {}
void Protocol::SendAbortSpeaking(AbortReason) {}
void Protocol::SendMcpMessage(const std::string&) {}

struct Barrier {
    std::mutex mutex;
    std::condition_variable cv;
    bool entered = false, leave = false;
    void Block() {
        std::unique_lock<std::mutex> lock(mutex);
        entered = true; cv.notify_all();
        cv.wait(lock, [&] { return leave; });
    }
    void Wait() {
        std::unique_lock<std::mutex> lock(mutex);
        assert(cv.wait_for(lock, std::chrono::seconds(2), [&] { return entered; }));
    }
    void Release() { std::lock_guard<std::mutex> lock(mutex); leave = true; cv.notify_all(); }
};
struct TestProtocol : Protocol {
    Result SendChatFullTextIfCurrent(const Job& job, const std::function<bool()>& current) override {
        return SendChatControlIfCurrent(job, current);
    }
    Barrier* send_barrier = nullptr;
    Result result = Result::Sent;
    std::vector<int> sends;
    std::vector<uint64_t> deadlines;
    int closes = 0;
    uint32_t healthy_epoch = 7;
    uint32_t deferred = 0;
    void CompleteDeferredClose(uint32_t epoch) override { deferred = epoch; }
    bool Start() override { return true; }
    bool OpenAudioChannel() override { return true; }
    void CloseAudioChannel(bool = true) override { ++closes; }
    bool IsAudioChannelOpened() const override { return true; }
    uint32_t CurrentConnectionEpoch() const override { return healthy_epoch; }
    bool SendAudio(std::unique_ptr<AudioStreamPacket>) override { assert(false); return false; }
    bool SendText(const std::string&) override { return false; }
    Result SendChatControlIfCurrent(const Job& job, const std::function<bool()>& current) override {
        if (!current()) return Result::Stale;
        sends.push_back(static_cast<int>(job.kind)); deadlines.push_back(job.deadline_us);
        if (send_barrier) send_barrier->Block();
        return result;
    }
    Result SendChatAudioIfCurrent(const AudioStreamPacket& packet, uint32_t epoch,
                                  const std::function<bool(const ChatCaptureTag&)>& current) override {
        assert(epoch == 7);
        if (!current(packet.capture_tag)) return Result::Stale;
        sends.push_back(100);
        if (send_barrier) send_barrier->Block();
        return result;
    }
};

using TaskHandle_t = void*;
using StackType_t = unsigned char;
using StaticTask_t = int;
constexpr int tskIDLE_PRIORITY = 0, pdTRUE = 1, portMAX_DELAY = -1;
constexpr uint32_t kChatOutboundWorkerStackDepth = 8192;
static StackType_t chat_outbound_task_stack[kChatOutboundWorkerStackDepth];
static StaticTask_t chat_outbound_task_buffer;
static bool task_creation_success = true;
static int task_creations = 0, notifications = 0;
static std::atomic<uint64_t> now_us{100};
TaskHandle_t xTaskGetCurrentTaskHandle() { return reinterpret_cast<void*>(1); }
uint64_t esp_timer_get_time() { return now_us.load(); }
int pdMS_TO_TICKS(int n) { return n; }
void* xTaskCreateStatic(void (*)(void*), const char*, uint32_t, void*, int priority, StackType_t*, StaticTask_t*) {
    assert(priority == 3); ++task_creations;
    return task_creation_success ? reinterpret_cast<void*>(1) : nullptr;
}
void xTaskNotifyGive(void*) { ++notifications; }
struct WorkerStopped {};
static int worker_wait = 0;
void ulTaskNotifyTake(int clear, int wait) {
    assert(clear == pdTRUE); worker_wait = wait; throw WorkerStopped{};
}
void xEventGroupSetBits(int, int) {}
#define MAIN_EVENT_CHAT_OUTBOUND 8192
#define MAIN_EVENT_CLOCK_TICK 64

class Application;
enum class NetworkWorkKind { kOpenChannel, kHeartbeat };
struct NetworkWorkItem { NetworkWorkKind kind; void* context; };
struct ConnectContext {
    Application* app;
    ListeningMode mode = kListeningModeAutoStop;
    uint32_t generation = 0;
    std::string wake_word;
    bool wake_word_invoke = false, passive_preconnect = false;
    Protocol* protocol = nullptr;
    uint64_t protocol_generation = 0, reservation = 0;
    bool start_protocol = false;
};
struct HeartbeatContext { Application* app; std::string url, device_secret, body; };
static void* open_channel_queue = reinterpret_cast<void*>(1);
static void* open_channel_task = reinterpret_cast<void*>(1);
static bool queue_accept = true;
static NetworkWorkItem queued_work{};
int xQueueSend(void*, const NetworkWorkItem* work, int) {
    auto* context = static_cast<ConnectContext*>(work->context);
    assert(context->reservation && context->protocol);
    if (queue_accept) queued_work = *work;
    return queue_accept;
}

class Application {
public:
    void PollChatRecovery(uint64_t) { assert(false); }
    void CancelChatRecovery() {}
    struct RecoveryAdapter { bool opening=false,adopted=false;uint32_t connect_generation=0;uint64_t protocol_generation=0,lesson_generation=0; } chat_recovery_;
    std::atomic<uint64_t> lesson_runtime_generation_{0};
    bool chat_cleanup_enabled_ = false;
    bool chat_protocol_infrastructure_fault_ = false;
    uint32_t chat_playout_stamp_=0;
    bool chat_playout_recovery_=false;
    void PollChatPlayout(uint64_t) { assert(false); }
    void PollChatStart(uint64_t) {}
    struct Controls { size_t Size() const { return 0; } } chat_control_intents_;
    bool DeliverChatControl(const Completion&) { return false; }
    void PollChatControls(uint64_t) {}
    void PollChatUnpair(uint64_t) {}
    void PollChatSourceOpen(uint64_t) {}
    struct ConnectionMessages {
        size_t Size() const { return 0; }
        bool Deliver(const Completion&) { return false; }
        void ObserveRetirement(uint64_t) {}
    } chat_connection_messages_;
    void PollChatConnectionMessages(uint64_t) {}
    void PollChatInboundMessages() {}
    bool IsChatConnectionCurrent(ConnectionSource,uint64_t,uint32_t) const { return true; }
    void PollChatAudioCleanup() {}
    void PollChatLessonCapture(uint64_t) {}
    void PollChatProtocolSignals() {}
    void RetryChatAudioCleanup() {}
    void PollChatReboot() {}
    bool PollChatProtocolCleanup() { assert(false); return false; }
    struct Audio {
        std::deque<std::unique_ptr<AudioStreamPacket>> queue;
        Barrier* pop_barrier = nullptr;
        std::atomic<bool> current{true};
        std::unique_ptr<AudioStreamPacket> PopPacketFromSendQueue() {
            if (pop_barrier) pop_barrier->Block();
            if (queue.empty()) return nullptr;
            auto result = std::move(queue.front()); queue.pop_front(); return result;
        }
        bool IsCurrentChatUplink(const AudioStreamPacket& p) const { return current && p.capture_tag.chat_scope; }
        void Stop() {}
    } audio_service_;
    std::unique_ptr<TestProtocol> protocol_{new TestProtocol};
    std::atomic<uint64_t> protocol_generation_{1};
    ProtocolWorkLifetime protocol_work_lifetime_;
    ChatOutboundWorker chat_outbound_worker_;
    TaskHandle_t chat_outbound_task_ = nullptr;
    uint64_t chat_outbound_reservation_ = 0, chat_outbound_request_id_ = 0;
    uint64_t chat_outbound_protocol_generation_ = 0;
    uint64_t chat_outbound_last_admitted_id_ = 0;
    uint32_t chat_outbound_generation_ = 0, chat_outbound_connection_epoch_ = 0;
    bool chat_outbound_fault_ = false;
    int event_group_ = 0, clock_ticks_ = 0;
    bool InitializeChatOutboundWorker();
    Result ActivateChatOutbound(uint32_t);
    Result SubmitChatOutbound(Job&);
    void RetireChatOutbound();
    bool PollChatOutbound(Completion* = nullptr);
    bool IsChatOutboundCompletionCurrent(const Completion&) const;
    void NotifyChatOutbound();
    static void ChatOutboundTask(void*);
    void PollChatOutboundEvents(uint32_t bits);
    enum class ProtocolActivation { kNone, kNormal, kWifiReprovision };
    ProtocolActivation protocol_activation_pending_ = ProtocolActivation::kNone;
    ConnectCloseDeferral connect_close_deferral_;
    std::atomic<bool> connect_in_flight_{false}, reset_pending_{false}, protocol_reinit_pending_{false}, reboot_pending_{false};
    std::atomic<bool> lesson_runtime_active_{false}, passive_ws_intent_{false}, reconnect_passive_{false};
    std::atomic<bool> online_intent_{false}, microphone_uplink_authorized_{false}, connect_attempt_active_{false};
    std::atomic<bool> backend_offline_{false}, lesson_interactive_listen_pending_{false};
    std::atomic<bool> lesson_interactive_listening_active_{false}, lesson_idle_repaint_suppressed_{false};
    std::atomic<bool> reconnect_resume_listening_{false}, heartbeat_inflight_{false};
    std::atomic<uint32_t> connect_generation_{1}, lesson_interactive_listen_generation_{0};
    int reconnect_attempt_ = 0, passive_reconnect_attempt_ = 0;
    ListeningMode reconnect_mode_ = kListeningModeAutoStop;
    uint64_t protocol_start_pending_generation_ = 0, deferred_close_generation_ = 0;
    uint32_t deferred_close_epoch_ = 0;
    bool protocol_heap_monitor_pending_ = false, claim_protocol_completion_pending_ = false;
    TaskHandle_t application_task_ = reinterpret_cast<void*>(1);
    void* reconnect_timer_ = nullptr;
    std::string deferred_wake_word_;
    struct Recovery { void Reset() {} } backend_recovery_window_;
    std::mutex tasks_mutex;
    std::deque<std::function<void()>> tasks;
    Barrier* heartbeat_barrier = nullptr;
    int publications = 0, watchdog_cancels = 0;
    DeviceState state = kDeviceStateConnecting;
    void Schedule(std::function<void()>&& task) {
        std::lock_guard<std::mutex> lock(tasks_mutex); tasks.push_back(std::move(task));
    }
    void RunTasks() { while (!tasks.empty()) { auto fn = std::move(tasks.front()); tasks.pop_front(); fn(); } }
    void CancelConnectWatchdog() { ++watchdog_cancels; }
    void RequestLessonStorageAbandonment() {}
    void CompleteProtocolActivation() {}
    void InitializeProtocol() { assert(!protocol_work_lifetime_.Busy()); protocol_.reset(new TestProtocol); ++protocol_generation_; ++publications; }
    int GetDeviceState() const { return state; }
    void SetDeviceState(int next) { state = next; }
    void SchedulePassiveLessonReconnect() {}
    void RearmClaimedIdleWakeWord() {}
    void ScheduleReconnect(ListeningMode, bool) {}
    void HandleHeartbeatAuthFailure(int) {}
    int SendDeviceHeartbeat(const std::string&, const std::string&, std::string) { heartbeat_barrier->Block(); return 200; }
    static void HeartbeatTask(void*);
    bool CompletePendingProtocolWork();
    void RequestInitializeProtocol(ProtocolActivation activation = ProtocolActivation::kNone);
    void DoResetProtocol();
    bool IsChatRequestCurrent(const ChatRequestContext&) const { return true; }
    void Reboot(ChatRequestContext context = {});
    void CompleteReboot();
    void ResetProtocol();
    void CloseAudioChannelByIntent();
    void HandleConnectWatchdog(uint32_t);
    bool StartOpenChannelWorker(void*);
    void ScheduleDeferredProtocolClose(Protocol*, uint32_t);
    void Packet() {
        auto p = std::make_unique<AudioStreamPacket>(); p->capture_tag = {6, true};
        p->payload = {1, 2, 3}; audio_service_.queue.push_back(std::move(p));
    }
};

// PRODUCTION_METHODS

int main() {
    using namespace std::chrono_literals;
    {
        Application app; app.InitializeChatOutboundWorker(); app.ActivateChatOutbound(7);
        Job drain; drain.kind = Kind::DrainAck;
        assert(app.SubmitChatOutbound(drain) == Result::Failed);
        assert(drain.deadline_us == 0);
        assert(!app.chat_outbound_worker_.RunOnce(now_us));
        assert(app.protocol_->sends.empty());
        app.RetireChatOutbound(); app.chat_outbound_worker_.RunOnce(now_us); app.PollChatOutbound();
    }
    {
        Application app; app.InitializeChatOutboundWorker(); app.ActivateChatOutbound(7);
        Job first; first.deadline_us = 1000000;
        Job second; second.deadline_us = 1000000;
        Job retry; retry.kind = Kind::ListenStop; retry.deadline_us = 900000;
        assert(app.SubmitChatOutbound(first) == Result::Sent);
        assert(app.SubmitChatOutbound(second) == Result::Sent);
        assert(app.SubmitChatOutbound(retry) == Result::Busy);
        const auto request_id = retry.request_id;
        const auto deadline = retry.deadline_us;
        app.chat_outbound_worker_.RunOnce(now_us);
        Completion completion; assert(app.PollChatOutbound(&completion));
        assert(completion.job.request_id == first.request_id);
        assert(app.SubmitChatOutbound(retry) == Result::Sent);
        assert(retry.request_id == request_id && retry.deadline_us == deadline);
        app.chat_outbound_worker_.RunOnce(now_us); assert(app.PollChatOutbound(&completion));
        assert(completion.job.request_id == second.request_id);
        app.chat_outbound_worker_.RunOnce(now_us); assert(app.PollChatOutbound(&completion));
        assert(completion.job.request_id == request_id && completion.job.deadline_us == deadline);
        assert(completion.result == Result::Sent);
        app.RetireChatOutbound(); app.chat_outbound_worker_.RunOnce(now_us); app.PollChatOutbound();
    }
    {
        Application app; app.InitializeChatOutboundWorker(); app.ActivateChatOutbound(7);
        app.Packet();
        app.ScheduleDeferredProtocolClose(app.protocol_.get(), 7); app.RunTasks();
        assert(app.protocol_work_lifetime_.Pending());
        assert(!app.chat_outbound_worker_.IsCurrent(app.chat_outbound_worker_.activation_.generation));
        app.chat_outbound_worker_.RunOnce(now_us);
        assert(app.protocol_->sends.empty() && app.protocol_->deferred == 0);
        app.PollChatOutbound();
        assert(app.protocol_->deferred == 7 && !app.protocol_work_lifetime_.Busy());
    }
    {
        Application app; app.InitializeChatOutboundWorker();
        try { Application::ChatOutboundTask(&app); } catch (const WorkerStopped&) {}
        assert(worker_wait == portMAX_DELAY);
        app.ActivateChatOutbound(7);
        Job job; job.deadline_us = 1000000; app.SubmitChatOutbound(job);
        app.protocol_->result = Result::Busy;
        try { Application::ChatOutboundTask(&app); } catch (const WorkerStopped&) {}
        assert(worker_wait == 10 && app.protocol_->sends.size() == 1);
        app.RetireChatOutbound(); app.chat_outbound_worker_.RunOnce(now_us); app.PollChatOutbound();
    }
    {
        Application app; assert(app.InitializeChatOutboundWorker());
        forbid_allocation = true;
        assert(app.ActivateChatOutbound(7) == Result::Sent);
        Job job; assert(app.SubmitChatOutbound(job) == Result::Sent);
        app.RetireChatOutbound(); app.chat_outbound_worker_.RunOnce(now_us); app.PollChatOutbound();
        assert(!app.protocol_work_lifetime_.Busy());
        forbid_allocation = false;
    }
    {
        Application app; app.InitializeChatOutboundWorker();
        app.chat_outbound_worker_.mailbox_.generation_.store(ChatOutboundMailbox::kExhausted - 1);
        assert(app.ActivateChatOutbound(7) == Result::Failed);
        assert(!app.protocol_work_lifetime_.Busy());
        assert(app.ActivateChatOutbound(7) == Result::Failed);
    }
    {
        Application app; app.InitializeChatOutboundWorker(); app.ActivateChatOutbound(7);
        app.chat_outbound_request_id_ = UINT64_MAX;
        Job job; assert(app.SubmitChatOutbound(job) == Result::Failed);
        assert(app.protocol_work_lifetime_.Busy());
        app.chat_outbound_worker_.RunOnce(now_us); app.PollChatOutbound();
        assert(!app.protocol_work_lifetime_.Busy());
        assert(app.SubmitChatOutbound(job) == Result::Failed);
    }
    for (bool running : {false, true}) {
        for (int action = 0; action < 5; ++action) {
            Application app; assert(app.InitializeChatOutboundWorker());
            assert(app.ActivateChatOutbound(7) == Result::Sent);
            Job job; job.deadline_us = 1000000; assert(app.SubmitChatOutbound(job) == Result::Sent);
            auto* original = app.protocol_.get();
            auto original_generation = app.protocol_generation_.load();
            const int reboot_before = reboots;
            Barrier send; original->send_barrier = running ? &send : nullptr;
            std::thread worker;
            if (running) {
                worker = std::thread([&] { app.chat_outbound_worker_.RunOnce(now_us); });
                send.Wait();
            }
            app.connect_in_flight_ = true;
            app.HandleConnectWatchdog(app.connect_generation_.load());
            assert(!app.connect_in_flight_ && app.protocol_work_lifetime_.Busy());
            ConnectContext open{}; open.app = &app;
            if (action == 0) { app.ResetProtocol(); app.RunTasks(); }
            if (action == 1) app.RequestInitializeProtocol();
            if (action == 2) { app.Reboot(); app.RunTasks(); }
            if (action == 3) app.CloseAudioChannelByIntent();
            if (action == 4) assert(!app.StartOpenChannelWorker(&open));
            assert(app.protocol_.get() == original && original->closes == 0);
            assert(app.protocol_generation_ == original_generation);
            assert(app.protocol_work_lifetime_.Busy() && reboots == reboot_before);
            if (running) { send.Release(); worker.join(); }
            app.chat_outbound_worker_.RunOnce(now_us);
            assert(app.protocol_work_lifetime_.Busy());
            Completion completion;
            app.PollChatOutbound(&completion);
            assert(!app.protocol_work_lifetime_.Busy());
            assert(!app.IsChatOutboundCompletionCurrent(completion));
            if (action == 0 || action == 2) assert(!app.protocol_);
            if (action == 1) assert(app.publications == 1 && app.protocol_generation_ > original_generation);
            if (action == 2) assert(reboots == reboot_before + 1);
            if (action == 3) assert(app.protocol_->closes == 1);
            if (action == 4) {
                assert(app.StartOpenChannelWorker(&open));
                assert(open.protocol == original && open.reservation);
                assert(app.ActivateChatOutbound(7) == Result::Busy);
                assert(app.protocol_work_lifetime_.Release(open.reservation));
            }
        }
    }
    // Actual adapter admission creates exactly one persistent worker; no inline fallback.
    {
        Application app;
        task_creation_success = false;
        assert(!app.InitializeChatOutboundWorker());
        assert(app.ActivateChatOutbound(7) == Result::Failed);
        assert(!app.protocol_work_lifetime_.Busy());
        task_creation_success = true;
        assert(app.InitializeChatOutboundWorker());
        int before = task_creations;
        assert(app.InitializeChatOutboundWorker() && task_creations == before);
        assert(app.ActivateChatOutbound(0) == Result::Failed);
        auto a = app.protocol_work_lifetime_.Reserve();
        auto b = app.protocol_work_lifetime_.Reserve();
        auto c = app.protocol_work_lifetime_.Reserve();
        auto d = app.protocol_work_lifetime_.Reserve();
        assert(app.ActivateChatOutbound(7) == Result::Busy);
        for (auto token : {a, b, c, d}) assert(app.protocol_work_lifetime_.Release(token));
    }
    // Each real admission returns before transport and retirement remains bounded
    // while the worker is inside either audio send or any of the five controls.
    for (int which = -1; which < 5; ++which) {
        Application app;
        assert(app.InitializeChatOutboundWorker());
        assert(app.ActivateChatOutbound(7) == Result::Sent);
        assert(app.protocol_work_lifetime_.Busy());
        Barrier transport;
        app.protocol_->send_barrier = &transport;
        Job job; job.kind = static_cast<Kind>(which < 0 ? 0 : which); job.deadline_us = 1000000;
        if (which < 0) app.Packet();
        else assert(app.SubmitChatOutbound(job) == Result::Sent);
        std::thread worker([&] { app.chat_outbound_worker_.RunOnce(now_us); });
        transport.Wait();
        for (int tick = 0; tick < 10; ++tick) app.PollChatOutboundEvents(MAIN_EVENT_CLOCK_TICK);
        assert(app.clock_ticks_ == 10);
        app.CloseAudioChannelByIntent();
        Job rejected; rejected.deadline_us = 1000000;
        assert(app.SubmitChatOutbound(rejected) != Result::Sent);
        app.PollChatOutbound();
        assert(app.protocol_work_lifetime_.Busy() && app.protocol_->closes == 0);
        transport.Release(); worker.join();
        app.chat_outbound_worker_.RunOnce(now_us);
        assert(app.protocol_work_lifetime_.Busy()); // Last access is done, app has not collected.
        Completion completion;
        app.PollChatOutbound(&completion);
        assert(!app.protocol_work_lifetime_.Busy() && app.protocol_->closes == 1);
        if (which >= 0) assert(!app.IsChatOutboundCompletionCurrent(completion));
    }
    {
        Application app; app.InitializeChatOutboundWorker(); app.ActivateChatOutbound(7);
        app.Packet(); Barrier queue; app.audio_service_.pop_barrier = &queue;
        std::thread worker([&] { app.chat_outbound_worker_.RunOnce(now_us); });
        queue.Wait(); app.RetireChatOutbound(); app.PollChatOutbound();
        assert(app.protocol_work_lifetime_.Busy());
        queue.Release(); worker.join(); app.chat_outbound_worker_.RunOnce(now_us); app.PollChatOutbound();
        assert(!app.protocol_work_lifetime_.Busy() && app.protocol_->sends.empty());
    }
    {
        Application app; app.InitializeChatOutboundWorker(); app.ActivateChatOutbound(7); app.Packet();
        Barrier heartbeat; app.heartbeat_barrier = &heartbeat;
        auto* context = new HeartbeatContext{&app, "url", "secret", "body"};
        std::thread network([&] { Application::HeartbeatTask(context); }); heartbeat.Wait();
        assert(app.chat_outbound_worker_.RunOnce(now_us));
        assert(app.protocol_->sends == std::vector<int>{100});
        heartbeat.Release(); network.join(); app.RunTasks();
        app.RetireChatOutbound(); app.chat_outbound_worker_.RunOnce(now_us); app.PollChatOutbound();
    }
    {
        Application app; app.InitializeChatOutboundWorker(); app.ActivateChatOutbound(7); app.Packet();
        Job first; first.deadline_us = 1000;
        assert(app.SubmitChatOutbound(first) == Result::Sent);
        Job second; second.kind = Kind::ListenStop; second.deadline_us = 1000;
        assert(app.SubmitChatOutbound(second) == Result::Sent);
        Job full; full.deadline_us = 1000;
        assert(app.SubmitChatOutbound(full) == Result::Busy);
        app.protocol_->result = Result::Busy;
        app.chat_outbound_worker_.RunOnce(100);
        app.chat_outbound_worker_.RunOnce(200);
        assert(app.protocol_->deadlines == std::vector<uint64_t>({1000, 1000}));
        app.chat_outbound_worker_.RunOnce(1000);
        Completion completion;
        assert(app.PollChatOutbound(&completion) && completion.result == Result::Failed);
        app.protocol_->result = Result::Sent;
        app.chat_outbound_worker_.RunOnce(100);
        assert(!app.chat_outbound_worker_.RunOnce(100)); // Wait for collection notification.
        assert(app.protocol_->sends.back() == static_cast<int>(Kind::ListenStop));
        assert(app.PollChatOutbound(&completion));
        assert(app.IsChatOutboundCompletionCurrent(completion));
        app.protocol_->healthy_epoch = 0;
        assert(!app.IsChatOutboundCompletionCurrent(completion));
        app.protocol_->healthy_epoch = 8;
        assert(!app.IsChatOutboundCompletionCurrent(completion));
        app.protocol_->healthy_epoch = 7;
        assert(app.SubmitChatOutbound(second) == Result::Stale); // Accepted request cannot be replayed.
        app.RetireChatOutbound();
        assert(!app.IsChatOutboundCompletionCurrent(completion));
        app.chat_outbound_worker_.RunOnce(100); app.PollChatOutbound();
    }
    {
        Application app; app.InitializeChatOutboundWorker(); app.ActivateChatOutbound(7); app.Packet(); app.Packet();
        app.protocol_->result = Result::Failed;
        app.chat_outbound_worker_.RunOnce(now_us);
        assert(!app.chat_outbound_worker_.RunOnce(now_us));
        assert(app.protocol_->sends.size() == 1);
        app.PollChatOutbound(); assert(app.chat_outbound_fault_);
        app.chat_outbound_worker_.RunOnce(now_us); app.PollChatOutbound();
        assert(!app.protocol_work_lifetime_.Busy());
        assert(app.ActivateChatOutbound(7) == Result::Sent);
        assert(!app.chat_outbound_fault_);
        app.RetireChatOutbound(); app.chat_outbound_worker_.RunOnce(now_us); app.PollChatOutbound();
    }
    {
        ChatOutboundMailbox mailbox;
        Job job; job.request_id = 1; job.protocol_generation = 1; job.connection_epoch = 1;
        job.generation = mailbox.AdvanceGeneration(); job.deadline_us = 500;
        assert(mailbox.TrySubmit(job));
        Job active; assert(mailbox.TryTake(active));
        ++active.deadline_us;
        assert(!mailbox.TryComplete(active, Result::Sent));
        assert(mailbox.TryComplete(job, Result::Sent));
    }
    for (auto result : {Result::Sent, Result::Failed}) {
        Application app; app.InitializeChatOutboundWorker(); app.ActivateChatOutbound(7);
        Job job; job.deadline_us = 1000000; app.SubmitChatOutbound(job);
        Barrier send; app.protocol_->send_barrier = &send; app.protocol_->result = result;
        std::thread worker([&] { app.chat_outbound_worker_.RunOnce(now_us); }); send.Wait();
        std::unique_lock<std::mutex> lock(app.chat_outbound_worker_.mailbox_.mutex_);
        send.Release(); worker.join();
        app.RetireChatOutbound(); lock.unlock();
        app.chat_outbound_worker_.RunOnce(now_us);
        Completion completion; assert(app.PollChatOutbound(&completion));
        assert(completion.stale && completion.result == result);
        assert(!app.protocol_work_lifetime_.Busy());
    }
    std::cout << "worker_object_bytes=" << sizeof(ChatOutboundWorker) << "\n";
}
