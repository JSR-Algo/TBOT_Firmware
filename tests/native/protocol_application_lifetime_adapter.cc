#include "protocol_work_lifetime.h"
#include "chat_protocol_signals.h"
#include "connect_close_deferral.h"
#include <atomic>
#include <cassert>
#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <vector>
#include <thread>
#include <mutex>
#include <condition_variable>
using ChatRequestContext = std::shared_ptr<void>;

#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
#define MAIN_EVENT_ACTIVATION_DONE 1
#define MAIN_EVENT_ERROR 2
using ListeningMode = int;
using DeviceState = int;
constexpr int kDeviceStateConnecting = 1, kDeviceStateIdle = 2;
constexpr int kDeviceStateWifiConfiguring = 3, kDeviceStateAudioTesting = 4;
constexpr int kDeviceStateSpeaking = 5, kDeviceStateListening = 6;
constexpr int kListeningModeManualStop = 1;
constexpr int kWakeWordAudioChannelOpenMaxAttempts = 3, kWakeWordAudioChannelRetryDelayMs = 700;
constexpr int portMAX_DELAY = -1;
int pdMS_TO_TICKS(int n) { return n; }
void vTaskDelay(int) {}
void esp_timer_stop(void*) {}
uint64_t esp_timer_get_time() { return 100; }
static std::vector<std::string> effects;
static int restart_count = 0;
void esp_restart() { ++restart_count; effects.push_back("restart"); }
namespace Lang {
namespace Strings { const char* PLEASE_WAIT = "wait"; const char* TBOT_CONNECT = "connect"; const char* CONNECTED = "connected"; }
namespace Sounds { const char* OGG_SUCCESS = "success"; }
}
enum class PowerSaveLevel { PERFORMANCE };
struct Codec { int output_sample_rate() { return 24000; } };
struct Board {
 static Board& GetInstance() { static Board b; return b; }
 Board* GetDisplay() { return this; }
 void SetStatus(const char*) {}
 void SetPowerSaveLevel(PowerSaveLevel) {}
};
using TaskHandle_t = int;
static TaskHandle_t current_task = 1;
int xTaskGetCurrentTaskHandle() { return current_task; }
namespace SystemInfo {
static int monitors = 0;
void StartHeapPhaseMonitor() { ++monitors; }
void StopHeapPhaseMonitor() { --monitors; }
void PrintHeapCheckpoint(const char*) {}
}
static int activation_events = 0;
void xEventGroupSetBits(int, int) { ++activation_events; }
static int destroyed = 0;
struct Protocol {
 ~Protocol() { ++destroyed; effects.push_back("destroy"); }
 int closes = 0, deferred = 0;
 bool opened = false;
 bool open_success = true;
 std::function<void()> open_effect;
 std::function<void()> opened_callback;
 void OnAudioChannelOpened(std::function<void()> callback) { opened_callback = std::move(callback); }
 int server_sample_rate() { return 24000; }
 bool IsAudioChannelOpened() const { return opened; }
 bool OpenAudioChannel() { if (open_effect) open_effect(); opened = open_success; return open_success; }
 void SetIncomingJsonTransportEpoch(uint64_t) {}
 bool Start() { if (open_effect) open_effect(); return true; }
 void CloseAudioChannel() { ++closes; }
 void CompleteDeferredClose(uint32_t epoch) { deferred = epoch; }
};
enum class NetworkWorkKind { kOpenChannel, kHeartbeat, kProtocolCleanup };
struct NetworkWorkItem { NetworkWorkKind kind; void* context; };
static void* open_channel_queue = reinterpret_cast<void*>(1);
static void* open_channel_task = reinterpret_cast<void*>(1);
static bool queue_accept = true;
static bool reserved_at_send = false;
static NetworkWorkItem queued_work{};
static bool queued_ready = false;
struct WorkerStopped {};
int xQueueReceive(void*, NetworkWorkItem* work, int) {
 if (!queued_ready) throw WorkerStopped{};
 *work = queued_work; queued_ready = false; return 1;
}
constexpr int pdTRUE = 1;
class Application;
struct ConnectContext {
 Application* app;
 int mode = 0;
 uint32_t generation = 0;
 std::string wake_word;
 bool wake_word_invoke = false, passive_preconnect = false;
 Protocol* protocol = nullptr;
 uint64_t protocol_generation = 0, reservation = 0;
 bool start_protocol = false;
};
int xQueueSend(void*, const NetworkWorkItem* work, int) {
 auto* ctx = static_cast<ConnectContext*>(work->context);
 reserved_at_send = ctx->reservation != 0 && ctx->protocol != nullptr;
 if (queue_accept) { queued_work = *work; queued_ready = true; }
 return queue_accept ? pdTRUE : 0;
}
bool ProtocolLifetimeMatches(Protocol* current, Protocol* expected,
                             uint64_t generation, uint64_t expected_generation) {
 return current == expected && generation == expected_generation;
}
class Application {
public:
 struct ChatRecoveryIntent { enum class Kind { None };Kind kind=Kind::None;bool ready=false,opening=false,adopted=false;uint64_t retry_at_us=0,protocol_generation=0,lesson_generation=0;uint32_t connect_generation=0; } chat_recovery_;
 std::atomic<uint64_t> lesson_runtime_generation_{0};
 std::shared_ptr<ChatProtocolSignals> chat_protocol_signals_;
 void PollChatRecovery(uint64_t) { assert(false); }
 void CancelChatRecovery() {}
 bool CompleteChatRecoveryOpen(uint32_t,bool) { return false; }
    bool chat_cleanup_enabled_ = false;
    bool chat_protocol_infrastructure_fault_ = false;
 bool PollChatProtocolCleanup() { assert(false); return false; }
 void RunChatProtocolCleanup() { assert(false); }
 Application() { restart_count = 0; activation_events = 0; effects.clear(); }
 enum class ProtocolActivation { kNone, kNormal, kWifiReprovision };
 std::unique_ptr<Protocol> protocol_{new Protocol};
 ProtocolWorkLifetime protocol_work_lifetime_;
 ConnectCloseDeferral connect_close_deferral_;
 std::atomic<bool> connect_in_flight_{false}, reset_pending_{false}, protocol_reinit_pending_{false}, reboot_pending_{false};
 std::atomic<bool> lesson_runtime_active_{false};
 std::atomic<bool> chat_protocol_owned_{false};
 std::atomic<uint64_t> protocol_generation_{1};
 std::atomic<uint32_t> connect_generation_{1};
 std::atomic<uint32_t> protocol_callback_connect_generation_{0};
 std::atomic<bool> backend_offline_{true}, connect_attempt_active_{false}, reconnect_passive_{false};
 std::atomic<bool> lesson_interactive_listen_pending_{false}, lesson_interactive_listening_active_{false};
 std::atomic<bool> passive_ws_intent_{false}, lesson_asset_sync_quiet_{false}, online_intent_{false};
 std::atomic<bool> microphone_uplink_authorized_{false};
 void* reconnect_timer_ = nullptr;
 struct Recovery { void Reset() {} } backend_recovery_window_;
 std::atomic<bool> reconnect_resume_listening_{true}, lesson_idle_repaint_suppressed_{false};
 std::atomic<uint32_t> lesson_interactive_listen_generation_{0};
 int reconnect_attempt_ = 0, passive_reconnect_attempt_ = 0, successes = 0, heartbeats = 0;
 int reconnect_mode_ = 0;
 int state = kDeviceStateConnecting, retries = 0;
 std::string deferred_wake_word_;
 struct Epoch { uint64_t PublishedEpoch() { return 1; } } lesson_transport_epoch_gate_;
 struct Audio {
  bool start_ok = true;
  bool Start() { effects.push_back("audio_start"); return start_ok; }
  void Stop() { effects.push_back("audio_stop"); }
  void EnableWakeWordDetection(bool) { effects.push_back("wake"); }
 } audio_service_;
 ProtocolActivation protocol_activation_pending_ = ProtocolActivation::kNone;
 bool protocol_heap_monitor_pending_ = false, claim_protocol_completion_pending_ = false;
 uint64_t protocol_start_pending_generation_ = 0, deferred_close_generation_ = 0;
 uint32_t deferred_close_epoch_ = 0;
 int application_task_ = 1, event_group_ = 0;
 int publications = 0, watchdog_cancels = 0, claim_completions = 0;
 int& reboots = restart_count;
 bool initialize_mqtt = false;
 std::deque<std::function<void()>> tasks;
 std::mutex tasks_mutex;
 void Schedule(std::function<void()>&& task) { std::lock_guard<std::mutex> lock(tasks_mutex); tasks.push_back(std::move(task)); }
 void RunTasks() { while (!tasks.empty()) { auto task = std::move(tasks.front()); tasks.pop_front(); task(); } }
 void CancelConnectWatchdog() { ++watchdog_cancels; }
 void ArmConnectWatchdog() {}
 void RequestLessonStorageAbandonment() {}
 void CancelLessonRobotEntranceOnDisplay() {}
 void CloseAudioChannelByIntent();
 // This legacy fixture leaves the additive outbound path inactive; dedicated
 // outbound adapter tests exercise real retirement with a live worker lease.
 void RetireChatOutbound() {}
 void CompleteReboot();
 void InitializeProtocol() {
  assert(!protocol_work_lifetime_.Busy());
  ++publications;
  protocol_.reset(new Protocol);
  ++protocol_generation_;
  if (protocol_activation_pending_ == ProtocolActivation::kNormal) {
   SystemInfo::StartHeapPhaseMonitor();
   protocol_heap_monitor_pending_ = true;
  }
  if (initialize_mqtt) protocol_work_lifetime_.Reserve();
  else CompleteProtocolActivation();
 }
 void CompleteClaimProtocolActivation();
 void ScheduleClaimLocalAssetsRetry() { effects.push_back("claim_retry"); }
 void Alert(const char*, const char*, const char*, const char*) { effects.push_back("alert"); }
 int GetDefaultListeningMode() { return 0; }
 int GetDeviceState() { return state; }
 bool IsDeviceClaimed() { return true; }
 bool ShouldKeepManagementHeartbeat() { return true; }
 void StartHeartbeat() { ++heartbeats; effects.push_back("heartbeat"); }
 void StopHeartbeat() {}
 void StopClaimPoll() {}
 void DispatchDeviceHeartbeat() { effects.push_back("dispatch"); }
 void SetListeningMode(int) { ++successes; }
 void SetDeviceState(int next) { state = next; effects.push_back("idle"); }
 void FinishWakeWordInvoke(const std::string&) { ++successes; }
 void ScheduleLessonAssetSyncWakeRearm(uint64_t) {}
 void SchedulePassiveLessonReconnect() { ++retries; reconnect_passive_ = true; }
 void RearmClaimedIdleWakeWord() {}
 void ScheduleReconnect(int, bool) { ++retries; }
 static void HeartbeatTask(void*) {}
 static void OpenChannelTask(void*);
 void StartProtocolWorker();
 bool IsConnectSuccessPublicationSuppressed() const;
 void InstallOpenedCallback() {
  auto& board = Board::GetInstance();
  static Codec codec_instance;
  auto* codec = &codec_instance;
  const auto callback_protocol_generation = protocol_generation_.load();
  auto* callback_protocol = protocol_.get();
  const auto callback_signals = std::make_shared<ChatProtocolSignals>();
  // PRODUCTION_OPENED_CALLBACK
 }
 bool StartOpenChannelWorker(void*);
 void DoResetProtocol();
 bool CompletePendingProtocolWork();
 void RequestInitializeProtocol(ProtocolActivation activation = ProtocolActivation::kNone);
 void CompleteProtocolActivation();
 void ScheduleDeferredProtocolClose(Protocol*, uint32_t);
 bool IsChatRequestCurrent(const ChatRequestContext&) const { return true; }
 void Reboot(ChatRequestContext context = {});
 void ResetProtocol();
 void HandleConnectWatchdog(uint32_t);
 void ContinueOpenAudioChannel(ListeningMode);
 void StartPassiveLessonWebsocket();
 void HandleReconnectTick();
};

// PRODUCTION_METHODS

int main() {
 using Action = ProtocolWorkLifetime::Action;
 for (bool passive : {false, true}) {
  Application app;
  app.protocol_->open_success = false;
  if (passive) app.StartPassiveLessonWebsocket();
  else app.ContinueOpenAudioChannel(0);
  std::mutex barrier_mutex;
  std::condition_variable barrier;
  bool entered = false, leave = false;
  app.protocol_->open_effect = [&] {
   std::unique_lock<std::mutex> lock(barrier_mutex);
   entered = true; barrier.notify_all();
   barrier.wait(lock, [&] { return leave; });
  };
  std::thread worker([&] { try { Application::OpenChannelTask(&app); } catch (const WorkerStopped&) {} });
  { std::unique_lock<std::mutex> lock(barrier_mutex); barrier.wait(lock, [&] { return entered; }); }
  app.HandleConnectWatchdog(app.connect_generation_.load());
  const int retries_before = app.retries;
  app.HandleReconnectTick();
  assert(app.retries == retries_before + 1 && !queued_ready);
  { std::lock_guard<std::mutex> lock(barrier_mutex); leave = true; barrier.notify_all(); }
  worker.join(); app.RunTasks();
  app.protocol_->open_effect = nullptr;
  app.HandleReconnectTick();
  assert(queued_ready && app.protocol_work_lifetime_.Busy());
  try { Application::OpenChannelTask(&app); } catch (const WorkerStopped&) {}
  app.RunTasks();
 }
 {
  Application app;
  app.ContinueOpenAudioChannel(0);
  app.CloseAudioChannelByIntent();
  const int retries_before = app.retries;
  app.HandleReconnectTick();
  assert(app.retries == retries_before);
  try { Application::OpenChannelTask(&app); } catch (const WorkerStopped&) {}
  app.RunTasks();
 }
 {
  Application app;
  int starts = 0;
  const auto events_before = activation_events;
  app.protocol_->open_effect = [&] { ++starts; };
  app.protocol_activation_pending_ = Application::ProtocolActivation::kNormal;
  app.StartProtocolWorker();
  app.CloseAudioChannelByIntent();
  try { Application::OpenChannelTask(&app); } catch (const WorkerStopped&) {}
  app.RunTasks();
  assert(starts == 0 && activation_events == events_before);
  assert(app.protocol_start_pending_generation_ == app.protocol_generation_.load());
  app.StartProtocolWorker();
  try { Application::OpenChannelTask(&app); } catch (const WorkerStopped&) {}
  app.RunTasks();
  assert(starts == 1 && activation_events == events_before + 1);
 }
 for (bool lesson : {false, true}) {
  Application app;
  app.lesson_runtime_active_ = lesson;
  app.lesson_interactive_listen_pending_ = lesson;
  app.state = kDeviceStateConnecting;
  app.ContinueOpenAudioChannel(0);
  assert(queued_ready && app.connect_in_flight_ && app.protocol_work_lifetime_.Busy());
  app.HandleConnectWatchdog(app.connect_generation_.load());
  assert(!app.connect_in_flight_ && app.protocol_work_lifetime_.Busy());
  try { Application::OpenChannelTask(&app); } catch (const WorkerStopped&) {}
  app.RunTasks();
  assert(!app.protocol_work_lifetime_.Busy());
  app.lesson_interactive_listen_pending_ = lesson;
  app.state = kDeviceStateConnecting;
  app.ContinueOpenAudioChannel(0);
  assert(queued_ready && app.connect_in_flight_ && app.protocol_work_lifetime_.Busy());
  try { Application::OpenChannelTask(&app); } catch (const WorkerStopped&) {}
  app.RunTasks();
 }
 {
  Application app;
  auto token = app.protocol_work_lifetime_.Reserve();
  app.claim_protocol_completion_pending_ = true;
  app.RequestInitializeProtocol();
  assert(effects.empty());
  app.protocol_work_lifetime_.Release(token); app.CompletePendingProtocolWork();
  assert((effects == std::vector<std::string>{"destroy", "audio_start", "idle", "wake", "heartbeat", "dispatch", "alert"}));
  app.CompleteProtocolActivation();
  assert(app.heartbeats == 1); // A duplicate completion cannot repeat claim effects.
 }
 {
  Application app;
  app.audio_service_.start_ok = false;
  app.claim_protocol_completion_pending_ = true;
  app.CompleteProtocolActivation();
  assert((effects == std::vector<std::string>{"audio_start", "claim_retry"}));
  assert(app.heartbeats == 0 && !app.claim_protocol_completion_pending_);
 }
 {
  Application app;
  int opens = 0;
  app.protocol_->open_effect = [&] { ++opens; };
  auto* ctx = new ConnectContext{&app}; ctx->generation = app.connect_generation_.load();
  assert(app.StartOpenChannelWorker(ctx));
  app.RequestInitializeProtocol(); // Mutation wins before queued work starts.
  try { Application::OpenChannelTask(&app); } catch (const WorkerStopped&) {}
  app.RunTasks();
  assert(opens == 0 && app.publications == 1 && app.successes == 0);
 }
 {
  Application app;
  app.InstallOpenedCallback();
  app.protocol_callback_connect_generation_ = app.connect_generation_.load();
  app.protocol_->opened_callback();
  ++app.connect_generation_; // Expire after callback has queued its publication.
  app.RunTasks();
  assert(app.heartbeats == 0 && !app.online_intent_);
  app.protocol_callback_connect_generation_ = app.connect_generation_.load();
  app.protocol_->opened_callback();
  ++app.protocol_generation_; // Replacement fences the queued callback too.
  app.RunTasks();
  assert(app.heartbeats == 0);
  app.InstallOpenedCallback();
  app.protocol_->opened_callback(); app.RunTasks();
  assert(app.heartbeats == 1 && app.online_intent_);
 }
 {
  Application app;
  ConnectContext ctx{&app};
  queue_accept = false;
  assert(!app.StartOpenChannelWorker(&ctx));
  assert(reserved_at_send && !app.protocol_work_lifetime_.Busy());
  queue_accept = true;
  assert(app.StartOpenChannelWorker(&ctx));
  assert(ctx.protocol == app.protocol_.get() && ctx.protocol_generation == 1);
  app.connect_in_flight_ = false; // Expiry does not return the queued lease.
  app.ResetProtocol(); app.RunTasks();
  assert(app.protocol_ && !app.StartOpenChannelWorker(&ctx));
  app.protocol_work_lifetime_.Release(ctx.reservation);
  assert(app.CompletePendingProtocolWork());
  assert(!app.protocol_);
 }
 {
  Application app;
  auto first = app.protocol_work_lifetime_.Reserve();
  auto last = app.protocol_work_lifetime_.Reserve();
  current_task = 2;
  app.RequestInitializeProtocol(Application::ProtocolActivation::kNormal);
  assert(app.publications == 0 && activation_events == 0);
  current_task = 1; app.RunTasks();
  assert(app.publications == 0 && activation_events == 0);
  app.protocol_work_lifetime_.Release(first);
  assert(app.CompletePendingProtocolWork());
  assert(app.publications == 0);
  assert(!app.protocol_work_lifetime_.Release(first)); // Replayed callback.
  app.protocol_work_lifetime_.Release(last);
  assert(app.CompletePendingProtocolWork());
  assert(app.publications == 1 && activation_events == 1 && SystemInfo::monitors == 0);
 }
 {
  Application app;
  auto* ctx = new ConnectContext{&app};
  ctx->generation = app.connect_generation_.load();
  assert(app.StartOpenChannelWorker(ctx));
  auto* original = app.protocol_.get();
  std::mutex barrier_mutex;
  std::condition_variable barrier;
  bool entered = false, leave = false;
  original->open_effect = [&] {
   std::unique_lock<std::mutex> lock(barrier_mutex);
   entered = true; barrier.notify_all();
   barrier.wait(lock, [&] { return leave; });
  };
  std::thread worker([&] { try { Application::OpenChannelTask(&app); } catch (const WorkerStopped&) {} });
  { std::unique_lock<std::mutex> lock(barrier_mutex); barrier.wait(lock, [&] { return entered; }); }
  ++app.connect_generation_; app.connect_in_flight_ = false;
  app.Reboot(); app.RunTasks();
  assert(app.protocol_.get() == original && app.publications == 0);
  { std::lock_guard<std::mutex> lock(barrier_mutex); leave = true; barrier.notify_all(); }
  worker.join();
  assert(app.protocol_work_lifetime_.Busy()); // Completion still queued.
  app.RunTasks();
  assert(app.reboots == 1 && app.successes == 0 && !app.protocol_work_lifetime_.Busy());
  assert((effects == std::vector<std::string>{"destroy", "audio_stop", "restart"}));
 }
 {
  Application app;
  auto* ctx = new ConnectContext{&app}; ctx->generation = app.connect_generation_.load();
  assert(app.StartOpenChannelWorker(ctx));
  try { Application::OpenChannelTask(&app); } catch (const WorkerStopped&) {}
  ++app.connect_generation_; // New watchdog generation before old completion runs.
  app.connect_in_flight_ = true;
  app.RunTasks();
  assert(app.successes == 0 && app.watchdog_cancels == 0 && app.connect_in_flight_);
  ctx = new ConnectContext{&app}; ctx->generation = app.connect_generation_.load();
  assert(app.StartOpenChannelWorker(ctx));
  try { Application::OpenChannelTask(&app); } catch (const WorkerStopped&) {}
  auto duplicate = app.tasks.front();
  app.RunTasks(); duplicate();
  assert(app.successes == 1 && app.watchdog_cancels == 1);
 }
 {
  Application app;
  app.protocol_activation_pending_ = Application::ProtocolActivation::kNormal;
  app.protocol_heap_monitor_pending_ = true; SystemInfo::StartHeapPhaseMonitor();
  const int before = activation_events;
  queue_accept = false; app.StartProtocolWorker();
  assert(app.protocol_start_pending_generation_ == 1 && !app.protocol_work_lifetime_.Busy());
  queue_accept = true; app.StartProtocolWorker();
  assert(app.protocol_start_pending_generation_ == 0 && app.protocol_work_lifetime_.Busy());
  ++app.connect_generation_; // Rejected audio attempt cannot cancel control startup.
  try { Application::OpenChannelTask(&app); } catch (const WorkerStopped&) {}
  assert(activation_events == before && SystemInfo::monitors == 1);
  app.RunTasks();
  assert(activation_events == before + 1 && SystemInfo::monitors == 0);
 }
 {
  Application app;
  auto token = app.protocol_work_lifetime_.Reserve();
  std::mutex mutex;
  std::condition_variable cv;
  bool entered = false, leave = false;
  Protocol* captured = app.protocol_.get();
  std::thread worker([&] {
   std::unique_lock<std::mutex> lock(mutex);
   entered = true; cv.notify_all();
   cv.wait(lock, [&] { return leave; });
   assert(captured->closes == 0); // Last actual protocol access before release.
  });
  { std::unique_lock<std::mutex> lock(mutex); cv.wait(lock, [&] { return entered; }); }
  app.connect_in_flight_ = false;
  app.RequestInitializeProtocol();
  current_task = 2; app.Reboot();
  assert(app.reboots == 0);
  current_task = 1; app.RunTasks();
  assert(app.reboots == 0 && app.publications == 0 && app.protocol_.get() == captured);
  { std::lock_guard<std::mutex> lock(mutex); leave = true; cv.notify_all(); }
  worker.join();
  app.protocol_work_lifetime_.Release(token);
  app.CompletePendingProtocolWork();
  assert(app.reboots == 1 && app.publications == 0);
 }
 {
  Application app;
  auto token = app.protocol_work_lifetime_.Reserve();
  app.ScheduleDeferredProtocolClose(app.protocol_.get(), 9); app.RunTasks();
  assert(app.protocol_->deferred == 0);
  app.protocol_work_lifetime_.Release(token); app.CompletePendingProtocolWork();
  assert(app.protocol_->deferred == 9);
  auto* old = app.protocol_.get();
  app.ScheduleDeferredProtocolClose(old, 10);
  app.RequestInitializeProtocol(); app.RunTasks();
  assert(app.protocol_->deferred == 0); // Old generation cannot close replacement.
 }
 {
  Application app;
  auto token = app.protocol_work_lifetime_.Reserve();
  app.connect_close_deferral_.Request(true);
  app.protocol_work_lifetime_.Request(Action::kClose);
  assert(app.CompletePendingProtocolWork());
  assert(app.protocol_->closes == 0);
  app.protocol_work_lifetime_.Release(token); app.CompletePendingProtocolWork();
  assert(app.protocol_->closes == 1);
 }
}
